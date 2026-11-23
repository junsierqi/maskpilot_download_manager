#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <atomic>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/credential_vault.h>
#include <mutex>
#include <thread>

using namespace maskpilot;
using namespace std::chrono_literals;
namespace {
struct Observation {
    std::mutex mutex;
    std::vector<Task> started;
    std::atomic<int> active{0};
};
struct ObservingTransport final : ITransferTransport {
    std::shared_ptr<Observation> observation;
    explicit ObservingTransport(std::shared_ptr<Observation> value) : observation(std::move(value)) {}
    void run(const Task& task, const std::filesystem::path&, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        ++observation->active;
        {
            std::lock_guard lock(observation->mutex);
            observation->started.push_back(task);
        }
        while (!control.pause && !control.cancel)
            std::this_thread::sleep_for(2ms);
        std::this_thread::sleep_for(40ms);
        --observation->active;
        TransferEvent event;
        event.outcome = control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused;
        callback(event);
    }
};
template <class Predicate> bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
class DownloadContexts : public ::testing::Test {
  protected:
    QTemporaryDir temporary;
    std::filesystem::path root = std::filesystem::path(temporary.path().toStdWString());
    std::shared_ptr<Observation> observation = std::make_shared<Observation>();
    std::unique_ptr<DownloadService> service;
    std::string installation;
    void SetUp() override { ASSERT_TRUE(temporary.isValid()); }
    void TearDown() override {
        service.reset();
        if (!std::filesystem::exists(root / L"tasks.db"))
            return;
        // Every record is from this test's private database/installation; never enumerate the OS vault.
        try {
            auto store = makeSqliteStore(root / L"tasks.db");
            auto vault = makeNativeCredentialVault(store.contexts->installationId());
            for (const auto& record : store.contexts->loadCredentialRecords()) {
                vault->forget({record.ownerId, record.proxyEndpoint, record.ownershipNonce},
                              record.reference);
                store.contexts->eraseCredentialRecord(record.reference);
            }
        } catch (const std::exception&) {
            ADD_FAILURE() << "Exact owned context fixture credential cleanup failed";
        }
    }
    void open() {
        auto store = makeSqliteStore(root / L"tasks.db");
        installation = store.contexts->installationId();
        service = std::make_unique<DownloadService>(
            std::move(store.tasks),
            [state = observation] { return std::make_unique<ObservingTransport>(state); }, root / L"parts",
            ContextDependencies{std::move(store.contexts), makeNativeCredentialVault(installation)});
    }
    ContextConfiguration configuration(const std::string& label) {
        ContextConfiguration result;
        result.externalKey = label;
        result.label = label;
        result.downloadRoot = root / label;
        return result;
    }
    ProxyCredentialBinding bindingFor(const std::string& reference) {
        auto store = makeSqliteStore(root / L"tasks.db");
        for (const auto& record : store.contexts->loadCredentialRecords())
            if (record.reference == reference)
                return {record.ownerId, record.proxyEndpoint, record.ownershipNonce};
        throw std::runtime_error("Expected owned test credential journal entry");
    }
    ScopedDownloadDraft draft(bool paused = true) {
        return {"https://example.invalid/data.bin", "data.bin", {}, paused};
    }
    bool state(const std::string& id, TaskState expected) {
        return eventually([&] {
            for (const auto& task : service->snapshot())
                if (task.id == id)
                    return task.state == expected;
            return false;
        });
    }
    std::size_t starts() {
        std::lock_guard lock(observation->mutex);
        return observation->started.size();
    }
};
TEST_F(DownloadContexts, StandaloneOriginRequirementSurvivesRestartAndNeedsAnExplicitAnonymousEdit) {
    open();
    DownloadSpec spec;
    spec.url = "https://example.invalid/private";
    spec.destination = root / "private.bin";
    spec.cookie = "session=original";
    spec.startPaused = true;
    const auto id = service->add(spec).get();
    EXPECT_TRUE(service->snapshot().front().originAuthenticationRequired);
    EXPECT_TRUE(service->snapshot().front().spec.originAuthenticationRequired);
    service.reset();
    open();
    auto anonymous = service->snapshot().front().spec;
    EXPECT_TRUE(anonymous.cookie.empty());
    EXPECT_TRUE(anonymous.originAuthenticationRequired);
    service->resume(id);
    ASSERT_TRUE(state(id, TaskState::NeedsAttention));
    EXPECT_EQ(starts(), 0u);
    anonymous.originAuthenticationRequired = false;
    service->updateRequest(id, anonymous); // Compatibility API cannot silently downgrade.
    service->resume(id);
    ASSERT_TRUE(state(id, TaskState::NeedsAttention));
    EXPECT_EQ(starts(), 0u);
    auto authorized = anonymous;
    authorized.cookie = "session=reentered";
    service->updateRequestWithCredentials(id, authorized, false).get();
    service->resume(id);
    ASSERT_TRUE(eventually([&] { return starts() == 1; }));
    {
        std::lock_guard lock(observation->mutex);
        EXPECT_EQ(observation->started.back().spec.cookie, "session=reentered");
        EXPECT_TRUE(observation->started.back().spec.originAuthenticationRequired);
    }
    EXPECT_THROW(service->retryCredentialCleanup().get(), std::exception);
    service->pause(id);
    ASSERT_TRUE(state(id, TaskState::Paused));
    service->updateRequestWithCredentials(id, anonymous, false).get();
    service->resume(id);
    ASSERT_TRUE(eventually([&] { return starts() == 2; }));
    {
        std::lock_guard lock(observation->mutex);
        EXPECT_FALSE(observation->started.back().originAuthenticationRequired);
        EXPECT_FALSE(observation->started.back().spec.originAuthenticationRequired);
    }
    service->pause(id);
    ASSERT_TRUE(state(id, TaskState::Paused));
}
TEST_F(DownloadContexts, RebindCannotDowngradeAnOptionalOriginGrantToAnonymous) {
    open();
    const auto owner = service->configureContext(configuration("optional-grant")).get();
    auto input = draft();
    input.originGrantId =
        service->issueOriginGrant(owner.id, input.url, "session=first", {}, unixTime() + 60).get();
    const auto id = service->addScoped(service->issueClient(owner.id).get(), input, newTaskId()).get();
    EXPECT_THROW(service->rebindTask(id).get(), std::exception);
    EXPECT_TRUE(service->snapshot().front().originAuthenticationRequired);
    EXPECT_EQ(starts(), 0u);
    const auto renewed =
        service->issueOriginGrant(owner.id, input.url, "session=renewed", {}, unixTime() + 60).get();
    service->rebindTask(id, renewed).get();
    service->resume(id);
    ASSERT_TRUE(eventually([&] { return starts() == 1; }));
    {
        std::lock_guard lock(observation->mutex);
        EXPECT_EQ(observation->started.back().spec.cookie, "session=renewed");
        EXPECT_TRUE(observation->started.back().originAuthenticationRequired);
    }
    service->revokeContext(owner.id).get();
}
TEST_F(DownloadContexts, StandaloneCredentialEditsRememberReplaceAndForgetExactNativeEntries) {
    open();
    DownloadSpec spec;
    spec.url = "https://example.invalid/data";
    spec.destination = root / "remembered.bin";
    spec.startPaused = true;
    spec.proxy = "http://127.0.0.1:18081";
    spec.proxyUsername = "first-user";
    spec.proxyPassword = "first-password";
    const auto id = service->add(spec, {}, true).get();
    auto initial = service->snapshot().front();
    auto native = makeNativeCredentialVault(installation);
    const auto firstBinding = bindingFor(initial.proxyCredentialRef);
    ASSERT_TRUE(native->recall(firstBinding, initial.proxyCredentialRef));
    auto replacement = initial.spec;
    replacement.proxyUsername = "second-user";
    replacement.proxyPassword = "second-password";
    service->updateRequestWithCredentials(id, replacement, true).get();
    auto current = service->snapshot().front();
    ASSERT_NE(current.proxyCredentialRef, initial.proxyCredentialRef);
    EXPECT_FALSE(native->recall(firstBinding, initial.proxyCredentialRef));
    const auto currentBinding = bindingFor(current.proxyCredentialRef);
    ASSERT_TRUE(native->recall(currentBinding, current.proxyCredentialRef));
    service->updateRequestWithCredentials(id, current.spec, true).get();
    EXPECT_EQ(service->snapshot().front().proxyCredentialRef, current.proxyCredentialRef);
    auto processOnly = current.spec;
    processOnly.proxyUsername = "session-user";
    processOnly.proxyPassword = "session-password";
    service->updateRequestWithCredentials(id, processOnly, false).get();
    EXPECT_TRUE(service->snapshot().front().proxyCredentialRef.empty());
    EXPECT_EQ(service->snapshot().front().spec.proxyUsername, "session-user");
    EXPECT_FALSE(native->recall(currentBinding, current.proxyCredentialRef));
    service->updateRequestWithCredentials(id, processOnly, true).get();
    current = service->snapshot().front();
    const auto forgottenBinding = bindingFor(current.proxyCredentialRef);
    service->forgetSavedProxyCredentials(id).get();
    EXPECT_FALSE(native->recall(forgottenBinding, current.proxyCredentialRef));
    EXPECT_TRUE(service->snapshot().front().proxyCredentialRef.empty());
    EXPECT_TRUE(service->snapshot().front().spec.proxyAuthenticationRequired);
    service->resume(id);
    ASSERT_TRUE(state(id, TaskState::NeedsAttention));
    EXPECT_EQ(starts(), 0u);
    EXPECT_NO_THROW(service->retryCredentialCleanup().get());
}
TEST_F(DownloadContexts, ForgetPreservesAnEntryStillReferencedByAnotherSavedTask) {
    open();
    DownloadSpec spec;
    spec.url = "https://example.invalid/data";
    spec.destination = root / "first.bin";
    spec.startPaused = true;
    spec.proxy = "http://127.0.0.1:18081";
    spec.proxyUsername = "shared-user";
    spec.proxyPassword = "shared-password";
    const auto first = service->add(spec, {}, true).get();
    auto clone = service->snapshot().front();
    const auto reference = clone.proxyCredentialRef;
    const auto binding = bindingFor(reference);
    auto native = makeNativeCredentialVault(installation);
    service.reset();
    clone.id = newTaskId();
    clone.requestId.clear();
    clone.spec.destination = root / "second.bin";
    {
        auto store = makeSqliteStore(root / L"tasks.db");
        store.tasks->save(clone); // Fixture writer runs only while its owned coordinator is stopped.
    }
    open();
    service->forgetSavedProxyCredentials(first).get();
    EXPECT_TRUE(native->recall(binding, reference));
    service->forgetSavedProxyCredentials(clone.id).get();
    EXPECT_FALSE(native->recall(binding, reference));
}
TEST_F(DownloadContexts, ScopedReplayControlsAndProjectionDoNotCrossOwners) {
    ASSERT_TRUE(temporary.isValid());
    open();
    const auto first = service->configureContext(configuration("first")).get();
    const auto second = service->configureContext(configuration("second")).get();
    const auto a = service->issueClient(first.id).get();
    const auto b = service->issueClient(second.id).get();
    const auto request = newTaskId();
    const auto one = service->addScoped(a, draft(), request).get();
    const auto two = service->addScoped(b, draft(), request).get();
    EXPECT_NE(one, two);
    EXPECT_NE(one, request);
    EXPECT_EQ(service->addScoped(a, draft(), request).get(), one);
    auto changed = draft();
    changed.filename = "different.bin";
    EXPECT_THROW(service->addScoped(a, changed, request).get(), std::invalid_argument);
    changed = draft();
    changed.url = "https://example.invalid/other.bin";
    EXPECT_THROW(service->addScoped(a, changed, request).get(), std::invalid_argument);
    for (const auto command : {ScopedTaskCommand::Pause, ScopedTaskCommand::Resume, ScopedTaskCommand::Cancel,
                               ScopedTaskCommand::Remove})
        EXPECT_THROW(service->controlScoped(a, command, two).get(), std::invalid_argument);
    EXPECT_THROW(service->listScoped(a, 10, two).get(), std::invalid_argument);
    EXPECT_THROW(service->listScoped("invalid").get(), std::exception);
    const auto page = service->listScoped(a).get();
    ASSERT_EQ(page.tasks.size(), 1u);
    EXPECT_EQ(page.tasks[0].id, one);
    EXPECT_EQ(page.tasks[0].name, "data.bin");
    EXPECT_EQ(service->listScoped(b).get().tasks[0].id, two);
    EXPECT_TRUE(service->lastError().empty());
    EXPECT_EQ(starts(), 0u);
}
TEST_F(DownloadContexts, ReplaySurvivesReopenAndOldSessionsDoNot) {
    open();
    const auto owner = service->configureContext(configuration("persisted")).get();
    const auto oldSession = service->issueClient(owner.id).get();
    const auto request = newTaskId();
    const auto id = service->addScoped(oldSession, draft(), request).get();
    service.reset();
    open();
    EXPECT_THROW(service->listScoped(oldSession).get(), std::exception);
    const auto session = service->issueClient(owner.id).get();
    EXPECT_EQ(service->addScoped(session, draft(), request).get(), id);
    const auto page = service->listScoped(session).get();
    ASSERT_EQ(page.tasks.size(), 1u);
    EXPECT_EQ(page.tasks[0].id, id);
}
TEST_F(DownloadContexts, LegacyStandaloneSessionHasNoProfileAuthority) {
    open();
    const auto profile = service->configureContext(configuration("private")).get();
    const auto session = service->issueClient(profile.id).get();
    const auto privateTask = service->addScoped(session, draft(), newTaskId()).get();
    const auto legacy = service->issueClient("standalone").get();
    EXPECT_TRUE(service->listScoped(legacy).get().tasks.empty());
    EXPECT_THROW(service->controlScoped(legacy, ScopedTaskCommand::Resume, privateTask).get(),
                 std::exception);
    EXPECT_THROW(service->addScoped(legacy, draft(), newTaskId()).get(), std::exception);
    DownloadSpec local;
    local.url = draft().url;
    local.destination = root / L"local.bin";
    local.startPaused = true;
    const auto id = service->add(local).get();
    const auto page = service->listScoped(legacy).get();
    ASSERT_EQ(page.tasks.size(), 1u);
    EXPECT_EQ(page.tasks[0].id, id);
    EXPECT_TRUE(service->lastError().empty());
}
TEST_F(DownloadContexts, RevocationWaitsForOwnedWorkersAndSurvivesReopen) {
    open();
    const auto owner = service->configureContext(configuration("revoked")).get();
    const auto session = service->issueClient(owner.id).get();
    const auto id = service->addScoped(session, draft(false), newTaskId()).get();
    ASSERT_TRUE(eventually([&] { return observation->active.load() == 1; }));
    auto revoked = service->revokeContext(owner.id);
    ASSERT_EQ(revoked.wait_for(5s), std::future_status::ready);
    EXPECT_NO_THROW(revoked.get());
    EXPECT_EQ(observation->active.load(), 0);
    EXPECT_TRUE(state(id, TaskState::Paused));
    EXPECT_THROW(service->listScoped(session).get(), std::exception);
    EXPECT_THROW(service->issueClient(owner.id).get(), std::exception);
    auto update = configuration("revoked");
    update.id = owner.id;
    update.expectedRevision = service->contexts().get()[0].revision;
    EXPECT_FALSE(service->configureContext(update).get().enabled);
    EXPECT_THROW(service->issueClient(owner.id).get(), std::exception);
    service.reset();
    open();
    EXPECT_THROW(service->issueClient(owner.id).get(), std::exception);
    service->resume(id);
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(starts(), 1u);
}
TEST_F(DownloadContexts, RouteRotationKeepsAcceptedSnapshotsUntilExplicitRebind) {
    open();
    auto config = configuration("route");
    config.proxyEndpoint = "http://127.0.0.1:18081";
    config.proxyAuthenticationRequired = true;
    const auto owner = service->configureContext(config, {"first-user", "first-password"}).get();
    const auto session = service->issueClient(owner.id).get();
    const auto id = service->addScoped(session, draft(), newTaskId()).get();
    config.id = owner.id;
    config.expectedRevision = owner.revision;
    config.proxyEndpoint = "http://127.0.0.1:18082";
    service->configureContext(config, {"second-user", "second-password"}).get();
    EXPECT_THROW(service->listScoped(session).get(), std::exception);
    const auto current = service->issueClient(owner.id).get();
    service->controlScoped(current, ScopedTaskCommand::Resume, id).get();
    ASSERT_TRUE(eventually([&] { return starts() == 1; }));
    {
        std::lock_guard lock(observation->mutex);
        EXPECT_EQ(observation->started[0].spec.proxy, "http://127.0.0.1:18081");
        EXPECT_EQ(observation->started[0].spec.proxyUsername, "first-user");
    }
    service->controlScoped(current, ScopedTaskCommand::Pause, id).get();
    ASSERT_TRUE(state(id, TaskState::Paused));
    service->rebindTask(id).get();
    service->controlScoped(current, ScopedTaskCommand::Resume, id).get();
    ASSERT_TRUE(eventually([&] { return starts() == 2; }));
    {
        std::lock_guard lock(observation->mutex);
        EXPECT_EQ(observation->started[1].spec.proxy, "http://127.0.0.1:18082");
        EXPECT_EQ(observation->started[1].spec.proxyUsername, "second-user");
    }
    service->revokeContext(owner.id).get();
}
TEST_F(DownloadContexts, MissingMemoryCredentialsAndOriginGrantsNeverStartAWorker) {
    open();
    auto config = configuration("ephemeral");
    config.proxyEndpoint = "http://127.0.0.1:18081";
    config.proxyAuthenticationRequired = true;
    config.originAuthenticationRequired = true;
    const auto owner = service->configureContext(config, {"user", "password"}).get();
    const auto session = service->issueClient(owner.id).get();
    EXPECT_THROW(service->addScoped(session, draft(), newTaskId()).get(), std::exception);
    const auto grant =
        service->issueOriginGrant(owner.id, draft().url, "owner=first", "", unixTime() + 60).get();
    auto authorized = draft();
    authorized.originGrantId = grant;
    const auto id = service->addScoped(session, authorized, newTaskId()).get();
    auto wrongUrl = authorized;
    wrongUrl.url += "?other";
    EXPECT_THROW(service->addScoped(session, wrongUrl, newTaskId()).get(), std::exception);
    service.reset();
    open();
    service->resume(id);
    ASSERT_TRUE(state(id, TaskState::NeedsAttention));
    EXPECT_EQ(starts(), 0u);
    EXPECT_TRUE(service->lastError().empty());
}
TEST_F(DownloadContexts, RememberedNativeCredentialsReopenAndRevokeExactly) {
    open();
    auto config = configuration("remembered");
    config.proxyEndpoint = "socks5://127.0.0.1:18081";
    config.proxyAuthenticationRequired = true;
    const auto owner = service->configureContext(config, {"native-user", "native-password"}, true).get();
    ASSERT_TRUE(isOpaqueId(owner.proxyCredentialRef));
    auto vault = makeNativeCredentialVault(installation);
    const auto binding = bindingFor(owner.proxyCredentialRef);
    ASSERT_TRUE(vault->recall(binding, owner.proxyCredentialRef));
    const auto session = service->issueClient(owner.id).get();
    const auto id = service->addScoped(session, draft(), newTaskId()).get();
    service.reset();
    open();
    const auto restoredSession = service->issueClient(owner.id).get();
    service->controlScoped(restoredSession, ScopedTaskCommand::Resume, id).get();
    ASSERT_TRUE(eventually([&] { return starts() == 1; }));
    {
        std::lock_guard lock(observation->mutex);
        EXPECT_EQ(observation->started[0].spec.proxyUsername, "native-user");
        EXPECT_EQ(observation->started[0].spec.proxyPassword, "native-password");
    }
    service->revokeContext(owner.id).get();
    EXPECT_FALSE(vault->recall(binding, owner.proxyCredentialRef));
    const auto revoked = service->contexts().get();
    ASSERT_EQ(revoked.size(), 1u);
    EXPECT_FALSE(revoked.front().enabled);
    EXPECT_TRUE(revoked.front().proxyCredentialRef.empty());
    EXPECT_GT(revoked.front().revision, owner.revision);
    service.reset();
    open();
    const auto reopened = service->contexts().get();
    ASSERT_EQ(reopened.size(), 1u);
    EXPECT_FALSE(reopened.front().enabled);
    EXPECT_TRUE(reopened.front().proxyCredentialRef.empty());
    EXPECT_EQ(reopened.front().revision, revoked.front().revision);
}
TEST_F(DownloadContexts, OwnerFilteringPrecedesPageLimitAndRejectsForeignCursors) {
    auto store = makeSqliteStore(root / L"tasks.db");
    DownloadContext first{newTaskId(), "large-first", "Large first", root / L"large-first"};
    DownloadContext second{newTaskId(), "large-second", "Large second", root / L"large-second"};
    store.contexts->saveContext(first);
    store.contexts->saveContext(second);
    for (int n = 0; n < 1005; ++n) {
        Task task;
        task.id = newTaskId();
        task.ownerId = n < 1003 ? first.id : second.id;
        task.contextRevision = 1;
        task.spec.url = "https://example.invalid/file";
        task.spec.destination = root / (std::to_string(n) + ".bin");
        task.state = TaskState::Paused;
        store.tasks->save(task);
    }
    store.tasks.reset();
    store.contexts.reset();
    open();
    const auto a = service->issueClient(first.id).get();
    const auto b = service->issueClient(second.id).get();
    const auto small = service->listScoped(b, 1000).get();
    ASSERT_EQ(small.tasks.size(), 2u);
    const auto page = service->listScoped(a, 1000).get();
    ASSERT_EQ(page.tasks.size(), 1000u);
    ASSERT_FALSE(page.nextCursor.empty());
    const auto last = service->listScoped(a, 1000, page.nextCursor).get();
    EXPECT_EQ(last.tasks.size(), 3u);
    EXPECT_TRUE(last.nextCursor.empty());
    EXPECT_THROW(service->listScoped(a, 10, small.tasks[0].id).get(), std::invalid_argument);
    EXPECT_EQ(starts(), 0u);
}
TEST_F(DownloadContexts, StartupCleansOnlyJournaledUnreferencedNativeEntries) {
    auto store = makeSqliteStore(root / L"tasks.db");
    installation = store.contexts->installationId();
    auto vault = makeNativeCredentialVault(installation);
    const CredentialRecord record{newTaskId(), newTaskId(), "http://proxy.example:8080"};
    store.contexts->saveCredentialRecord(record);
    vault->store(record.reference, {record.ownerId, record.proxyEndpoint}, {"user", "password"});
    ASSERT_TRUE(vault->recall({record.ownerId, record.proxyEndpoint}, record.reference));
    store.tasks.reset();
    store.contexts.reset();
    open();
    EXPECT_FALSE(vault->recall({record.ownerId, record.proxyEndpoint}, record.reference));
    auto observed = makeSqliteStore(root / L"tasks.db");
    EXPECT_TRUE(observed.contexts->loadCredentialRecords().empty());
}
struct FailingPolicyRepository final : IContextRepository {
    std::unique_ptr<IContextRepository> delegate;
    std::shared_ptr<std::atomic_bool> failOnlyClear;
    explicit FailingPolicyRepository(std::unique_ptr<IContextRepository> value,
                                     std::shared_ptr<std::atomic_bool> clearFailure = {})
        : delegate(std::move(value)), failOnlyClear(std::move(clearFailure)) {}
    std::string installationId() override { return delegate->installationId(); }
    std::vector<DownloadContext> loadContexts() override { return delegate->loadContexts(); }
    void saveContext(const DownloadContext& context) override {
        if (!failOnlyClear ||
            (failOnlyClear->load() && !context.enabled && context.proxyCredentialRef.empty()))
            throw std::runtime_error("Injected policy write failure");
        delegate->saveContext(context);
    }
    std::vector<CredentialRecord> loadCredentialRecords() override {
        return delegate->loadCredentialRecords();
    }
    void saveCredentialRecord(const CredentialRecord& record) override {
        delegate->saveCredentialRecord(record);
    }
    void eraseCredentialRecord(const std::string& reference) override {
        delegate->eraseCredentialRecord(reference);
    }
};
struct RecordedNativeWrite {
    std::string reference;
    ProxyCredentialBinding binding;
};
struct FailingCleanupVault final : ICredentialVault {
    std::unique_ptr<ICredentialVault> native;
    std::shared_ptr<RecordedNativeWrite> write;
    std::shared_ptr<std::atomic_bool> allowCleanup;
    FailingCleanupVault(std::unique_ptr<ICredentialVault> value, std::shared_ptr<RecordedNativeWrite> state,
                        std::shared_ptr<std::atomic_bool> allow = {})
        : native(std::move(value)), write(std::move(state)), allowCleanup(std::move(allow)) {}
    std::string remember(const ProxyCredentialBinding& binding,
                         const ProxyCredentials& credentials) override {
        return native->remember(binding, credentials);
    }
    void store(const std::string& reference, const ProxyCredentialBinding& binding,
               const ProxyCredentials& credentials) override {
        native->store(reference, binding, credentials);
        *write = {reference, binding};
    }
    std::optional<ProxyCredentials> recall(const ProxyCredentialBinding& binding,
                                           const std::string& reference) override {
        return native->recall(binding, reference);
    }
    bool forget(const ProxyCredentialBinding& binding, const std::string& reference) override {
        if (allowCleanup && allowCleanup->load())
            return native->forget(binding, reference);
        throw CredentialVaultError(CredentialVaultFailure::AccessDenied, "Injected native cleanup denial");
    }
};
TEST_F(DownloadContexts, ExplicitIdleCleanupRetriesACommittedForgetFailure) {
    auto store = makeSqliteStore(root / L"tasks.db");
    installation = store.contexts->installationId();
    auto recorded = std::make_shared<RecordedNativeWrite>();
    auto allow = std::make_shared<std::atomic_bool>(false);
    service = std::make_unique<DownloadService>(
        std::move(store.tasks), [state = observation] { return std::make_unique<ObservingTransport>(state); },
        root / L"parts",
        ContextDependencies{
            std::move(store.contexts),
            std::make_unique<FailingCleanupVault>(makeNativeCredentialVault(installation), recorded, allow)});
    DownloadSpec spec;
    spec.url = "https://example.invalid/data";
    spec.destination = root / "cleanup-retry.bin";
    spec.startPaused = true;
    spec.proxy = "http://127.0.0.1:18081";
    spec.proxyUsername = "retry-user";
    spec.proxyPassword = "retry-password";
    const auto id = service->add(spec, {}, true).get();
    auto native = makeNativeCredentialVault(installation);
    EXPECT_THROW(service->forgetSavedProxyCredentials(id).get(), std::exception);
    EXPECT_TRUE(service->snapshot().front().proxyCredentialRef.empty());
    EXPECT_TRUE(native->recall(recorded->binding, recorded->reference));
    ASSERT_TRUE(eventually([&] { return !service->lastError().empty(); }));
    EXPECT_THROW(service->retryCredentialCleanup().get(), std::exception);
    allow->store(true);
    EXPECT_NO_THROW(service->retryCredentialCleanup().get());
    EXPECT_FALSE(native->recall(recorded->binding, recorded->reference));
    EXPECT_TRUE(service->lastError().empty());
}
TEST_F(DownloadContexts, ReactivationCannotResurrectCredentialReferencesAfterFailedRevoke) {
    auto store = makeSqliteStore(root / L"tasks.db");
    installation = store.contexts->installationId();
    auto recorded = std::make_shared<RecordedNativeWrite>();
    auto allow = std::make_shared<std::atomic_bool>(false);
    service = std::make_unique<DownloadService>(
        std::move(store.tasks), [state = observation] { return std::make_unique<ObservingTransport>(state); },
        root / L"parts",
        ContextDependencies{
            std::move(store.contexts),
            std::make_unique<FailingCleanupVault>(makeNativeCredentialVault(installation), recorded, allow)});
    auto configurationDraft = configuration("reactivation-cleanup");
    configurationDraft.proxyEndpoint = "http://127.0.0.1:18081";
    configurationDraft.proxyAuthenticationRequired = true;
    const auto owner =
        service->configureContext(configurationDraft, {"old-user", "old-password"}, true).get();
    const auto pinnedTask =
        service->addScoped(service->issueClient(owner.id).get(), draft(), newTaskId()).get();
    auto native = makeNativeCredentialVault(installation);
    EXPECT_THROW(service->revokeContext(owner.id).get(), std::exception);
    auto disabled = service->contexts().get().front();
    ASSERT_FALSE(disabled.enabled);
    EXPECT_TRUE(native->recall(recorded->binding, recorded->reference));
    configurationDraft.id = owner.id;
    configurationDraft.expectedRevision = disabled.revision;
    configurationDraft.reactivate = true;
    EXPECT_THROW(service->configureContext(configurationDraft, {"new-user", "new-password"}).get(),
                 std::exception);
    EXPECT_FALSE(service->contexts().get().front().enabled);
    allow->store(true);
    service->retryCredentialCleanup().get();
    disabled = service->contexts().get().front();
    EXPECT_TRUE(disabled.proxyCredentialRef.empty());
    EXPECT_FALSE(native->recall(recorded->binding, recorded->reference));
    configurationDraft.expectedRevision = disabled.revision;
    const auto reactivated =
        service->configureContext(configurationDraft, {"new-user", "new-password"}).get();
    EXPECT_TRUE(reactivated.enabled);
    EXPECT_TRUE(reactivated.proxyCredentialRef.empty());
    const auto currentSession = service->issueClient(owner.id).get();
    EXPECT_TRUE(isOpaqueId(currentSession));
    service->controlScoped(currentSession, ScopedTaskCommand::Resume, pinnedTask).get();
    ASSERT_TRUE(state(pinnedTask, TaskState::NeedsAttention));
    EXPECT_EQ(starts(), 0u);
    service->rebindTask(pinnedTask).get();
    service->controlScoped(currentSession, ScopedTaskCommand::Resume, pinnedTask).get();
    ASSERT_TRUE(eventually([&] { return starts() == 1; }));
    {
        std::lock_guard lock(observation->mutex);
        EXPECT_EQ(observation->started.back().spec.proxyUsername, "new-user");
    }
    service->pause(pinnedTask);
    ASSERT_TRUE(state(pinnedTask, TaskState::Paused));
    EXPECT_TRUE(eventually([&] { return service->lastError().empty(); }));
}
TEST_F(DownloadContexts, SuccessfulReactivationClearsRecoveredCredentialMetadataWarning) {
    auto store = makeSqliteStore(root / L"tasks.db");
    installation = store.contexts->installationId();
    auto rejectClear = std::make_shared<std::atomic_bool>(true);
    service = std::make_unique<DownloadService>(
        std::move(store.tasks), [state = observation] { return std::make_unique<ObservingTransport>(state); },
        root / L"parts",
        ContextDependencies{std::make_unique<FailingPolicyRepository>(std::move(store.contexts), rejectClear),
                            makeNativeCredentialVault(installation)});
    auto configurationDraft = configuration("metadata-recovery");
    configurationDraft.proxyEndpoint = "http://127.0.0.1:18081";
    configurationDraft.proxyAuthenticationRequired = true;
    const auto owner =
        service->configureContext(configurationDraft, {"old-user", "old-password"}, true).get();
    const auto binding = bindingFor(owner.proxyCredentialRef);
    auto native = makeNativeCredentialVault(installation);
    EXPECT_THROW(service->revokeContext(owner.id).get(), std::exception);
    const auto disabled = service->contexts().get().front();
    EXPECT_FALSE(disabled.enabled);
    EXPECT_FALSE(disabled.proxyCredentialRef.empty());
    EXPECT_FALSE(native->recall(binding, owner.proxyCredentialRef));
    ASSERT_TRUE(eventually([&] { return !service->lastError().empty(); }));
    rejectClear->store(false);
    configurationDraft.id = owner.id;
    configurationDraft.expectedRevision = disabled.revision;
    configurationDraft.reactivate = true;
    const auto recovered = service->configureContext(configurationDraft, {"new-user", "new-password"}).get();
    EXPECT_TRUE(recovered.enabled);
    EXPECT_TRUE(recovered.proxyCredentialRef.empty());
    EXPECT_TRUE(eventually([&] { return service->lastError().empty(); }));
    EXPECT_NO_THROW(service->retryCredentialCleanup().get());
}
TEST_F(DownloadContexts, FailedPolicyAndCleanupKeepDurableOwnershipForReopen) {
    auto store = makeSqliteStore(root / L"tasks.db");
    installation = store.contexts->installationId();
    auto recorded = std::make_shared<RecordedNativeWrite>();
    service = std::make_unique<DownloadService>(
        std::move(store.tasks), [state = observation] { return std::make_unique<ObservingTransport>(state); },
        root / L"parts",
        ContextDependencies{
            std::make_unique<FailingPolicyRepository>(std::move(store.contexts)),
            std::make_unique<FailingCleanupVault>(makeNativeCredentialVault(installation), recorded)});
    auto config = configuration("failed-write");
    config.proxyEndpoint = "http://proxy.example:8080";
    EXPECT_THROW(service->configureContext(config, {"user", "password"}, true).get(), std::exception);
    ASSERT_TRUE(isOpaqueId(recorded->reference));
    auto native = makeNativeCredentialVault(installation);
    ASSERT_TRUE(native->recall(recorded->binding, recorded->reference));
    {
        auto observed = makeSqliteStore(root / L"tasks.db");
        const auto records = observed.contexts->loadCredentialRecords();
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].reference, recorded->reference);
        EXPECT_TRUE(observed.contexts->loadContexts().empty());
    }
    // A scoped policy failure and cleanup warning must not disable independent local work.
    DownloadSpec local;
    local.url = draft().url;
    local.destination = root / L"independent.bin";
    local.startPaused = true;
    EXPECT_NO_THROW(service->add(local).get());
    service.reset();
    open();
    EXPECT_FALSE(native->recall(recorded->binding, recorded->reference));
    auto observed = makeSqliteStore(root / L"tasks.db");
    EXPECT_TRUE(observed.contexts->loadCredentialRecords().empty());
}

struct OwnedChild {
    const char* label;
    QProcess process;
    explicit OwnedChild(const char* value) : label(value) {}
    ~OwnedChild() { stop(); }
    void stop() {
        if (process.state() == QProcess::NotRunning)
            return;
        process.write("quit\n");
        process.closeWriteChannel();
        if (!process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished(5000);
            ADD_FAILURE() << "Owned " << label << " fixture did not shut down within its budget";
        }
    }
    QJsonObject start(const QStringList& arguments) {
        process.start(QString::fromUtf8(MASKPILOT_TEST_PYTHON), arguments);
        if (!process.waitForStarted(5000))
            throw std::runtime_error("Owned Python fixture did not start");
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        QByteArray output;
        while (!output.contains('\n') && output.size() <= 4096 &&
               std::chrono::steady_clock::now() < deadline) {
            process.waitForReadyRead(100);
            output += process.readAllStandardOutput();
            if (process.state() == QProcess::NotRunning)
                break;
        }
        const auto object = QJsonDocument::fromJson(output).object();
        if (output.size() > 4096 || object.isEmpty())
            throw std::runtime_error(std::string("Owned Python ") + label +
                                     " fixture did not publish its endpoint; state=" +
                                     std::to_string(static_cast<int>(process.state())) +
                                     "; error=" + process.errorString().toStdString() +
                                     "; stdout=" + output.left(4096).toStdString() +
                                     "; stderr=" + process.readAllStandardError().left(4096).toStdString());
        return object;
    }
};
QJsonObject readObject(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Owned fixture state is unavailable");
    return QJsonDocument::fromJson(file.readAll()).object();
}
TEST_F(DownloadContexts, ActualHttpAndSocksRoutesCarryOnlyTheirOwnedOriginGrants) {
    const QString originScript = R"PY(
import hashlib,http.server,json,pathlib,sys,threading
path=pathlib.Path(sys.argv[1]); lock=threading.Lock(); state={'cookies':[],'proxy_auth':[],'paths':[]}
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args): pass
 def answer(self,head):
  cookie=self.headers.get('Cookie',''); body=('payload-'+cookie).encode()
  with lock:
   state['cookies'].append(cookie); state['proxy_auth'].append(self.headers.get('Proxy-Authorization','')); state['paths'].append(self.path)
   path.write_text(json.dumps(state),encoding='utf-8')
  if self.path=='/redirect':
   self.send_response(302); self.send_header('Location','/outside-grant'); self.send_header('Content-Length','0')
   self.send_header('Connection','close'); self.end_headers(); return
  self.send_response(200); self.send_header('Content-Length',str(len(body)))
  self.send_header('ETag','"'+hashlib.sha256(body).hexdigest()+'"'); self.send_header('Connection','close')
  self.end_headers()
  if not head: self.wfile.write(body)
 def do_HEAD(self): self.answer(True)
 def do_GET(self): self.answer(False)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
server.daemon_threads=True
thread=threading.Thread(target=server.serve_forever); thread.start()
print(json.dumps({'port':server.server_address[1]}),flush=True)
try: sys.stdin.readline()
finally: server.shutdown(); server.server_close(); thread.join()
)PY";
    OwnedChild origin{"origin"}, http{"HTTP proxy"}, socks{"SOCKS proxy"};
    const auto originState = root / L"origin.json";
    const auto originInfo =
        origin.start({"-u", "-c", originScript, QString::fromStdWString(originState.wstring())});
    const auto httpState = root / L"http-proxy.json", socksState = root / L"socks-proxy.json";
    const auto startProxy = [&](OwnedChild& child, const QString& kind, const std::filesystem::path& path) {
        return child.start({"-u", QString::fromUtf8(MASKPILOT_TEST_PROXY_FIXTURE), "--serve", "--kind", kind,
                            "--state", QString::fromStdWString(path.wstring()), "--username", kind,
                            "--password", "fixture-password", "--map", "context-origin.invalid=127.0.0.1"});
    };
    const auto httpInfo = startProxy(http, "http", httpState);
    const auto socksInfo = startProxy(socks, "socks5", socksState);
    auto store = makeSqliteStore(root / L"tasks.db");
    installation = store.contexts->installationId();
    service = std::make_unique<DownloadService>(
        std::move(store.tasks), makeUnifiedTransferFactory(), root / L"parts",
        ContextDependencies{std::move(store.contexts), makeNativeCredentialVault(installation)});
    auto aConfig = configuration("alpha"), bConfig = configuration("beta");
    aConfig.proxyEndpoint = "http://127.0.0.1:" + std::to_string(httpInfo["port"].toInt());
    bConfig.proxyEndpoint = "socks5://127.0.0.1:" + std::to_string(socksInfo["port"].toInt());
    aConfig.originAuthenticationRequired = bConfig.originAuthenticationRequired = true;
    const auto a = service->configureContext(aConfig, {"http", "fixture-password"}).get();
    const auto b = service->configureContext(bConfig, {"socks5", "fixture-password"}).get();
    const auto aSession = service->issueClient(a.id).get(), bSession = service->issueClient(b.id).get();
    ScopedDownloadDraft input;
    input.url = "http://context-origin.invalid:" + std::to_string(originInfo["port"].toInt()) + "/file";
    input.filename = "data.bin";
    input.originGrantId =
        service->issueOriginGrant(a.id, input.url, "owner=alpha", "", unixTime() + 60).get();
    const auto first = service->addScoped(aSession, input, newTaskId()).get();
    EXPECT_THROW(service->addScoped(bSession, input, newTaskId()).get(), std::exception);
    input.originGrantId = service->issueOriginGrant(b.id, input.url, "owner=beta", "", unixTime() + 60).get();
    const auto second = service->addScoped(bSession, input, newTaskId()).get();
    ASSERT_TRUE(state(first, TaskState::Completed));
    ASSERT_TRUE(state(second, TaskState::Completed));
    input.url = "http://context-origin.invalid:" + std::to_string(originInfo["port"].toInt()) + "/redirect";
    input.filename = "redirect.bin";
    input.originGrantId =
        service->issueOriginGrant(a.id, input.url, "owner=alpha", "", unixTime() + 60).get();
    const auto redirect = service->addScoped(aSession, input, newTaskId()).get();
    EXPECT_TRUE(state(redirect, TaskState::NeedsAttention));
    for (const auto& [label, expected] :
         {std::pair{"alpha", "payload-owner=alpha"}, std::pair{"beta", "payload-owner=beta"}}) {
        std::ifstream inputFile(root / label / L"data.bin", std::ios::binary);
        EXPECT_EQ(std::string((std::istreambuf_iterator<char>(inputFile)), {}), expected);
    }
    http.stop();
    socks.stop();
    origin.stop();
    const auto recorded = readObject(originState);
    const auto cookies = recorded["cookies"].toArray();
    EXPECT_TRUE(cookies.contains("owner=alpha"));
    EXPECT_TRUE(cookies.contains("owner=beta"));
    EXPECT_TRUE(recorded["paths"].toArray().contains("/redirect"));
    EXPECT_FALSE(recorded["paths"].toArray().contains("/outside-grant"));
    for (const auto& value : cookies)
        EXPECT_TRUE(value == "owner=alpha" || value == "owner=beta");
    for (const auto& value : recorded["proxy_auth"].toArray())
        EXPECT_TRUE(value.toString().isEmpty());
    for (const auto& path : {httpState, socksState}) {
        const auto proxy = readObject(path);
        EXPECT_GT(proxy["auth_accepted"].toInt(), 0);
        EXPECT_GT(proxy["forwarded_bytes"].toDouble(), 0);
    }
}
} // namespace
