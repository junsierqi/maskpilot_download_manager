#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <maskpilot/application/download_service.h>
#include <mutex>
#include <thread>

namespace maskpilot {
struct DownloadService::Impl {
    struct Job {
        std::shared_ptr<TransferControl> control = std::make_shared<TransferControl>();
        std::jthread thread;
    };
    std::unique_ptr<ITaskRepository> repository;
    TransportFactory factory;
    std::filesystem::path workDirectory;
    std::map<std::string, Task> tasks;
    std::map<std::string, std::unique_ptr<Job>> jobs;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> commands;
    std::vector<Task> published;
    std::string error;
    std::jthread coordinator;
    std::atomic_bool stopping{false};
    bool healthy = true;
    int concurrentTasks = 3, mediaSlots = 3;
    std::int64_t globalLimit = 0;

    void post(std::function<void()> command) {
        {
            std::lock_guard lock(mutex);
            commands.push_back(std::move(command));
        }
        cv.notify_one();
    }
    void save(Task& task) {
        task.updatedAt = unixTime();
        repository->save(task);
    }
    void clearOwnedParts(const Task& task) {
        const auto folder = workDirectory / task.id;
        std::error_code ignored;
        // Completed tasks usually had their cache reclaimed already. Avoid dozens
        // of redundant delete requests per record during bulk history removal.
        if (std::filesystem::exists(folder, ignored)) {
            for (int n = 0; n < 8; ++n)
                for (const auto& prefix : {"part-", "offset-"}) {
                    auto file = folder / (std::string(prefix) + std::to_string(n));
                    std::filesystem::remove(file, ignored);
                    file += L".new";
                    std::filesystem::remove(file, ignored);
                }
            std::filesystem::remove(folder / "identity", ignored);
            std::filesystem::remove(folder / "identity.new", ignored);
            std::filesystem::remove(folder, ignored);
            std::filesystem::remove_all(folder / "media", ignored);
            std::filesystem::remove(folder, ignored);
        }
        // A damaged persisted specification must never turn cleanup into a truncated
        // or unintended filesystem path. The task cache above is owned by validated ID.
        if (!validateSpec(task.spec).empty())
            return;
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
        if (event.outcome == TransferOutcome::Progress) {
            if (task.state != TaskState::Pausing && task.state != TaskState::Cancelling)
                task.state = event.phase;
            // Transfer metadata, not progress counters, determines resumable bytes after a crash.
            return;
        }
        jobs.erase(id); // The worker has enqueued its terminal event and no longer uses service state.
        task.bytesPerSecond = 0;
        task.error = std::move(event.error);
        switch (event.outcome) {
        case TransferOutcome::Complete:
            task.state = TaskState::Completed;
            break;
        case TransferOutcome::Paused:
            task.state = TaskState::Paused;
            break;
        case TransferOutcome::Cancelled:
            task.state = TaskState::Cancelled;
            break;
        case TransferOutcome::Attention:
            task.state = TaskState::NeedsAttention;
            break;
        case TransferOutcome::Failed:
            if (event.retryable && task.retries < task.spec.maxRetries && !stopping) {
                ++task.retries;
                task.retryAt = unixTime() + std::max(event.retryAfter, std::min(60, 1 << task.retries));
                task.state = TaskState::RetryWaiting;
            } else
                task.state = TaskState::Failed;
            break;
        default:
            break;
        }
        save(task);
        if (isTerminal(task.state))
            clearOwnedParts(task);
    }
    void schedule() {
        if (!healthy || stopping)
            return;
        if (jobs.empty())
            mediaSlots = concurrentTasks;
        const auto now = unixTime();
        for (auto& [id, task] : tasks) {
            if ((task.state == TaskState::Scheduled && now >= task.spec.scheduledAt) ||
                (task.state == TaskState::RetryWaiting && now >= task.retryAt)) {
                task.state = TaskState::Queued;
                save(task);
            }
            if (isActive(task.state) && task.spec.stopAt > 0 && now >= task.spec.stopAt &&
                jobs.contains(id)) {
                jobs[id]->control->pause = true;
                task.state = TaskState::Pausing;
            }
        }
        std::vector<Task*> ready;
        for (auto& [id, task] : tasks)
            if (task.state == TaskState::Queued)
                ready.push_back(&task);
        std::stable_sort(ready.begin(), ready.end(), [](auto a, auto b) {
            return a->spec.priority == b->spec.priority ? a->createdAt < b->createdAt
                                                        : a->spec.priority > b->spec.priority;
        });
        for (auto* task : ready) {
            if (const auto validationError = validateSpec(task->spec); !validationError.empty()) {
                task->state = TaskState::NeedsAttention;
                task->error = "Invalid saved task: " + validationError;
                save(*task);
                continue;
            }
            if (jobs.size() >= static_cast<size_t>(concurrentTasks))
                break;
            if (task->spec.stopAt > 0 && now >= task->spec.stopAt) {
                task->state = TaskState::Paused;
                save(*task);
                continue;
            }
            auto job = std::make_unique<Job>();
            auto control = job->control;
            const auto initialShare =
                globalLimit > 0 ? std::max<std::int64_t>(1, globalLimit / mediaSlots) : 0;
            control->speedLimit = initialShare > 0 && task->spec.speedLimit > 0
                                      ? std::min(initialShare, task->spec.speedLimit)
                                      : std::max(initialShare, task->spec.speedLimit);
            task->state = TaskState::Probing;
            task->error.clear();
            ++task->generation;
            save(*task);
            const auto copy = *task;
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
        const auto mediaShare = globalLimit > 0 ? std::max<std::int64_t>(1, globalLimit / mediaSlots) : 0;
        std::int64_t reserved = 0, nativeCount = 0;
        for (const auto& [id, job] : jobs) {
            (void)job;
            const auto& spec = tasks.at(id).spec;
            if (!spec.media) {
                ++nativeCount;
                continue;
            }
            auto rate = mediaShare > 0 && spec.speedLimit > 0 ? std::min(mediaShare, spec.speedLimit)
                                                              : std::max(mediaShare, spec.speedLimit);
            if (globalLimit > 0)
                reserved += rate;
        }
        const auto share =
            globalLimit > 0
                ? std::max<std::int64_t>(1, (globalLimit - reserved) / std::max<std::int64_t>(1, nativeCount))
                : 0;
        for (auto& [id, job] : jobs) {
            auto limit = tasks.at(id).spec.speedLimit;
            auto allocation = tasks.at(id).spec.media ? mediaShare : share;
            job->control->speedLimit =
                allocation > 0 && limit > 0 ? std::min(allocation, limit) : std::max(allocation, limit);
        }
    }
    void loop(std::promise<void> ready) {
        try {
            for (auto task : repository->load()) {
                if (isActive(task.state) || task.state == TaskState::Queued ||
                    task.state == TaskState::RetryWaiting) {
                    task.state = TaskState::Paused;
                    task.error = "Recovered after exit. Resume to validate local parts and remote identity.";
                    save(task);
                }
                tasks.emplace(task.id, std::move(task));
            }
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
            publish();
            if (shouldStop && jobs.empty())
                break;
        }
    }
};

DownloadService::DownloadService(std::unique_ptr<ITaskRepository> repository, TransportFactory factory,
                                 std::filesystem::path workDirectory)
    : impl_(std::make_unique<Impl>()) {
    impl_->repository = std::move(repository);
    impl_->factory = std::move(factory);
    impl_->workDirectory = std::move(workDirectory);
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
std::future<std::string> DownloadService::add(DownloadSpec spec, std::string requestId) {
    auto promise = std::make_shared<std::promise<std::string>>();
    auto result = promise->get_future();
    impl_->post([p = impl_.get(), spec = std::move(spec), requestId = std::move(requestId),
                 promise]() mutable {
        try {
            if (!p->healthy)
                throw std::runtime_error("Task storage unavailable.");
            if (auto message = validateSpec(spec); !message.empty())
                throw std::invalid_argument(message);
            if (!requestId.empty() && (requestId.size() != 32 ||
                                       !std::all_of(requestId.begin(), requestId.end(), [](unsigned char c) {
                                           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                                       })))
                throw std::invalid_argument("Invalid request ID.");
            if (!requestId.empty() && p->tasks.contains(requestId)) {
                if (p->tasks.at(requestId).spec.url != spec.url ||
                    p->tasks.at(requestId).spec.media != spec.media)
                    throw std::invalid_argument("Request ID belongs to another resource.");
                promise->set_value(requestId);
                return;
            }
            spec.destination = std::filesystem::absolute(spec.destination).lexically_normal();
            const auto original = spec.destination;
            auto reserved = [&](const auto& path) {
                return std::filesystem::exists(path) ||
                       std::any_of(p->tasks.begin(), p->tasks.end(), [&](const auto& pair) {
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
            Task task;
            task.id = requestId.empty() ? newTaskId() : requestId;
            task.spec = std::move(spec);
            task.createdAt = unixTime();
            task.updatedAt = task.createdAt;
            task.state = task.spec.startPaused ? TaskState::Paused
                                               : (task.spec.scheduledAt > unixTime() ? TaskState::Scheduled
                                                                                     : TaskState::Queued);
            p->save(task);
            const auto id = task.id;
            p->tasks.emplace(id, std::move(task));
            p->publish();
            promise->set_value(id);
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    return result;
}
void DownloadService::pause(const std::string& id) {
    impl_->post([p = impl_.get(), id] {
        auto it = p->tasks.find(id);
        if (it == p->tasks.end())
            return;
        auto& t = it->second;
        if (p->jobs.contains(id)) {
            p->jobs[id]->control->pause = true;
            t.state = TaskState::Pausing;
        } else if (!isTerminal(t.state)) {
            t.state = TaskState::Paused;
            p->save(t);
        }
    });
}
void DownloadService::resume(const std::string& id) {
    impl_->post([p = impl_.get(), id] {
        auto it = p->tasks.find(id);
        if (it == p->tasks.end() || p->jobs.contains(id) || isTerminal(it->second.state))
            return;
        auto& t = it->second;
        t.error.clear();
        t.retries = 0;
        if (t.spec.stopAt <= unixTime())
            t.spec.stopAt = 0;
        t.state = TaskState::Queued;
        p->save(t);
    });
}
void DownloadService::cancel(const std::string& id) {
    impl_->post([p = impl_.get(), id] {
        auto it = p->tasks.find(id);
        if (it == p->tasks.end() || isTerminal(it->second.state))
            return;
        auto& t = it->second;
        if (p->jobs.contains(id)) {
            p->jobs[id]->control->cancel = true;
            t.state = TaskState::Cancelling;
        } else {
            t.state = TaskState::Cancelled;
            p->save(t);
            p->clearOwnedParts(t);
        }
    });
}
void DownloadService::remove(const std::string& id) {
    impl_->post([p = impl_.get(), id] {
        auto it = p->tasks.find(id);
        if (it == p->tasks.end() || p->jobs.contains(id))
            return;
        p->repository->erase(id);
        p->clearOwnedParts(it->second);
        p->tasks.erase(it);
    });
}
void DownloadService::setLimits(int concurrentTasks, std::int64_t globalBytesPerSecond) {
    impl_->post([p = impl_.get(), concurrentTasks, globalBytesPerSecond] {
        p->concurrentTasks = std::clamp(concurrentTasks, 1, 8);
        // Retain stable reservations while a batch drains. Reducing configured concurrency
        // must not multiply the budget of existing media subprocesses.
        p->mediaSlots = std::max(p->concurrentTasks, static_cast<int>(p->jobs.size()));
        p->globalLimit = std::max<std::int64_t>(0, globalBytesPerSecond);
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
        it->second.spec = std::move(spec);
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
        const auto folder = p->workDirectory / id;
        for (int n = 0; n < 8; ++n) {
            std::error_code error;
            std::filesystem::remove(folder / ("part-" + std::to_string(n)), error);
            std::filesystem::remove(folder / ("offset-" + std::to_string(n)), error);
        }
        std::error_code error;
        std::filesystem::remove(folder / "identity", error);
        auto& task = it->second;
        task.downloaded = 0;
        task.total = -1;
        task.retries = 0;
        task.error.clear();
        if (task.spec.stopAt <= unixTime())
            task.spec.stopAt = 0;
        task.state = TaskState::Queued;
        p->save(task);
    });
}
void DownloadService::updateRequest(const std::string& id, DownloadSpec spec) {
    impl_->post([p = impl_.get(), id, spec = std::move(spec)]() mutable {
        auto it = p->tasks.find(id);
        if (it == p->tasks.end() || p->jobs.contains(id) || isTerminal(it->second.state) ||
            !validateSpec(spec).empty())
            return;
        spec.destination = it->second.spec.destination;
        if (it->second.spec.media != spec.media)
            p->clearOwnedParts(it->second);
        it->second.spec = std::move(spec);
        it->second.state = TaskState::Paused;
        it->second.error.clear();
        p->save(it->second);
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
        task.state = startAt > unixTime() ? TaskState::Scheduled : TaskState::Queued;
        p->save(task);
    });
}
std::string DownloadService::lastError() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->error;
}
} // namespace maskpilot
