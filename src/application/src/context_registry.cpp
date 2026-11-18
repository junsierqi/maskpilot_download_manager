#include "context_registry.h"
#include <maskpilot/domain/proxy.h>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace maskpilot {
namespace {
constexpr std::size_t registryLimit = 4096;
std::string canonicalRoute(const std::string& value) {
    const auto endpoint = parseProxyEndpoint(value);
    if (endpoint.type == ProxyType::Direct)
        return {};
    const auto scheme = endpoint.type == ProxyType::Http    ? "http://"
                        : endpoint.type == ProxyType::Https ? "https://"
                                                            : "socks5h://";
    const auto host =
        endpoint.host.find(':') == std::string::npos ? endpoint.host : "[" + endpoint.host + "]";
    return scheme + host + ":" + std::to_string(endpoint.port);
}
std::filesystem::path nativePath(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
bool hasCredentials(const ProxyCredentials& value) {
    return !value.username.empty() || !value.password.empty();
}
void validateCredentials(const std::string& route, const ProxyCredentials& credentials, bool required) {
    DownloadSpec spec;
    spec.proxy = route;
    spec.proxyUsername = credentials.username;
    spec.proxyPassword = credentials.password;
    spec.proxyAuthenticationRequired = required;
    if (const auto error = validateProxy(spec, true); !error.empty())
        throw std::invalid_argument(error);
}
[[noreturn]] void denied() {
    throw std::invalid_argument("Download context authorization is invalid or expired.");
}
} // namespace

struct ContextRegistry::Impl {
    struct Session {
        std::string owner;
        std::int64_t revision, expiresAt;
    };
    struct Grant {
        std::string owner, url, cookie, authorization;
        std::int64_t expiresAt;
    };
    struct MemoryCredential {
        std::string route;
        ProxyCredentials credentials;
    };
    std::unique_ptr<IContextRepository> repository;
    std::unique_ptr<ICredentialVault> vault;
    std::map<std::string, DownloadContext> contexts;
    std::map<std::string, CredentialRecord> records;
    std::map<std::pair<std::string, std::int64_t>, MemoryCredential> memory;
    std::map<std::string, Session> sessions;
    std::map<std::string, Grant> grants;
    std::set<std::string> pendingCleanup;
    std::set<std::string> pendingContextCleanup;

    void requireRepository() const {
        if (!repository)
            throw std::runtime_error("Download context storage is unavailable.");
    }
    const DownloadContext& context(const std::string& owner) const {
        const auto found = contexts.find(owner);
        if (!isOpaqueId(owner) || found == contexts.end())
            throw std::invalid_argument("Download context was not found.");
        return found->second;
    }
    bool enabled(const std::string& owner) const {
        if (owner == "standalone")
            return true;
        const auto found = contexts.find(owner);
        return isOpaqueId(owner) && found != contexts.end() && found->second.enabled;
    }
    void invalidateSessions(const std::string& owner) {
        std::erase_if(sessions, [&](const auto& value) { return value.second.owner == owner; });
    }
    const Grant& grant(const std::string& id, const std::string& owner, const std::string& url) const {
        const auto found = grants.find(id);
        if (!isOpaqueId(id) || found == grants.end() || found->second.owner != owner ||
            found->second.url != url || found->second.expiresAt <= unixTime())
            throw std::invalid_argument(
                "Origin authentication is missing or expired. Issue a new URL-specific grant.");
        return found->second;
    }
    void cleanup(const std::string& reference) {
        const auto found = records.find(reference);
        if (found == records.end())
            return;
        try {
            if (!vault)
                throw std::runtime_error("Native credential storage is unavailable.");
            const auto& record = found->second;
            vault->forget({record.ownerId, record.proxyEndpoint, record.ownershipNonce}, reference);
            repository->eraseCredentialRecord(reference);
            records.erase(found);
            pendingCleanup.erase(reference);
        } catch (...) {
            pendingCleanup.insert(reference);
            throw;
        }
    }
    void clearRevokedReference(const std::string& owner) {
        const auto found = contexts.find(owner);
        if (found == contexts.end() || found->second.enabled)
            return;
        if (found->second.proxyCredentialRef.empty()) {
            pendingContextCleanup.erase(owner);
            return;
        }
        if (std::any_of(records.begin(), records.end(),
                        [&](const auto& entry) { return entry.second.ownerId == owner; }))
            return; // A failed exact native deletion must remain visible and retryable.
        try {
            auto cleared = found->second;
            if (cleared.revision == std::numeric_limits<std::int64_t>::max())
                throw std::runtime_error("Context revision limit reached.");
            cleared.proxyCredentialRef.clear();
            ++cleared.revision;
            repository->saveContext(cleared);
            found->second = std::move(cleared);
            pendingContextCleanup.erase(owner);
        } catch (...) {
            pendingContextCleanup.insert(owner);
            throw std::runtime_error(
                "Context is revoked, but its saved credential status could not be updated. Retry cleanup.");
        }
    }
    std::string store(const std::string& owner, const std::string& endpoint,
                      const ProxyCredentials& credentials) {
        requireRepository();
        if (!vault)
            throw std::runtime_error("Native credential storage is unavailable.");
        validateCredentials(endpoint, credentials, true);
        std::string reference;
        do {
            reference = newTaskId();
        } while (records.contains(reference));
        CredentialRecord record{reference, owner, canonicalRoute(endpoint), newTaskId()};
        repository->saveCredentialRecord(record);
        records.emplace(record.reference, record);
        try {
            vault->store(record.reference, {owner, record.proxyEndpoint, record.ownershipNonce}, credentials);
        } catch (const CredentialVaultError& error) {
            if (error.failure() == CredentialVaultFailure::AlreadyExists) {
                // This failed intent never owned the existing native entry. Even
                // after a crash its different nonce prevents deletion of that entry.
                try {
                    repository->eraseCredentialRecord(record.reference);
                    records.erase(record.reference);
                    pendingCleanup.erase(record.reference);
                } catch (...) {
                    pendingCleanup.insert(record.reference);
                }
            } else {
                try {
                    cleanup(record.reference);
                } catch (...) {
                }
            }
            throw;
        } catch (...) {
            try {
                cleanup(record.reference);
            } catch (...) {
            }
            throw;
        }
        return record.reference;
    }
    ProxyCredentials recall(const Task& task) const {
        const auto record = records.find(task.proxyCredentialRef);
        if (!vault || record == records.end() || record->second.ownerId != task.ownerId ||
            canonicalRoute(record->second.proxyEndpoint) != canonicalRoute(task.spec.proxy))
            throw std::invalid_argument(
                "Saved proxy credential binding is unavailable. Rebind this task explicitly.");
        try {
            auto value = vault->recall({task.ownerId, task.spec.proxy, record->second.ownershipNonce},
                                       task.proxyCredentialRef);
            if (!value)
                throw std::runtime_error("Missing credential entry.");
            validateCredentials(task.spec.proxy, *value, true);
            return *value;
        } catch (...) {
            throw std::invalid_argument(
                "Saved proxy credentials are unavailable. Reenter credentials and rebind this task.");
        }
    }
};

ContextRegistry::ContextRegistry(ContextDependencies dependencies) : impl_(std::make_unique<Impl>()) {
    impl_->repository = std::move(dependencies.repository);
    impl_->vault = std::move(dependencies.vault);
}
ContextRegistry::~ContextRegistry() = default;
void ContextRegistry::initialize(const std::vector<Task>& savedTasks) {
    if (!impl_->repository)
        return;
    if (!isOpaqueId(impl_->repository->installationId()))
        throw std::runtime_error("Invalid download context installation identity.");
    for (auto context : impl_->repository->loadContexts()) {
        if (!validateDownloadContext(context).empty() || !impl_->contexts.emplace(context.id, context).second)
            throw std::runtime_error("Saved download context is invalid.");
    }
    for (auto record : impl_->repository->loadCredentialRecords()) {
        if (!isOpaqueId(record.reference) ||
            (record.ownerId != "standalone" && !isOpaqueId(record.ownerId)) ||
            canonicalRoute(record.proxyEndpoint).empty() ||
            (!record.ownershipNonce.empty() && !isOpaqueId(record.ownershipNonce)) ||
            !impl_->records.emplace(record.reference, record).second)
            throw std::runtime_error("Saved credential ownership journal is invalid.");
    }
    cleanupUnused(savedTasks);
}
bool ContextRegistry::cleanupUnused(const std::vector<Task>& savedTasks) {
    if (!impl_->repository)
        return true;
    std::set<std::string> used;
    for (const auto& [id, context] : impl_->contexts)
        if (context.enabled && !context.proxyCredentialRef.empty())
            used.insert(context.proxyCredentialRef);
    for (const auto& task : savedTasks)
        if (impl_->enabled(task.ownerId) && !task.proxyCredentialRef.empty())
            used.insert(task.proxyCredentialRef);
    std::vector<std::string> unused;
    for (const auto& [reference, record] : impl_->records)
        if (!impl_->enabled(record.ownerId) || !used.contains(reference))
            unused.push_back(reference);
    for (const auto& reference : unused)
        try {
            impl_->cleanup(reference);
        } catch (...) { /* Journal and visible warning retain the retry. */
        }
    for (const auto& [owner, context] : impl_->contexts)
        if (!context.enabled)
            try {
                impl_->clearRevokedReference(owner);
            } catch (...) { /* Persisted disabled state retains the retry. */
            }
    return impl_->pendingCleanup.empty() && impl_->pendingContextCleanup.empty();
}
DownloadContext ContextRegistry::configure(ContextConfiguration draft, ProxyCredentials credentials,
                                           bool remember) {
    impl_->requireRepository();
    const DownloadContext* previous = nullptr;
    if (!draft.id.empty())
        previous = &impl_->context(draft.id);
    if ((!previous && draft.expectedRevision != 0) ||
        (previous && previous->revision != draft.expectedRevision))
        throw std::invalid_argument("Download context revision changed. Reload before editing.");
    if (previous && previous->revision == std::numeric_limits<std::int64_t>::max())
        throw std::invalid_argument("Download context revision limit reached.");
    if (previous && !previous->enabled && draft.reactivate &&
        std::any_of(impl_->records.begin(), impl_->records.end(),
                    [&](const auto& entry) { return entry.second.ownerId == previous->id; }))
        throw std::invalid_argument("Complete revoked context credential cleanup before reactivating it.");
    if (!previous && impl_->contexts.size() >= registryLimit)
        throw std::invalid_argument("Too many download contexts.");
    DownloadContext result;
    result.id = previous ? previous->id : newTaskId();
    result.externalKey = std::move(draft.externalKey);
    result.label = std::move(draft.label);
    result.downloadRoot = draft.downloadRoot.lexically_normal();
    result.proxyEndpoint = canonicalRoute(draft.proxyEndpoint);
    result.proxyAuthenticationRequired = draft.proxyAuthenticationRequired || hasCredentials(credentials);
    result.originAuthenticationRequired = draft.originAuthenticationRequired;
    result.revision = previous ? previous->revision + 1 : 1;
    result.enabled = !previous || previous->enabled || draft.reactivate;
    if (!result.enabled && (remember || hasCredentials(credentials)))
        throw std::invalid_argument("Reactivate the context explicitly before supplying credentials.");
    for (const auto& [id, context] : impl_->contexts)
        if (id != result.id && context.externalKey == result.externalKey)
            throw std::invalid_argument("A download context already uses this external key.");
    if (const auto error = validateDownloadContext(result); !error.empty())
        throw std::invalid_argument(error);
    if (hasCredentials(credentials) || remember)
        validateCredentials(result.proxyEndpoint, credentials, true);
    std::optional<Impl::MemoryCredential> memory;
    if (hasCredentials(credentials) && !remember)
        memory = Impl::MemoryCredential{result.proxyEndpoint, credentials};
    if (!hasCredentials(credentials) && !remember && previous && previous->enabled && !draft.reactivate &&
        result.proxyAuthenticationRequired &&
        result.proxyEndpoint == canonicalRoute(previous->proxyEndpoint)) {
        result.proxyCredentialRef = previous->proxyCredentialRef;
        if (const auto found = impl_->memory.find({previous->id, previous->revision});
            found != impl_->memory.end())
            memory = found->second;
    }
    if (memory && impl_->memory.size() >= registryLimit)
        throw std::invalid_argument("Too many pinned process-local credential revisions.");
    std::string createdReference;
    if (remember)
        result.proxyCredentialRef = createdReference =
            impl_->store(result.id, result.proxyEndpoint, credentials);
    try {
        impl_->repository->saveContext(result);
    } catch (...) {
        if (!createdReference.empty())
            try {
                impl_->cleanup(createdReference);
            } catch (...) {
            }
        throw;
    }
    impl_->contexts[result.id] = result;
    impl_->pendingContextCleanup.erase(result.id);
    if (memory)
        impl_->memory[{result.id, result.revision}] = std::move(*memory);
    impl_->invalidateSessions(result.id);
    return result;
}
std::vector<DownloadContext> ContextRegistry::list() const {
    std::vector<DownloadContext> result;
    for (const auto& [id, context] : impl_->contexts)
        result.push_back(context);
    return result;
}
std::string ContextRegistry::issueClient(const std::string& ownerId) {
    std::int64_t revision = 0;
    if (ownerId != "standalone") {
        const auto& context = impl_->context(ownerId);
        if (!context.enabled)
            denied();
        revision = context.revision;
    }
    std::erase_if(impl_->sessions, [](const auto& value) { return value.second.expiresAt <= unixTime(); });
    if (impl_->sessions.size() >= registryLimit)
        throw std::invalid_argument("Too many active download context sessions.");
    const auto token = newTaskId();
    impl_->sessions.emplace(token, Impl::Session{ownerId, revision, unixTime() + 3600});
    return token;
}
std::string ContextRegistry::ownerFor(const std::string& token) const {
    const auto session = impl_->sessions.find(token);
    if (!isOpaqueId(token) || session == impl_->sessions.end() || session->second.expiresAt <= unixTime() ||
        !impl_->enabled(session->second.owner))
        denied();
    if (session->second.owner == "standalone")
        return "standalone";
    const auto& context = impl_->context(session->second.owner);
    if (context.revision != session->second.revision)
        denied();
    return session->second.owner;
}
Task ContextRegistry::prepareScoped(const std::string& token, const ScopedDownloadDraft& draft) const {
    Task task;
    task.ownerId = ownerFor(token);
    if (task.ownerId == "standalone")
        throw std::invalid_argument(
            "Standalone sessions cannot create scoped downloads. Use trusted desktop addition.");
    const auto& context = impl_->context(task.ownerId);
    if (!(draft.url.starts_with("http://") || draft.url.starts_with("https://") ||
          draft.url.starts_with("magnet:?")))
        throw std::invalid_argument("Scoped downloads require an HTTP, HTTPS or magnet source.");
    if (draft.filename.empty() || draft.filename.size() > 1024 || draft.filename == "." ||
        draft.filename == ".." || draft.filename.find_first_of("/\\") != std::string::npos)
        throw std::invalid_argument("Scoped download filename must be a single valid filename.");
    task.requestName = draft.filename;
    task.contextRevision = context.revision;
    task.proxyCredentialRef = context.proxyCredentialRef;
    task.originAuthenticationRequired = context.originAuthenticationRequired || !draft.originGrantId.empty();
    task.originGrantId = draft.originGrantId;
    task.spec.url = draft.url;
    task.spec.kind = sourceKind(task.spec);
    task.spec.destination = context.downloadRoot / nativePath(draft.filename);
    task.spec.proxy = context.proxyEndpoint;
    task.spec.proxyAuthenticationRequired = context.proxyAuthenticationRequired;
    task.spec.startPaused = draft.startPaused;
    if (const auto error = validateSpec(task.spec); !error.empty())
        throw std::invalid_argument(error);
    if (task.originAuthenticationRequired)
        impl_->grant(task.originGrantId, task.ownerId, task.spec.url);
    return task;
}
DownloadSpec ContextRegistry::resolve(const Task& task) const {
    if (!impl_->enabled(task.ownerId))
        throw std::invalid_argument("Download context is disabled or unavailable.");
    auto spec = task.spec;
    if (task.ownerId != "standalone") {
        spec.proxyUsername.clear();
        spec.proxyPassword.clear();
        spec.cookie.clear();
        spec.authorization.clear();
        spec.headers.clear();
    }
    if (!task.proxyCredentialRef.empty()) {
        const auto credentials = impl_->recall(task);
        spec.proxyUsername = credentials.username;
        spec.proxyPassword = credentials.password;
    } else if (task.ownerId != "standalone" && spec.proxyAuthenticationRequired) {
        const auto found = impl_->memory.find({task.ownerId, task.contextRevision});
        if (found == impl_->memory.end() || found->second.route != canonicalRoute(spec.proxy))
            throw std::invalid_argument(
                "Process-local proxy credentials are unavailable. Reenter credentials and rebind this task.");
        spec.proxyUsername = found->second.credentials.username;
        spec.proxyPassword = found->second.credentials.password;
    }
    if (task.ownerId != "standalone" && task.originAuthenticationRequired) {
        const auto& grant = impl_->grant(task.originGrantId, task.ownerId, spec.url);
        spec.cookie = grant.cookie;
        spec.authorization = grant.authorization;
    }
    spec.originAuthenticationRequired =
        task.originAuthenticationRequired || requiresOriginAuthentication(spec);
    if (const auto error = validateOriginAuthentication(spec, true); !error.empty())
        throw std::invalid_argument(error);
    if (const auto error = validateProxy(spec, true); !error.empty())
        throw std::invalid_argument(error);
    return spec;
}
std::string ContextRegistry::issueGrant(const std::string& ownerId, std::string url, std::string cookie,
                                        std::string authorization, std::int64_t expiresAt) {
    if (!impl_->context(ownerId).enabled)
        denied();
    const auto now = unixTime();
    if (expiresAt <= now || expiresAt > now + 300 ||
        !(url.starts_with("http://") || url.starts_with("https://")) || cookie.size() > 16384 ||
        authorization.size() > 16384 || (cookie.empty() && authorization.empty()))
        throw std::invalid_argument(
            "Origin grants require an HTTP URL, bounded credentials and expiry within five minutes.");
    DownloadSpec check;
    check.url = url;
    check.destination = "grant-validation.bin";
    check.cookie = cookie;
    check.authorization = authorization;
    if (const auto error = validateSpec(check); !error.empty())
        throw std::invalid_argument(error);
    std::erase_if(impl_->grants, [now](const auto& value) { return value.second.expiresAt <= now; });
    if (impl_->grants.size() >= registryLimit)
        throw std::invalid_argument("Too many active origin grants.");
    const auto id = newTaskId();
    impl_->grants.emplace(
        id, Impl::Grant{ownerId, std::move(url), std::move(cookie), std::move(authorization), expiresAt});
    return id;
}
void ContextRegistry::beginRevoke(const std::string& ownerId) {
    auto context = impl_->context(ownerId);
    if (context.enabled) {
        if (context.revision == std::numeric_limits<std::int64_t>::max())
            throw std::invalid_argument("Download context revision limit reached.");
        context.enabled = false;
        ++context.revision;
        impl_->repository->saveContext(context);
        impl_->contexts[ownerId] = context;
    }
    impl_->invalidateSessions(ownerId);
}
void ContextRegistry::finishRevoke(const std::string& ownerId) {
    if (impl_->context(ownerId).enabled)
        throw std::invalid_argument("Disable the download context before revocation cleanup.");
    std::erase_if(impl_->memory, [&](const auto& value) { return value.first.first == ownerId; });
    std::erase_if(impl_->grants, [&](const auto& value) { return value.second.owner == ownerId; });
    std::vector<std::string> owned;
    for (const auto& [reference, record] : impl_->records)
        if (record.ownerId == ownerId)
            owned.push_back(reference);
    bool failed = false;
    for (const auto& reference : owned)
        try {
            impl_->cleanup(reference);
        } catch (...) {
            failed = true;
        }
    if (failed)
        throw std::runtime_error("Context is revoked, but some native credential cleanup requires retry.");
    impl_->clearRevokedReference(ownerId);
}
void ContextRegistry::rebind(Task& task, const std::string& grantId) const {
    const auto& context = impl_->context(task.ownerId);
    if (!context.enabled)
        denied();
    auto rebound = task;
    rebound.contextRevision = context.revision;
    rebound.proxyCredentialRef = context.proxyCredentialRef;
    rebound.spec.proxy = context.proxyEndpoint;
    rebound.spec.proxyAuthenticationRequired = context.proxyAuthenticationRequired;
    rebound.spec.proxyUsername.clear();
    rebound.spec.proxyPassword.clear();
    rebound.spec.cookie.clear();
    rebound.spec.authorization.clear();
    rebound.spec.headers.clear();
    rebound.originAuthenticationRequired =
        task.originAuthenticationRequired || context.originAuthenticationRequired || !grantId.empty();
    rebound.spec.originAuthenticationRequired = rebound.originAuthenticationRequired;
    rebound.originGrantId = grantId;
    if (rebound.originAuthenticationRequired)
        impl_->grant(grantId, task.ownerId, task.spec.url);
    if (const auto error = validateSpec(rebound.spec); !error.empty())
        throw std::invalid_argument(error);
    task = std::move(rebound);
}
void ContextRegistry::rememberTask(Task& task) {
    if (!impl_->enabled(task.ownerId))
        denied();
    const ProxyCredentials credentials{task.spec.proxyUsername, task.spec.proxyPassword};
    const auto reference = impl_->store(task.ownerId, task.spec.proxy, credentials);
    task.proxyCredentialRef = reference;
    task.spec.proxyAuthenticationRequired = true;
    task.spec.proxyUsername.clear();
    task.spec.proxyPassword.clear();
}
void ContextRegistry::discardUncommitted(const Task& task) {
    const auto found = impl_->records.find(task.proxyCredentialRef);
    if (found == impl_->records.end())
        return;
    if (found->second.ownerId != task.ownerId ||
        canonicalRoute(found->second.proxyEndpoint) != canonicalRoute(task.spec.proxy))
        throw std::invalid_argument("Credential cleanup binding does not match.");
    impl_->cleanup(task.proxyCredentialRef);
}
bool ContextRegistry::enabled(const std::string& ownerId) const { return impl_->enabled(ownerId); }
std::string ContextRegistry::warning() const {
    return impl_->pendingCleanup.empty() && impl_->pendingContextCleanup.empty()
               ? std::string{}
               : "Some owned credential cleanup or saved credential status requires retry. Unavailable "
                 "bindings will not be used.";
}
} // namespace maskpilot
