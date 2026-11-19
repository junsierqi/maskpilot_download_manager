#include "browser_recovery_registry.h"
#include <algorithm>
#include <limits>
#include <maskpilot/domain/download_context.h>
#include <stdexcept>

namespace maskpilot {
namespace {
[[noreturn]] void denied() {
    throw std::invalid_argument(
        "Browser recovery is unavailable. Pause the original HTTP task and start recovery again.");
}
bool bounded(const std::string& value, std::size_t maximum) {
    return value.size() <= maximum &&
           std::none_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}
} // namespace
void BrowserRecoveryRegistry::eligible(const Task& task) {
    if (task.ownerId != "standalone" || !task.projectId.empty() || task.spec.kind != TransferKind::Http ||
        sourceKind(task.spec) != TransferKind::Http || task.runIntent != RunIntent::Paused ||
        isActive(task.state) || isTerminal(task.state) || !validateSpec(task.spec).empty())
        denied();
}
BrowserRecoverySession BrowserRecoveryRegistry::begin(const Task& task, std::int64_t now) {
    eligible(task);
    if (now < 0 || now > std::numeric_limits<std::int64_t>::max() - 600)
        denied();
    std::erase_if(entries_, [&](const auto& pair) {
        const auto& entry = pair.second;
        return !entry.valid || now < entry.issuedAt || now >= entry.expiresAt || entry.taskId == task.id;
    });
    if (entries_.size() >= 64)
        throw std::invalid_argument("Too many pending browser recoveries. Cancel one or wait for expiry.");
    auto token = newTaskId();
    while (entries_.contains(token))
        token = newTaskId();
    entries_.emplace(token, Entry{task.id, now, now + 600});
    const auto filename = task.spec.destination.filename().u8string();
    auto source = task.spec;
    source.url = task.spec.referrer;
    const bool validPage = (source.url.starts_with("http://") || source.url.starts_with("https://")) &&
                           bounded(source.url, 32768) && validateSpec(source).empty();
    return {token, task.id, std::string(filename.begin(), filename.end()),
            validPage ? source.url : task.spec.url, now + 600};
}
BrowserRecoveryRegistry::Entry& BrowserRecoveryRegistry::current(const std::string& token, std::int64_t now) {
    const auto found = entries_.find(token);
    if (!isOpaqueId(token) || found == entries_.end() || !found->second.valid)
        denied();
    if (now < found->second.issuedAt || now >= found->second.expiresAt) {
        found->second.valid = false;
        found->second.material = {};
        denied();
    }
    return found->second;
}
std::string BrowserRecoveryRegistry::taskId(const std::string& token, std::int64_t now) {
    return current(token, now).taskId;
}
void BrowserRecoveryRegistry::apply(const std::string& token, const Task& task,
                                    BrowserRecoveryMaterial material, std::int64_t now,
                                    const std::function<void(Task&)>& commit) {
    auto& entry = current(token, now);
    eligible(task);
    if (entry.taskId != task.id)
        denied();
    if (!bounded(material.url, 32768) || !bounded(material.cookie, 16384) ||
        !bounded(material.referrer, 32768) || !bounded(material.userAgent, 1024))
        denied();
    if (entry.applied) {
        if (entry.material != material)
            denied();
        return; // The identical retry acknowledges the prior durable save, never reapplies it.
    }
    auto candidate = task;
    candidate.spec.url = material.url;
    candidate.spec.cookie = material.cookie;
    candidate.spec.referrer = material.referrer;
    candidate.spec.userAgent = material.userAgent.empty() ? defaultUserAgent : material.userAgent;
    candidate.spec.authorization.clear();
    candidate.spec.headers.clear();
    candidate.originGrantId.clear();
    candidate.originAuthenticationRequired = task.originAuthenticationRequired ||
                                             task.spec.originAuthenticationRequired ||
                                             requiresOriginAuthentication(candidate.spec);
    candidate.spec.originAuthenticationRequired = candidate.originAuthenticationRequired;
    if (!(material.url.starts_with("http://") || material.url.starts_with("https://")) ||
        !validateSpec(candidate.spec).empty())
        throw std::invalid_argument("Invalid browser recovery source material.");
    if (!validateOriginAuthentication(candidate.spec, true).empty())
        throw std::invalid_argument("This task requires fresh sign-in material. Enable Cookie forwarding or "
                                    "edit sign-in information on the desktop.");
    candidate.spec.startPaused = true;
    candidate.state = TaskState::Paused;
    candidate.runIntent = RunIntent::Paused;
    candidate.responseUrl.clear();
    candidate.responseContentType.clear();
    candidate.error.clear();
    candidate.retries = 0;
    candidate.retryAt = 0;
    if (candidate.spec.url != task.spec.url) {
        candidate.downloaded = 0;
        candidate.total = -1;
        candidate.resumable = false;
    }
    // commit persists before replacing actor state. Its central save invalidates old sessions.
    // On persistence failure this receipt remains unapplied and can safely be retried.
    commit(candidate);
    entry.material = std::move(material);
    entry.applied = true;
    entry.valid = true;
}
void BrowserRecoveryRegistry::cancel(const std::string& token) {
    if (!isOpaqueId(token))
        denied();
    entries_.erase(token);
}
void BrowserRecoveryRegistry::invalidate(const std::string& taskId) {
    for (auto& [token, entry] : entries_)
        if (entry.taskId == taskId) {
            entry.valid = false;
            entry.material = {};
        }
}
} // namespace maskpilot
