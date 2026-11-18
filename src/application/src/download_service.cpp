#include "context_registry.h"
#include "payload_budget.h"
#include "queue_registry.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <maskpilot/application/download_service.h>
#include <maskpilot/domain/proxy.h>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <type_traits>

namespace maskpilot {
namespace {
bool sameProxyRoute(const std::string& first, const std::string& second) {
    const auto a = parseProxyEndpoint(first), b = parseProxyEndpoint(second);
    return a.type == b.type && a.host == b.host && a.port == b.port;
}
bool ownedRecoveryArtifact(const std::filesystem::path& path) {
    const auto text = path.filename().generic_u8string();
    std::u8string_view name(text);
    if (name == u8"identity" || name == u8"identity.new")
        return true;
    if (name.ends_with(u8".new"))
        name.remove_suffix(4);
    if (name.starts_with(u8"part-"))
        name.remove_prefix(5);
    else if (name.starts_with(u8"offset-"))
        name.remove_prefix(7);
    else
        return false;
    if (name.empty() || name.size() > 3 || (name.size() > 1 && name.front() == u8'0'))
        return false;
    int id = 0;
    for (const auto ch : name) {
        if (ch < u8'0' || ch > u8'9')
            return false;
        id = id * 10 + (ch - u8'0');
    }
    return id < maxRecoveryExtents;
}
void loadTorrentSource(DownloadSpec& spec, const TransportFactory& factory) {
    if (spec.kind != TransferKind::BitTorrent || spec.torrent.metadata || !spec.url.starts_with("torrent:"))
        return;
    auto bytes = factory()->captureMetadata(spec);
    if (!bytes || bytes->empty() || bytes->size() > maxTorrentMetadataBytes)
        throw std::invalid_argument("Torrent metadata is empty or exceeds 16 MiB.");
    spec.torrent.metadata = std::move(bytes);
}
void validateSelection(const DownloadSpec& spec, const TransferDescription& description) {
    if (spec.torrent.allFiles || spec.torrent.selectedFiles.empty())
        return;
    if (!description.files)
        throw std::invalid_argument("Obtain torrent metadata before choosing files.");
    for (const auto index : spec.torrent.selectedFiles)
        if (std::none_of(description.files->begin(), description.files->end(),
                         [&](const auto& file) { return file.index == index && !file.padding; }))
            throw std::invalid_argument("Torrent selection refers to an unavailable file.");
}
} // namespace
struct DownloadService::Impl {
    struct Job {
        std::shared_ptr<TransferControl> control = std::make_shared<TransferControl>();
        std::jthread thread;
        std::int64_t mediaRate = -1;
        bool policyPause = false;
    };
    std::unique_ptr<ITaskRepository> repository;
    TransportFactory factory;
    std::filesystem::path workDirectory;
    std::map<std::string, Task> tasks;
    std::map<std::string, std::unique_ptr<Job>> jobs;
    std::unique_ptr<ContextRegistry> contexts;
    std::unique_ptr<QueueRegistry> queueRegistry;
    std::shared_ptr<IQueueTrafficRepository> traffic;
    std::string lastQueue;
    std::map<std::string, std::vector<std::shared_ptr<std::promise<void>>>> pendingRevocations;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> commands;
    std::vector<Task> published;
    std::string error;
    std::string contextWarning;
    std::jthread coordinator;
    std::atomic_bool stopping{false};
    bool healthy = true;
    int concurrentTasks = 3;
    std::int64_t globalLimit = 0, globalUploadLimit = 0;

    template <class Function> auto request(Function function) -> std::future<std::invoke_result_t<Function>> {
        using Result = std::invoke_result_t<Function>;
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        post([promise, function = std::move(function)]() mutable {
            try {
                if constexpr (std::is_void_v<Result>) {
                    function();
                    promise->set_value();
                } else {
                    promise->set_value(function());
                }
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
        return future;
    }
    std::vector<Task> allTasks() const {
        std::vector<Task> result;
        result.reserve(tasks.size());
        for (const auto& [id, task] : tasks)
            result.push_back(task);
        return result;
    }
    std::string addPrepared(Task task, std::string requestId, bool legacy, bool rememberProxy) {
        if (!healthy)
            throw std::runtime_error("Task storage unavailable.");
        queueRegistry->get(task.queueId);
        auto& spec = task.spec;
        task.originAuthenticationRequired =
            task.originAuthenticationRequired || requiresOriginAuthentication(spec);
        spec.originAuthenticationRequired = task.originAuthenticationRequired;
        spec.proxyAuthenticationRequired =
            spec.proxyAuthenticationRequired || !spec.proxyUsername.empty() || !spec.proxyPassword.empty();
        if (const auto message = validateSpec(spec); !message.empty())
            throw std::invalid_argument(message);
        spec.kind = sourceKind(spec);
        if (!requestId.empty() && !isOpaqueId(requestId))
            throw std::invalid_argument("Invalid request ID.");
        if (task.ownerId != "standalone" && !contexts->enabled(task.ownerId))
            throw std::invalid_argument("Download context is unavailable.");
        const auto name = spec.destination.filename().u8string();
        task.requestName.assign(name.begin(), name.end());
        if (!requestId.empty()) {
            for (const auto& [id, existing] : tasks) {
                const bool keyMatches = existing.requestId == requestId ||
                                        (legacy && existing.requestId.empty() && id == requestId);
                if (existing.ownerId != task.ownerId || !keyMatches)
                    continue;
                if (existing.spec.url != spec.url || existing.spec.kind != spec.kind ||
                    existing.contextRevision != task.contextRevision || existing.spec.proxy != spec.proxy ||
                    existing.proxyCredentialRef != task.proxyCredentialRef ||
                    (!existing.requestName.empty() && existing.requestName != task.requestName) ||
                    existing.originAuthenticationRequired != task.originAuthenticationRequired)
                    throw std::invalid_argument("Request ID belongs to another download request.");
                return id;
            }
            if (legacy && tasks.contains(requestId))
                throw std::invalid_argument("Request ID is unavailable.");
        }
        const bool unsupportedBudget =
            queueRegistry->get(task.queueId).dailyPayloadBudget > 0 &&
            (spec.kind == TransferKind::BitTorrent || spec.kind == TransferKind::Media);
        if (unsupportedBudget && spec.kind == TransferKind::BitTorrent && spec.url.starts_with("torrent:"))
            throw std::invalid_argument("Choose an unbudgeted queue before importing a local torrent. Native "
                                        "budgets support HTTP/FTP only.");
        if (!unsupportedBudget)
            loadTorrentSource(spec, factory);
        if (!unsupportedBudget && spec.kind == TransferKind::BitTorrent) {
            task.torrent = factory()->describe(spec);
            validateSelection(spec, task.torrent);
        }
        spec.destination = std::filesystem::absolute(spec.destination).lexically_normal();
        const auto original = spec.destination;
        const auto reserved = [&](const auto& path) {
            return std::filesystem::exists(path) ||
                   std::any_of(tasks.begin(), tasks.end(), [&](const auto& pair) {
                       return !isTerminal(pair.second.state) && pair.second.spec.destination == path;
                   });
        };
        for (int n = 1; reserved(spec.destination); ++n) {
            if (n > 10000)
                throw std::runtime_error("Too many conflicting file names.");
            spec.destination =
                original.parent_path() / (original.stem().wstring() + L" (" + std::to_wstring(n) + L")" +
                                          original.extension().wstring());
        }
        task.id = legacy && !requestId.empty() ? requestId : newTaskId();
        task.requestId = std::move(requestId);
        task.createdAt = unixTime();
        task.updatedAt = task.createdAt;
        task.runIntent = spec.startPaused ? RunIntent::Paused : RunIntent::Run;
        task.state = spec.startPaused ? TaskState::Paused
                                      : (spec.scheduledAt > queueRegistry->now() ? TaskState::Scheduled
                                                                                 : TaskState::Queued);
        if (spec.kind == TransferKind::BitTorrent && task.torrent.files && !spec.torrent.allFiles &&
            spec.torrent.selectedFiles.empty())
            task.state = TaskState::AwaitingSelection;
        if (unsupportedBudget) {
            task.state = TaskState::NeedsAttention;
            task.error = "Daily payload budgets support native HTTP/FTP tasks only. Move this task to an "
                         "unbudgeted queue.";
        }
        if (rememberProxy)
            contexts->rememberTask(task);
        try {
            save(task);
        } catch (...) {
            if (rememberProxy && !task.proxyCredentialRef.empty()) {
                try {
                    contexts->discardUncommitted(task);
                } catch (...) {
                    // Durable ownership journal remains available for exact cleanup on reopen.
                }
            }
            throw;
        }
        const auto id = task.id;
        tasks.emplace(id, std::move(task));
        publish();
        return id;
    }
    bool releaseUnusedReference(const Task& oldTask) {
        const auto& reference = oldTask.proxyCredentialRef;
        if (reference.empty())
            return true;
        for (const auto& [id, task] : tasks)
            if (task.proxyCredentialRef == reference)
                return true;
        for (const auto& context : contexts->list())
            if (context.proxyCredentialRef == reference)
                return true;
        try {
            contexts->discardUncommitted(oldTask);
            return true;
        } catch (...) {
            return false;
        }
    }
    bool updateStandalone(const std::string& id, DownloadSpec spec, bool rememberProxy,
                          bool preserveRequired) {
        const auto found = tasks.find(id);
        if (!healthy || found == tasks.end() || found->second.ownerId != "standalone" || jobs.contains(id) ||
            isTerminal(found->second.state))
            throw std::invalid_argument("Pause an unfinished standalone task before editing its request.");
        const auto old = found->second;
        if (old.spec.kind == TransferKind::BitTorrent || isTorrentUrl(spec.url)) {
            if (old.spec.kind != TransferKind::BitTorrent || spec.url != old.spec.url)
                throw std::invalid_argument("Create a new task when changing a torrent source.");
            spec.torrent.metadata = old.spec.torrent.metadata;
            if (old.torrentPublished && (spec.torrent.allFiles != old.spec.torrent.allFiles ||
                                         spec.torrent.selectedFiles != old.spec.torrent.selectedFiles))
                throw std::invalid_argument("Published torrent selection cannot be changed.");
        }
        spec.destination = old.spec.destination;
        spec.proxyAuthenticationRequired =
            spec.proxyAuthenticationRequired || !spec.proxyUsername.empty() || !spec.proxyPassword.empty();
        if (const auto validationError = validateSpec(spec); !validationError.empty())
            throw std::invalid_argument(validationError);
        spec.kind = sourceKind(spec);
        if (spec.kind == TransferKind::BitTorrent)
            validateSelection(spec, old.torrent);
        auto candidate = old;
        candidate.spec = std::move(spec);
        candidate.originAuthenticationRequired = (preserveRequired && old.originAuthenticationRequired) ||
                                                 requiresOriginAuthentication(candidate.spec);
        candidate.spec.originAuthenticationRequired = candidate.originAuthenticationRequired;
        if (candidate.spec.url != old.spec.url)
            candidate.originGrantId.clear();
        const bool supplied = !candidate.spec.proxyUsername.empty() || !candidate.spec.proxyPassword.empty();
        candidate.proxyCredentialRef.clear();
        bool created = false;
        if (rememberProxy) {
            if (supplied) {
                contexts->rememberTask(candidate);
                created = true;
            } else if (!old.proxyCredentialRef.empty() &&
                       sameProxyRoute(old.spec.proxy, candidate.spec.proxy)) {
                candidate.proxyCredentialRef = old.proxyCredentialRef;
                candidate.spec.proxyAuthenticationRequired = true;
            } else
                throw std::invalid_argument("Enter proxy credentials before choosing Remember.");
        }
        candidate.runIntent = RunIntent::Paused;
        candidate.state = TaskState::Paused;
        candidate.error.clear();
        try {
            save(candidate);
        } catch (...) {
            if (created)
                try {
                    contexts->discardUncommitted(candidate);
                } catch (...) {
                }
            throw;
        }
        found->second = std::move(candidate);
        publish();
        if (old.spec.kind != found->second.spec.kind)
            clearOwnedParts(old);
        return releaseUnusedReference(old);
    }
    bool control(const std::string& id, ScopedTaskCommand command) {
        auto it = tasks.find(id);
        if (it == tasks.end())
            return false;
        auto& task = it->second;
        switch (command) {
        case ScopedTaskCommand::Pause:
            task.runIntent = RunIntent::Paused;
            if (jobs.contains(id)) {
                jobs[id]->control->pause = true;
                task.state = TaskState::Pausing;
                save(task);
            } else if (!isTerminal(task.state)) {
                task.state = TaskState::Paused;
                save(task);
            }
            return true;
        case ScopedTaskCommand::Resume:
            if (jobs.contains(id) || isTerminal(task.state))
                return false;
            if (!contexts->enabled(task.ownerId))
                return false;
            task.runIntent = RunIntent::Run;
            task.retryAt = 0;
            task.spec.scheduledAt = 0;
            task.error.clear();
            task.retries = 0;
            if (task.spec.stopAt <= queueRegistry->now())
                task.spec.stopAt = 0;
            task.state = TaskState::Queued;
            save(task);
            return true;
        case ScopedTaskCommand::Cancel:
            task.runIntent = RunIntent::Paused;
            if (isTerminal(task.state))
                return true;
            if (jobs.contains(id)) {
                jobs[id]->control->cancel = true;
                task.state = TaskState::Cancelling;
                save(task);
            } else {
                task.state = TaskState::Cancelled;
                save(task);
                clearOwnedParts(task);
            }
            return true;
        case ScopedTaskCommand::Remove:
            if (jobs.contains(id))
                return false;
            repository->erase(id);
            clearOwnedParts(task);
            tasks.erase(it);
            return true;
        }
        return false;
    }
    void completeRevocations() {
        for (auto it = pendingRevocations.begin(); it != pendingRevocations.end();) {
            const auto owner = it->first;
            const bool running = std::any_of(jobs.begin(), jobs.end(), [&](const auto& entry) {
                return tasks.at(entry.first).ownerId == owner;
            });
            if (running) {
                ++it;
                continue;
            }
            try {
                contexts->finishRevoke(owner);
                for (auto& [id, task] : tasks) {
                    if (task.ownerId == owner) {
                        task.originGrantId.clear();
                        task.spec.cookie.clear();
                        task.spec.authorization.clear();
                        task.spec.proxyUsername.clear();
                        task.spec.proxyPassword.clear();
                    }
                }
                for (auto& promise : it->second)
                    promise->set_value();
            } catch (...) {
                for (auto& promise : it->second)
                    promise->set_exception(std::current_exception());
            }
            it = pendingRevocations.erase(it);
        }
    }

    void post(std::function<void()> command) {
        {
            std::lock_guard lock(mutex);
            commands.push_back(std::move(command));
        }
        cv.notify_one();
    }
    void save(Task& task) {
        task.originAuthenticationRequired =
            task.originAuthenticationRequired || requiresOriginAuthentication(task.spec);
        task.spec.originAuthenticationRequired = task.originAuthenticationRequired;
        task.updatedAt = unixTime();
        repository->save(task);
    }
    void clearOwnedParts(const Task& task) {
        const auto folder = workDirectory / task.id;
        std::error_code ignored;
        // Completed tasks usually had their cache reclaimed already. Avoid dozens
        // of redundant delete requests per record during bulk history removal.
        if (std::filesystem::exists(folder, ignored)) {
            // Enumerate existing bounded names instead of issuing 1,026 probes per history row.
            for (auto it = std::filesystem::directory_iterator(folder, ignored);
                 !ignored && it != std::filesystem::directory_iterator{}; it.increment(ignored))
                if (ownedRecoveryArtifact(it->path())) {
                    std::error_code removeError;
                    std::filesystem::remove(it->path(), removeError);
                }
            std::filesystem::remove(folder, ignored);
            std::filesystem::remove_all(folder / "media", ignored);
            std::filesystem::remove(folder, ignored);
        }
        // A damaged persisted specification must never turn cleanup into a truncated
        // or unintended filesystem path. The task cache above is owned by validated ID.
        if (!validateSpec(task.spec).empty())
            return;
        if (task.spec.kind == TransferKind::BitTorrent) {
            try {
                factory()->discard(task, folder);
            } catch (const std::exception&) {
                std::lock_guard lock(mutex);
                error = "Some torrent recovery files could not be removed. Published files were preserved.";
            }
            std::filesystem::remove(folder, ignored);
            return;
        }
        auto stage = task.spec.destination;
        stage += L".maskpilot-" + std::filesystem::path(task.id).wstring() + L".tmp";
        std::filesystem::remove(stage, ignored);
    }
    void publish() {
        std::vector<Task> copy;
        for (const auto& [id, task] : tasks)
            copy.push_back(task);
        std::sort(copy.begin(), copy.end(), [](const Task& a, const Task& b) {
            return a.createdAt == b.createdAt ? a.id < b.id : a.createdAt > b.createdAt;
        });
        std::lock_guard lock(mutex);
        published = std::move(copy);
        contextWarning = contexts->warning();
    }
    void processEvent(const std::string& id, std::uint64_t generation, TransferEvent event) {
        auto it = tasks.find(id);
        if (it == tasks.end() || it->second.generation != generation)
            return;
        auto& task = it->second;
        task.downloaded = event.downloaded;
        task.total = event.total;
        task.bytesPerSecond = event.speed;
        task.resumable = event.resumable;
        task.uploaded = std::max(task.uploaded, event.uploaded);
        task.uploadSpeed = event.uploadSpeed;
        task.peers = event.peers;
        task.seeds = event.seeds;
        const bool newlyPublished = !task.torrentPublished && event.torrentPublished;
        task.torrentPublished = task.torrentPublished || event.torrentPublished;
        if (event.description)
            task.torrent = *event.description;
        if (event.torrentMetadata && !task.spec.torrent.metadata) {
            if (event.torrentMetadata->empty() || event.torrentMetadata->size() > maxTorrentMetadataBytes)
                throw std::runtime_error("Invalid received torrent metadata size.");
            task.spec.torrent.metadata = std::move(event.torrentMetadata);
            save(task);
        }
        if (event.outcome == TransferOutcome::Progress) {
            if (task.state != TaskState::Pausing && task.state != TaskState::Cancelling)
                task.state = event.phase;
            if (newlyPublished)
                save(task); // Seeding can continue indefinitely; publication must survive a crash now.
            // Transfer metadata, not progress counters, determines resumable bytes after a crash.
            return;
        }
        auto budget = jobs.at(id)->control->payloadBudget;
        const bool policyPause = jobs.at(id)->policyPause || (budget && budget->exhausted());
        jobs.erase(id); // The worker has enqueued its terminal event and no longer uses service state.
        if (budget) {
            budget->finish();
            if (budget->failed()) {
                event.outcome = TransferOutcome::Attention;
                event.error = "Payload accounting failed. Downloads paused to protect the daily allowance.";
                healthy = false;
                std::lock_guard lock(mutex);
                error = event.error;
            }
        }
        task.bytesPerSecond = 0;
        task.uploadSpeed = 0;
        task.error = std::move(event.error);
        switch (event.outcome) {
        case TransferOutcome::Complete:
            task.state = TaskState::Completed;
            break;
        case TransferOutcome::Paused:
            task.state = task.runIntent == RunIntent::Run && policyPause
                             ? (budget && budget->exhausted()
                                    ? TaskState::PolicyWaiting
                                    : (queueRegistry->available(task.queueId, queueRegistry->now())
                                           ? TaskState::Queued
                                           : TaskState::PolicyWaiting))
                             : TaskState::Paused;
            if (task.state == TaskState::Paused)
                task.runIntent = RunIntent::Paused;
            break;
        case TransferOutcome::Cancelled:
            task.state = TaskState::Cancelled;
            break;
        case TransferOutcome::Attention:
            task.state = TaskState::NeedsAttention;
            break;
        case TransferOutcome::SelectionRequired:
            task.state = TaskState::AwaitingSelection;
            break;
        case TransferOutcome::Failed:
            if (event.retryable && task.retries < task.spec.maxRetries && !stopping) {
                ++task.retries;
                task.retryAt =
                    queueRegistry->now() + std::max(event.retryAfter, std::min(60, 1 << task.retries));
                task.state = TaskState::RetryWaiting;
            } else
                task.state = TaskState::Failed;
            break;
        default:
            break;
        }
        if (budget && budget->exhausted() && task.state == TaskState::PolicyWaiting)
            task.error =
                "Daily payload allowance exhausted or UTC period changed. Waiting for available allowance.";
        save(task);
        if (isTerminal(task.state))
            clearOwnedParts(task);
    }
    std::vector<RateClaim> claims(const Task* candidate = nullptr, std::int64_t reservation = -1,
                                  bool upload = false) const {
        std::vector<RateClaim> result;
        for (const auto& [id, job] : jobs) {
            const auto& t = tasks.at(id);
            if (upload && t.spec.kind != TransferKind::BitTorrent)
                continue;
            result.push_back({id, t.queueId, upload ? t.spec.torrent.uploadLimit : t.spec.speedLimit,
                              upload ? -1 : job->mediaRate});
        }
        if (candidate && (!upload || candidate->spec.kind == TransferKind::BitTorrent))
            result.push_back({candidate->id, candidate->queueId,
                              upload ? candidate->spec.torrent.uploadLimit : candidate->spec.speedLimit,
                              upload ? -1 : reservation});
        return result;
    }
    void validateLimits(int count, std::int64_t down, std::int64_t up,
                        const std::vector<DownloadQueue>& policies) const {
        if (count < 1 || count > 8 || down < 0 || up < 0 || up > 2147483647LL)
            throw std::invalid_argument("Invalid global download limits.");
        allocateQueueRates(down, policies, claims());
        allocateQueueRates(up, policies, claims(nullptr, -1, true), true);
    }
    void schedule() {
        if (!healthy || stopping)
            return;
        const auto now = queueRegistry->now();
        for (auto& [id, t] : tasks) {
            const bool available = queueRegistry->available(t.queueId, now);
            if (jobs.contains(id) && t.runIntent == RunIntent::Run && !available &&
                !jobs.at(id)->control->pause && !jobs.at(id)->control->cancel) {
                jobs.at(id)->policyPause = true;
                jobs.at(id)->control->pause = true;
                t.state = TaskState::Pausing;
                save(t);
            }
            if (t.runIntent == RunIntent::Run &&
                (t.state == TaskState::Queued || t.state == TaskState::Scheduled ||
                 t.state == TaskState::RetryWaiting || t.state == TaskState::PolicyWaiting)) {
                auto desired = !available
                                   ? TaskState::PolicyWaiting
                                   : (t.spec.scheduledAt > now
                                          ? TaskState::Scheduled
                                          : (t.retryAt > now ? TaskState::RetryWaiting : TaskState::Queued));
                // A bandwidth-held task remains waiting until admission succeeds. Persisting
                // Queued first would alternate two FULL database writes on every actor tick.
                if (t.state != desired &&
                    !(t.state == TaskState::PolicyWaiting && desired == TaskState::Queued)) {
                    t.state = desired;
                    save(t);
                }
            }
            if (jobs.contains(id) && t.spec.stopAt > 0 && now >= t.spec.stopAt) {
                t.runIntent = RunIntent::Paused;
                jobs.at(id)->control->pause = true;
                t.state = TaskState::Pausing;
                save(t);
            }
        }
        std::set<std::string> attempted;
        std::map<std::string, TrafficUsage> exhaustedQueues;
        while (jobs.size() < static_cast<std::size_t>(concurrentTasks)) {
            std::map<std::string, Task*> candidates;
            for (auto& [id, t] : tasks) {
                const bool retryHeld =
                    t.state == TaskState::PolicyWaiting && t.spec.scheduledAt <= now && t.retryAt <= now;
                if ((t.state != TaskState::Queued && !retryHeld) || t.runIntent != RunIntent::Run ||
                    jobs.contains(id) || attempted.contains(id) || !contexts->enabled(t.ownerId))
                    continue;
                const auto& q = queueRegistry->get(t.queueId);
                if (!queueRegistry->available(t.queueId, now))
                    continue;
                auto active = std::count_if(jobs.begin(), jobs.end(), [&](const auto& item) {
                    return tasks.at(item.first).queueId == t.queueId;
                });
                if (active >= q.maxActive)
                    continue;
                auto found = candidates.find(t.queueId);
                if (found == candidates.end() || t.spec.priority > found->second->spec.priority ||
                    (t.spec.priority == found->second->spec.priority &&
                     (t.createdAt < found->second->createdAt ||
                      (t.createdAt == found->second->createdAt && t.id < found->second->id))))
                    candidates[t.queueId] = &t;
            }
            if (candidates.empty())
                break;
            auto next = candidates.upper_bound(lastQueue);
            if (next == candidates.end())
                next = candidates.begin();
            auto* task = next->second;
            lastQueue = next->first;
            attempted.insert(task->id);
            if (auto message = validateSpec(task->spec); !message.empty()) {
                task->state = TaskState::NeedsAttention;
                task->error = "Invalid saved task: " + message;
                save(*task);
                continue;
            }
            const auto& policy = queueRegistry->get(task->queueId);
            if (policy.dailyPayloadBudget > 0) {
                if (task->spec.kind == TransferKind::BitTorrent || task->spec.kind == TransferKind::Media) {
                    task->state = TaskState::NeedsAttention;
                    task->error = "Daily payload budgets support native HTTP/FTP tasks only. Move this task "
                                  "to an unbudgeted queue.";
                    save(*task);
                    continue;
                }
                TrafficUsage usage;
                try {
                    const auto cached = exhaustedQueues.find(task->queueId);
                    usage =
                        cached == exhaustedQueues.end() ? traffic->usage(task->queueId, now) : cached->second;
                } catch (const std::exception&) {
                    task->state = TaskState::NeedsAttention;
                    task->error =
                        "Payload accounting failed. Downloads paused to protect the daily allowance.";
                    save(*task);
                    healthy = false;
                    {
                        std::lock_guard lock(mutex);
                        error = task->error;
                    }
                    break;
                }
                if (usage.charged >= policy.dailyPayloadBudget) {
                    exhaustedQueues.insert_or_assign(task->queueId, usage);
                    if (task->state != TaskState::PolicyWaiting) {
                        task->state = TaskState::PolicyWaiting;
                        task->error = "Daily payload allowance exhausted. Waiting for the next UTC day or a "
                                      "policy change.";
                        save(*task);
                    }
                    continue;
                }
            }
            DownloadSpec resolved;
            try {
                resolved = contexts->resolve(*task);
            } catch (const std::exception&) {
                task->state = TaskState::NeedsAttention;
                task->error =
                    "Download context or required credentials are unavailable. Rebind the paused task.";
                save(*task);
                continue;
            }
            if (task->spec.stopAt > 0 && now >= task->spec.stopAt) {
                task->runIntent = RunIntent::Paused;
                task->state = TaskState::Paused;
                save(*task);
                continue;
            }
            auto job = std::make_unique<Job>();
            auto control = job->control;
            try {
                if (task->spec.kind == TransferKind::Media)
                    job->mediaRate =
                        mediaReservation(globalLimit, concurrentTasks, queueRegistry->get(task->queueId),
                                         task->spec.speedLimit);
                const auto rates =
                    allocateQueueRates(globalLimit, queueRegistry->list(), claims(task, job->mediaRate));
                const auto uploads = allocateQueueRates(globalUploadLimit, queueRegistry->list(),
                                                        claims(task, job->mediaRate, true), true);
                control->speedLimit = rates.at(task->id);
                if (uploads.contains(task->id))
                    control->uploadLimit = uploads.at(task->id);
            } catch (const std::invalid_argument&) {
                if (task->state != TaskState::PolicyWaiting) {
                    task->state = TaskState::PolicyWaiting;
                    save(*task);
                }
                continue;
            }
            if (policy.dailyPayloadBudget > 0) {
                try {
                    control->payloadBudget =
                        makeNativePayloadBudget(traffic, task->id, task->queueId,
                                                [registry = queueRegistry.get()] { return registry->now(); });
                } catch (const std::exception&) {
                    task->state = TaskState::NeedsAttention;
                    task->error =
                        "Payload accounting failed. Downloads paused to protect the daily allowance.";
                    save(*task);
                    healthy = false;
                    {
                        std::lock_guard lock(mutex);
                        error = task->error;
                    }
                    break;
                }
            }
            task->state = TaskState::Probing;
            task->error.clear();
            ++task->generation;
            save(*task);
            auto copy = *task;
            copy.spec = std::move(resolved);
            auto path = workDirectory / task->id;
            auto transport = factory();
            job->thread =
                std::jthread([this, copy, path, control, transport = std::move(transport)]() mutable {
                    auto callback = [this, id = copy.id, generation = copy.generation](TransferEvent event) {
                        post([this, id, generation, event = std::move(event)]() mutable {
                            processEvent(id, generation, std::move(event));
                        });
                    };
                    try {
                        transport->run(copy, path, *control, callback);
                    } catch (const std::exception&) {
                        TransferEvent e;
                        e.outcome = TransferOutcome::Failed;
                        e.error = "Transfer failed unexpectedly. Inspect destination access and retry.";
                        callback(std::move(e));
                    }
                });
            jobs[task->id] = std::move(job);
        }
        const auto rates = allocateQueueRates(globalLimit, queueRegistry->list(), claims());
        const auto uploads =
            allocateQueueRates(globalUploadLimit, queueRegistry->list(), claims(nullptr, -1, true), true);
        for (auto& [id, job] : jobs) {
            if (job->mediaRate < 0)
                job->control->speedLimit = rates.at(id);
            if (uploads.contains(id))
                job->control->uploadLimit = uploads.at(id);
        }
    }
    void loop(std::promise<void> ready) {
        try {
            if (traffic)
                traffic->abandonOpenRuns();
            for (auto task : repository->load()) {
                queueRegistry->get(task.queueId);
                const bool unsupportedBudget =
                    queueRegistry->get(task.queueId).dailyPayloadBudget > 0 &&
                    (task.spec.kind == TransferKind::BitTorrent || task.spec.kind == TransferKind::Media);
                if (unsupportedBudget && !isTerminal(task.state)) {
                    task.state = TaskState::NeedsAttention;
                    task.error = "Daily payload budgets support native HTTP/FTP tasks only. Move this task "
                                 "to an unbudgeted queue.";
                    save(task);
                }
                if (!unsupportedBudget && task.spec.kind == TransferKind::BitTorrent) {
                    try {
                        task.torrent = factory()->describe(task.spec);
                        validateSelection(task.spec, task.torrent);
                    } catch (const std::exception&) {
                        task.state = TaskState::NeedsAttention;
                        task.error = "Saved torrent metadata or file selection is invalid.";
                        save(task);
                    }
                }
                if (isActive(task.state) || task.state == TaskState::Queued ||
                    task.state == TaskState::RetryWaiting || task.state == TaskState::PolicyWaiting) {
                    if (task.runIntent == RunIntent::Run &&
                        (!queueRegistry->available(task.queueId, queueRegistry->now()) ||
                         (task.state == TaskState::PolicyWaiting &&
                          queueRegistry->get(task.queueId).dailyPayloadBudget > 0))) {
                        if (task.state != TaskState::PolicyWaiting) {
                            task.state = TaskState::PolicyWaiting;
                            save(task);
                        }
                    } else {
                        task.runIntent = RunIntent::Paused;
                        task.state = TaskState::Paused;
                        task.error =
                            "Recovered after exit. Resume to validate local parts and remote identity.";
                        save(task);
                    }
                }
                tasks.emplace(task.id, std::move(task));
            }
            contexts->initialize(allTasks());
            publish();
            ready.set_value();
        } catch (...) {
            ready.set_exception(std::current_exception());
            return;
        }
        for (;;) {
            std::deque<std::function<void()>> pending;
            bool shouldStop;
            {
                std::unique_lock lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(100), [this] { return !commands.empty(); });
                pending.swap(commands);
                shouldStop = stopping;
            }
            for (auto& command : pending) {
                try {
                    command();
                } catch (const std::exception&) {
                    healthy = false;
                    std::lock_guard lock(mutex);
                    error = "Task storage operation failed. Downloads paused to protect persistent state.";
                }
            }
            if (shouldStop || !healthy)
                for (auto& [id, job] : jobs)
                    job->control->pause = true;
            try {
                schedule();
            } catch (const std::exception&) {
                healthy = false;
                std::lock_guard lock(mutex);
                error = "Unable to persist task state. Check available disk space.";
            }
            completeRevocations();
            publish();
            if (shouldStop && jobs.empty())
                break;
        }
    }
};

DownloadService::DownloadService(std::unique_ptr<ITaskRepository> repository, TransportFactory factory,
                                 std::filesystem::path workDirectory, ContextDependencies contexts,
                                 QueueDependencies queues)
    : impl_(std::make_unique<Impl>()) {
    impl_->repository = std::move(repository);
    impl_->factory = std::move(factory);
    impl_->workDirectory = std::move(workDirectory);
    impl_->contexts = std::make_unique<ContextRegistry>(std::move(contexts));
    const auto initialLimits = queues.initialLimits;
    impl_->traffic = queues.traffic;
    impl_->queueRegistry = std::make_unique<QueueRegistry>(std::move(queues));
    if (!impl_->traffic)
        for (const auto& policy : impl_->queueRegistry->list())
            if (policy.dailyPayloadBudget > 0)
                throw std::invalid_argument("Saved queue payload budgets require persistent accounting.");
    if (initialLimits) {
        impl_->validateLimits(initialLimits->concurrentTasks, initialLimits->downloadLimit,
                              initialLimits->uploadLimit, impl_->queueRegistry->list());
        impl_->concurrentTasks = initialLimits->concurrentTasks;
        impl_->globalLimit = initialLimits->downloadLimit;
        impl_->globalUploadLimit = initialLimits->uploadLimit;
    }
    std::promise<void> ready;
    auto result = ready.get_future();
    impl_->coordinator =
        std::jthread([p = impl_.get(), ready = std::move(ready)]() mutable { p->loop(std::move(ready)); });
    result.get();
}
DownloadService::~DownloadService() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->cv.notify_one();
    if (impl_->coordinator.joinable())
        impl_->coordinator.join();
}
std::future<std::string> DownloadService::add(DownloadSpec spec, std::string requestId, bool rememberProxy,
                                              std::string queueId) {
    return impl_->request([p = impl_.get(), spec = std::move(spec), requestId = std::move(requestId),
                           rememberProxy, queueId = std::move(queueId)]() mutable {
        Task task;
        task.queueId = std::move(queueId);
        task.spec = std::move(spec);
        return p->addPrepared(std::move(task), std::move(requestId), true, rememberProxy);
    });
}
void DownloadService::pause(const std::string& id) {
    impl_->post([p = impl_.get(), id] { p->control(id, ScopedTaskCommand::Pause); });
}
void DownloadService::resume(const std::string& id) {
    impl_->post([p = impl_.get(), id] { p->control(id, ScopedTaskCommand::Resume); });
}
void DownloadService::cancel(const std::string& id) {
    impl_->post([p = impl_.get(), id] { p->control(id, ScopedTaskCommand::Cancel); });
}
void DownloadService::remove(const std::string& id) {
    impl_->post([p = impl_.get(), id] { p->control(id, ScopedTaskCommand::Remove); });
}
std::future<DownloadContext> DownloadService::configureContext(ContextConfiguration configuration,
                                                               ProxyCredentials credentials,
                                                               bool rememberProxy) {
    return impl_->request([p = impl_.get(), configuration = std::move(configuration),
                           credentials = std::move(credentials), rememberProxy]() mutable {
        if (!p->healthy || p->pendingRevocations.contains(configuration.id))
            throw std::runtime_error("Context configuration is currently unavailable.");
        return p->contexts->configure(std::move(configuration), std::move(credentials), rememberProxy);
    });
}
std::future<std::vector<DownloadContext>> DownloadService::contexts() {
    return impl_->request([p = impl_.get()] { return p->contexts->list(); });
}
std::future<std::string> DownloadService::issueClient(const std::string& ownerId) {
    return impl_->request([p = impl_.get(), ownerId] { return p->contexts->issueClient(ownerId); });
}
std::future<std::string> DownloadService::issueOriginGrant(const std::string& ownerId, std::string url,
                                                           std::string cookie, std::string authorization,
                                                           std::int64_t expiresAt) {
    return impl_->request([p = impl_.get(), ownerId, url = std::move(url), cookie = std::move(cookie),
                           authorization = std::move(authorization), expiresAt]() mutable {
        return p->contexts->issueGrant(ownerId, std::move(url), std::move(cookie), std::move(authorization),
                                       expiresAt);
    });
}
std::future<void> DownloadService::revokeContext(const std::string& ownerId) {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    impl_->post([p = impl_.get(), ownerId, promise] {
        try {
            p->contexts->beginRevoke(ownerId);
            for (auto& [id, job] : p->jobs) {
                if (p->tasks.at(id).ownerId == ownerId)
                    job->control->pause = true;
            }
            for (auto& [id, task] : p->tasks) {
                if (task.ownerId != ownerId || isTerminal(task.state))
                    continue;
                task.runIntent = RunIntent::Paused;
                task.state = p->jobs.contains(id) ? TaskState::Pausing : TaskState::Paused;
                p->save(task);
            }
            p->pendingRevocations[ownerId].push_back(promise);
            p->completeRevocations();
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    return future;
}
std::future<void> DownloadService::rebindTask(const std::string& taskId, std::string grantId) {
    return impl_->request([p = impl_.get(), taskId, grantId = std::move(grantId)] {
        const auto found = p->tasks.find(taskId);
        if (found == p->tasks.end() || p->jobs.contains(taskId) || isTerminal(found->second.state))
            throw std::invalid_argument("Pause an unfinished task before rebinding it.");
        auto task = found->second;
        p->contexts->rebind(task, grantId);
        task.runIntent = RunIntent::Paused;
        task.state = TaskState::Paused;
        task.error.clear();
        p->save(task);
        found->second = std::move(task);
        p->publish();
    });
}
std::future<std::string> DownloadService::addScoped(std::string token, ScopedDownloadDraft draft,
                                                    std::string requestId) {
    return impl_->request([p = impl_.get(), token = std::move(token), draft = std::move(draft),
                           requestId = std::move(requestId)]() mutable {
        auto task = p->contexts->prepareScoped(token, draft);
        if (!isOpaqueId(requestId))
            throw std::invalid_argument("Invalid request ID.");
        return p->addPrepared(std::move(task), std::move(requestId), false, false);
    });
}
std::future<ScopedTaskPage> DownloadService::listScoped(std::string token, std::size_t limit,
                                                        std::string afterId) {
    return impl_->request([p = impl_.get(), token = std::move(token), limit, afterId = std::move(afterId)] {
        const auto owner = p->contexts->ownerFor(token);
        if (limit < 1 || limit > 1000)
            throw std::invalid_argument("Invalid page limit.");
        if (!afterId.empty()) {
            const auto cursor = p->tasks.find(afterId);
            if (cursor == p->tasks.end() || cursor->second.ownerId != owner)
                throw std::invalid_argument("Invalid page cursor.");
        }
        ScopedTaskPage page;
        for (const auto& [id, task] : p->tasks) {
            if (task.ownerId != owner || (!afterId.empty() && id <= afterId))
                continue;
            if (page.tasks.size() == limit) {
                page.nextCursor = page.tasks.back().id;
                break;
            }
            const auto filename = task.spec.destination.filename().u8string();
            page.tasks.push_back(
                {id, std::string(filename.begin(), filename.end()), task.state, task.downloaded, task.total});
        }
        return page;
    });
}
std::future<void> DownloadService::controlScoped(std::string token, ScopedTaskCommand command,
                                                 std::string taskId) {
    return impl_->request([p = impl_.get(), token = std::move(token), command, taskId = std::move(taskId)] {
        const auto owner = p->contexts->ownerFor(token);
        const auto found = p->tasks.find(taskId);
        if (found == p->tasks.end() || found->second.ownerId != owner)
            throw std::invalid_argument("Task was not found.");
        if (!p->control(taskId, command))
            throw std::invalid_argument("Task cannot perform this operation in its current state.");
        p->publish();
    });
}
std::future<void> DownloadService::configureLimits(int count, std::int64_t down, std::int64_t up) {
    return impl_->request([p = impl_.get(), count, down, up] {
        p->validateLimits(count, down, up, p->queueRegistry->list());
        p->concurrentTasks = count;
        p->globalLimit = down;
        p->globalUploadLimit = up;
    });
}
void DownloadService::setLimits(int count, std::int64_t down, std::int64_t up) {
    // Compatibility API intentionally ignores ordinary validation denials; checked callers await
    // configureLimits.
    (void)configureLimits(count, down, up);
}
std::future<std::vector<DownloadQueue>> DownloadService::queues() {
    return impl_->request([p = impl_.get()] { return p->queueRegistry->list(); });
}
std::future<TrafficUsage> DownloadService::queueTraffic(std::string queueId) {
    return impl_->request([p = impl_.get(), queueId = std::move(queueId)] {
        p->queueRegistry->get(queueId);
        if (!p->traffic)
            return TrafficUsage{};
        return p->traffic->usage(queueId, p->queueRegistry->now());
    });
}
std::future<DownloadQueue> DownloadService::configureQueue(QueueConfiguration config) {
    return impl_->request([p = impl_.get(), config = std::move(config)]() mutable {
        auto candidate = p->queueRegistry->prepare(config);
        if (candidate.dailyPayloadBudget > 0 && !p->traffic)
            throw std::invalid_argument("Daily payload budgets require persistent accounting.");
        const auto existing = p->queueRegistry->list();
        auto old = std::find_if(existing.begin(), existing.end(),
                                [&](const auto& q) { return q.id == candidate.id; });
        if (old != existing.end() && old->dailyPayloadBudget != candidate.dailyPayloadBudget)
            for (const auto& [id, job] : p->jobs)
                if (p->tasks.at(id).queueId == candidate.id)
                    throw std::invalid_argument(
                        "Pause every task in this queue before changing its daily payload budget.");
        auto policies = p->queueRegistry->list();
        auto it = std::find_if(policies.begin(), policies.end(),
                               [&](const auto& q) { return q.id == candidate.id; });
        if (it == policies.end())
            policies.push_back(candidate);
        else
            *it = candidate;
        p->validateLimits(p->concurrentTasks, p->globalLimit, p->globalUploadLimit, policies);
        p->queueRegistry->commit(candidate, config.expectedRevision);
        if (!p->queueRegistry->available(candidate.id, p->queueRegistry->now()))
            for (auto& [id, t] : p->tasks)
                if (t.queueId == candidate.id && t.runIntent == RunIntent::Run) {
                    if (p->jobs.contains(id) && !p->jobs.at(id)->control->pause &&
                        !p->jobs.at(id)->control->cancel) {
                        p->jobs.at(id)->policyPause = true;
                        p->jobs.at(id)->control->pause = true;
                        t.state = TaskState::Pausing;
                        p->save(t);
                    } else if (t.state == TaskState::Queued || t.state == TaskState::Scheduled ||
                               t.state == TaskState::RetryWaiting) {
                        t.state = TaskState::PolicyWaiting;
                        p->save(t);
                    }
                }
        p->publish();
        return candidate;
    });
}
std::future<void> DownloadService::moveToQueue(std::vector<std::string> ids, std::string target) {
    return impl_->request([p = impl_.get(), ids = std::move(ids), target = std::move(target)] {
        p->queueRegistry->get(target);
        if (ids.empty() || ids.size() > 10000)
            throw std::invalid_argument("Choose tasks to move.");
        std::set<std::string> unique;
        std::vector<QueueMove> moves;
        for (const auto& id : ids) {
            auto it = p->tasks.find(id);
            if (!unique.insert(id).second || it == p->tasks.end() || p->jobs.contains(id) ||
                isActive(it->second.state))
                throw std::invalid_argument("Pause every selected task before moving queues.");
            moves.push_back({id, it->second.queueId});
        }
        const auto now = unixTime();
        p->queueRegistry->move(moves, target, now);
        for (const auto& id : ids) {
            p->tasks.at(id).queueId = target;
            p->tasks.at(id).updatedAt = now;
        }
        p->publish();
    });
}
std::future<void> DownloadService::removeQueue(std::string id, std::int64_t revision) {
    return impl_->request([p = impl_.get(), id = std::move(id), revision] {
        for (auto& [key, t] : p->tasks)
            if (t.queueId == id)
                throw std::invalid_argument("Move all tasks and history before removing a queue.");
        p->queueRegistry->remove(id, revision);
    });
}
void DownloadService::setPriority(const std::string& id, int priority) {
    impl_->post([p = impl_.get(), id, priority] {
        auto it = p->tasks.find(id);
        if (it != p->tasks.end()) {
            it->second.spec.priority = std::clamp(priority, -10, 10);
            p->save(it->second);
        }
    });
}
void DownloadService::updateUrl(const std::string& id, std::string url) {
    impl_->post([p = impl_.get(), id, url = std::move(url)] {
        auto it = p->tasks.find(id);
        if (it == p->tasks.end() || p->jobs.contains(id) || isTerminal(it->second.state))
            return;
        if (it->second.spec.kind == TransferKind::BitTorrent || isTorrentUrl(url))
            return; // A different torrent is a different task, never a refreshed file URL.
        auto spec = it->second.spec;
        spec.url = url;
        spec.cookie.clear();
        spec.authorization.clear();
        spec.headers.clear();
        spec.referrer.clear();
        spec.ftpUsername.clear();
        spec.ftpPassword.clear();
        if (!isFtpUrl(url)) {
            spec.ftpExplicitTls = false;
            spec.ftpActiveMode = false;
            spec.ftpCaBundle.clear();
        }
        if (!validateSpec(spec).empty())
            return;
        spec.kind = sourceKind(spec);
        if (it->second.spec.url != spec.url)
            it->second.originGrantId.clear();
        it->second.spec = std::move(spec);
        it->second.runIntent = RunIntent::Paused;
        it->second.state = TaskState::Paused;
        p->save(it->second);
    });
}
std::vector<Task> DownloadService::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->published;
}
void DownloadService::restart(const std::string& id) {
    impl_->post([p = impl_.get(), id] {
        auto it = p->tasks.find(id);
        if (it == p->tasks.end() || p->jobs.contains(id) || isTerminal(it->second.state))
            return;
        if (it->second.spec.kind == TransferKind::BitTorrent) {
            if (it->second.torrentPublished)
                return;
        }
        p->clearOwnedParts(it->second);
        auto& task = it->second;
        task.downloaded = 0;
        task.total = -1;
        task.retries = 0;
        task.error.clear();
        if (task.spec.stopAt <= p->queueRegistry->now())
            task.spec.stopAt = 0;
        task.runIntent = RunIntent::Run;
        task.retryAt = 0;
        task.spec.scheduledAt = 0;
        task.state = TaskState::Queued;
        p->save(task);
    });
}
void DownloadService::updateRequest(const std::string& id, DownloadSpec spec) {
    impl_->post([p = impl_.get(), id, spec = std::move(spec)]() mutable {
        try {
            const auto old = p->tasks.find(id);
            if (old == p->tasks.end() || old->second.ownerId != "standalone")
                return;
            const bool keepStored = !old->second.proxyCredentialRef.empty() && spec.proxyUsername.empty() &&
                                    spec.proxyPassword.empty() &&
                                    sameProxyRoute(old->second.spec.proxy, spec.proxy);
            p->updateStandalone(id, std::move(spec), keepStored, true);
        } catch (const std::invalid_argument&) {
            // Preserve the legacy fire-and-forget API's validation behavior.
        }
    });
}
std::future<void> DownloadService::updateRequestWithCredentials(const std::string& id, DownloadSpec spec,
                                                                bool rememberProxy) {
    return impl_->request([p = impl_.get(), id, spec = std::move(spec), rememberProxy]() mutable {
        if (!p->updateStandalone(id, std::move(spec), rememberProxy, false))
            throw std::runtime_error("Task updated, but prior native credential cleanup requires retry.");
    });
}
std::future<void> DownloadService::forgetSavedProxyCredentials(const std::string& id) {
    return impl_->request([p = impl_.get(), id] {
        const auto found = p->tasks.find(id);
        if (!p->healthy || found == p->tasks.end() || found->second.ownerId != "standalone" ||
            p->jobs.contains(id) || isTerminal(found->second.state))
            throw std::invalid_argument("Pause an unfinished standalone task before forgetting credentials.");
        const auto old = found->second;
        if (old.proxyCredentialRef.empty())
            throw std::invalid_argument("No saved proxy credentials are attached to this task.");
        auto candidate = old;
        candidate.proxyCredentialRef.clear();
        candidate.spec.proxyUsername.clear();
        candidate.spec.proxyPassword.clear();
        candidate.spec.proxyAuthenticationRequired = true;
        candidate.runIntent = RunIntent::Paused;
        candidate.state = TaskState::Paused;
        candidate.error.clear();
        p->save(candidate);
        found->second = std::move(candidate);
        const bool cleaned = p->releaseUnusedReference(old);
        p->publish();
        if (!cleaned)
            throw std::runtime_error(
                "Task binding was removed, but native credential cleanup requires retry.");
    });
}
std::future<void> DownloadService::retryCredentialCleanup() {
    return impl_->request([p = impl_.get()] {
        if (!p->jobs.empty())
            throw std::invalid_argument("Pause all tasks before retrying credential cleanup.");
        const bool complete = p->contexts->cleanupUnused(p->allTasks());
        p->publish();
        if (!complete)
            throw std::runtime_error("Some owned native credentials still require cleanup retry.");
    });
}
std::future<std::string> DownloadService::selectTorrentFiles(const std::string& id, std::vector<int> indices,
                                                             bool allFiles) {
    auto promise = std::make_shared<std::promise<std::string>>();
    auto result = promise->get_future();
    impl_->post([p = impl_.get(), id, indices = std::move(indices), allFiles, promise]() mutable {
        try {
            auto it = p->tasks.find(id);
            if (it == p->tasks.end() || p->jobs.contains(id) || isTerminal(it->second.state) ||
                it->second.spec.kind != TransferKind::BitTorrent || it->second.torrentPublished)
                throw std::invalid_argument("Pause an unpublished torrent before changing its files.");
            auto spec = it->second.spec;
            spec.torrent.allFiles = allFiles;
            spec.torrent.selectedFiles = allFiles ? std::vector<int>{} : std::move(indices);
            if (!allFiles && spec.torrent.selectedFiles.empty())
                throw std::invalid_argument("Select at least one file.");
            if (const auto error = validateSpec(spec); !error.empty())
                throw std::invalid_argument(error);
            validateSelection(spec, it->second.torrent);
            it->second.spec = std::move(spec);
            it->second.runIntent = RunIntent::Paused;
            it->second.state = TaskState::Paused;
            it->second.error.clear();
            p->save(it->second);
            p->publish();
            promise->set_value(id);
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    return result;
}
void DownloadService::stopSeeding(const std::string& id) {
    impl_->post([p = impl_.get(), id] {
        const auto it = p->tasks.find(id);
        if (it != p->tasks.end() && it->second.state == TaskState::Seeding && p->jobs.contains(id))
            p->jobs.at(id)->control->finish = true;
    });
}
std::future<std::vector<DownloadSpec>> DownloadService::prepareImport(std::vector<DownloadSpec> specs) {
    return std::async(std::launch::async, [factory = impl_->factory, specs = std::move(specs)]() mutable {
        if (specs.size() > 10000)
            throw std::invalid_argument("Task list exceeds 10000 entries.");
        std::size_t metadataBytes = 0;
        for (auto& spec : specs) {
            spec.proxyAuthenticationRequired = spec.proxyAuthenticationRequired ||
                                               !spec.proxyUsername.empty() || !spec.proxyPassword.empty();
            if (const auto error = validateSpec(spec); !error.empty())
                throw std::invalid_argument(error);
            spec.kind = sourceKind(spec);
            loadTorrentSource(spec, factory);
            if (spec.kind == TransferKind::BitTorrent) {
                if (spec.torrent.metadata)
                    metadataBytes += spec.torrent.metadata->size();
                if (metadataBytes > 24 * 1024 * 1024)
                    throw std::invalid_argument(
                        "Imported torrent metadata exceeds 24 MiB. Use smaller lists.");
                validateSelection(spec, factory()->describe(spec));
            }
            spec.startPaused = true;
        }
        return specs;
    });
}
void DownloadService::schedule(const std::string& id, std::int64_t startAt, std::int64_t stopAt) {
    impl_->post([p = impl_.get(), id, startAt, stopAt] {
        auto it = p->tasks.find(id);
        if (it == p->tasks.end() || p->jobs.contains(id) || isTerminal(it->second.state) || startAt < 0 ||
            (stopAt > 0 && stopAt <= startAt))
            return;
        auto& task = it->second;
        task.spec.scheduledAt = startAt;
        task.spec.stopAt = stopAt;
        task.runIntent = RunIntent::Run;
        task.retryAt = 0;
        task.state = startAt > p->queueRegistry->now() ? TaskState::Scheduled : TaskState::Queued;
        p->save(task);
    });
}
std::string DownloadService::lastError() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->error.empty() ? impl_->contextWarning : impl_->error;
}
} // namespace maskpilot
