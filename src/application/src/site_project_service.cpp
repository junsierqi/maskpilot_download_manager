#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <maskpilot/application/site_project_service.h>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>

namespace maskpilot {
namespace {
using Clock = std::chrono::steady_clock;
bool document(SiteContentKind kind) { return kind == SiteContentKind::Html || kind == SiteContentKind::Css; }
bool accepted(SiteProjectEntryState state) {
    return state == SiteProjectEntryState::Ready || state == SiteProjectEntryState::Processed ||
           state == SiteProjectEntryState::Skipped;
}
bool running(SiteProjectState state) {
    return state == SiteProjectState::Previewing || state == SiteProjectState::Running;
}
void disarmSynchronization(SiteProject& project) {
    project.syncIntervalMinutes = project.syncRemainingRuns = 0;
    project.syncNextAt = project.syncLastAt = 0;
    project.automaticRevision = false;
}
std::string urlPath(const std::string& url) {
    const auto authority = url.find("://");
    const auto begin = authority == std::string::npos ? 0 : url.find('/', authority + 3);
    return begin == std::string::npos ? "/" : url.substr(begin);
}
void normalizeFilters(SiteProjectOptions& options) {
    for (auto* prefixes : {&options.includePrefixes, &options.excludePrefixes})
        for (auto& prefix : *prefixes) {
            const auto canonical = canonicalSiteUrl(prefix, options.rootUrl);
            if (!sameSiteOrigin(options.rootUrl, canonical) || prefix.find('#') != std::string::npos)
                throw std::invalid_argument(
                    "Path filters must remain on the project origin without fragments.");
            prefix = urlPath(canonical);
        }
    if (const auto error = validateSiteProjectOptions(options); !error.empty())
        throw std::invalid_argument(error);
}
bool includes(const SiteProjectOptions& options, const std::string& url) {
    const auto path = urlPath(url);
    const auto matches = [&](const auto& prefix) { return path.starts_with(prefix); };
    return (options.includePrefixes.empty() ||
            std::any_of(options.includePrefixes.begin(), options.includePrefixes.end(), matches)) &&
           !std::any_of(options.excludePrefixes.begin(), options.excludePrefixes.end(), matches);
}
bool allows(const SiteProjectOptions& options, SiteContentKind kind) {
    return kind == SiteContentKind::Html || (kind == SiteContentKind::Css && options.styles) ||
           (kind == SiteContentKind::Image && options.images) ||
           (kind == SiteContentKind::Font && options.fonts) ||
           (kind == SiteContentKind::Media && options.media);
}
std::string extension(const SiteProjectEntry& item) {
    if (item.kind == SiteContentKind::Html)
        return "html";
    if (item.kind == SiteContentKind::Css)
        return "css";
    auto type = item.contentType.substr(0, item.contentType.find(';'));
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    while (!type.empty() && type.back() == ' ')
        type.pop_back();
    static const std::map<std::string, std::string> values = {{"image/png", "png"},
                                                              {"image/jpeg", "jpg"},
                                                              {"image/gif", "gif"},
                                                              {"image/webp", "webp"},
                                                              {"image/avif", "avif"},
                                                              {"image/bmp", "bmp"},
                                                              {"image/x-icon", "ico"},
                                                              {"image/vnd.microsoft.icon", "ico"},
                                                              {"font/woff", "woff"},
                                                              {"font/woff2", "woff2"},
                                                              {"application/font-woff", "woff"},
                                                              {"font/ttf", "ttf"},
                                                              {"font/otf", "otf"},
                                                              {"video/mp4", "mp4"},
                                                              {"audio/mp4", "m4a"},
                                                              {"video/webm", "webm"},
                                                              {"audio/webm", "webm"},
                                                              {"video/ogg", "ogg"},
                                                              {"audio/ogg", "ogg"},
                                                              {"audio/mpeg", "mp3"},
                                                              {"audio/wav", "wav"},
                                                              {"audio/flac", "flac"}};
    const auto found = values.find(type);
    if (found == values.end())
        throw std::runtime_error("Unsupported exported resource type.");
    return found->second;
}
std::string fileName(const SiteProject& project, const SiteProjectEntry& item) {
    if (item.id == project.entries.front().id)
        return "index.html";
    const char prefix = item.kind == SiteContentKind::Html    ? 'p'
                        : item.kind == SiteContentKind::Css   ? 'c'
                        : item.kind == SiteContentKind::Image ? 'i'
                        : item.kind == SiteContentKind::Font  ? 'f'
                                                              : 'm';
    return std::string(1, prefix) + '-' + item.id + '.' + extension(item);
}
} // namespace

struct SiteProjectService::Impl {
    DownloadService& downloads;
    std::unique_ptr<ISiteProjectRepository> repository;
    std::unique_ptr<ISiteContentProcessor> processor;
    std::unique_ptr<ISiteProjectPublisher> publisher;
    std::map<std::string, SiteProject> projects;
    std::map<std::string, ProxyCredentials> credentials;
    std::map<std::string, Clock::time_point> awaitingAck, awaitingResume;
    std::map<std::string, Clock::time_point> progressSaved;
    std::mutex commandMutex;
    std::condition_variable_any changed;
    std::deque<std::function<void()>> commands;
    mutable std::mutex viewMutex;
    std::vector<SiteProjectSummary> summaries;
    std::map<std::string, std::shared_ptr<const SiteProject>> details;
    std::set<std::string> dirtyDetails;
    std::string storageError;
    std::atomic_int pending = 0;
    std::atomic_bool unfinished = false;
    std::function<std::int64_t()> utcNow;
    std::jthread thread;

    Impl(DownloadService& service, std::unique_ptr<ISiteProjectRepository> store,
         std::unique_ptr<ISiteContentProcessor> content, std::unique_ptr<ISiteProjectPublisher> output,
         std::function<std::int64_t()> now)
        : downloads(service), repository(std::move(store)), processor(std::move(content)),
          publisher(std::move(output)), utcNow(std::move(now)) {
        if (!utcNow)
            utcNow = [] {
                return std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                    .count();
            };
        if (!repository || !processor || !publisher)
            throw std::invalid_argument("Site project dependencies are required.");
        std::promise<void> ready;
        auto initialized = ready.get_future();
        thread = std::jthread(
            [this, ready = std::move(ready)](std::stop_token stop) mutable { loop(stop, std::move(ready)); });
        initialized.get();
    }
    ~Impl() {
        thread.request_stop();
        changed.notify_all();
        if (thread.joinable())
            thread.join();
    }
    void publishView() {
        decltype(details) nextDetails;
        {
            std::lock_guard lock(viewMutex);
            nextDetails = details;
        }
        // save() may persist a replacement before it is installed in projects.
        // Readers see only committed coordinator state, in one detail/summary frame.
        for (const auto& id : dirtyDetails) {
            const auto found = projects.find(id);
            if (found == projects.end())
                nextDetails.erase(id);
            else
                nextDetails[id] = std::make_shared<const SiteProject>(found->second);
        }
        std::vector<SiteProjectSummary> output;
        bool any = false;
        for (const auto& [id, project] : projects) {
            SiteProjectSummary value;
            value.id = id;
            value.name = project.options.name;
            value.rootUrl = project.options.rootUrl;
            value.queueId = project.options.queueId;
            value.state = project.state;
            value.revision = project.revision;
            value.error = project.error;
            value.publishedRevision = project.published ? project.published->revision : 0;
            value.needsProxyCredentials = project.options.proxyAuthenticationRequired &&
                                          (!credentials.contains(id) || credentials.at(id).username.empty());
            for (const auto& item : project.entries) {
                if (document(item.kind))
                    ++value.documents;
                else
                    ++value.resources;
                if (accepted(item.state)) {
                    value.storedBytes += item.bytes;
                    ++value.ready;
                } else
                    value.reservedBytes += item.maxStoredBytes;
                if (item.state == SiteProjectEntryState::Skipped)
                    ++value.skipped;
            }
            any = any || !siteProjectTerminal(project.state) || project.syncIntervalMinutes != 0;
            output.push_back(std::move(value));
        }
        {
            std::lock_guard lock(viewMutex);
            details.swap(nextDetails);
            summaries = std::move(output);
        }
        dirtyDetails.clear();
        unfinished = any || !storageError.empty() || pending.load() != 0;
    }
    void save(SiteProject& project) {
        if (!storageError.empty())
            throw std::runtime_error(storageError);
        try {
            repository->save(project);
        } catch (...) {
            {
                std::lock_guard lock(viewMutex);
                storageError = "Cannot save project intent. Project resources have been paused.";
            }
            for (const auto& [id, existing] : projects)
                stopChildren(existing, false);
            stopChildren(project, false);
            publishView();
            throw;
        }
        dirtyDetails.insert(project.id);
    }
    SiteProject& find(const std::string& id, std::int64_t revision) {
        if (!storageError.empty())
            throw std::runtime_error(storageError);
        const auto found = projects.find(id);
        if (found == projects.end() || found->second.revision != revision)
            throw std::invalid_argument("Project changed or is unavailable. Refresh its current revision.");
        return found->second;
    }
    template <class Function> auto request(Function function) {
        using Value = std::invoke_result_t<Function>;
        auto promise = std::make_shared<std::promise<Value>>();
        auto future = promise->get_future();
        {
            std::lock_guard lock(commandMutex);
            if (commands.size() >= 128) {
                promise->set_exception(
                    std::make_exception_ptr(std::runtime_error("Too many pending project commands.")));
                return future;
            }
            ++pending;
            unfinished = true;
            commands.push_back([this, promise, function = std::move(function)]() mutable {
                try {
                    if constexpr (std::is_void_v<Value>) {
                        function();
                        publishView();
                        promise->set_value();
                    } else {
                        auto result = function();
                        publishView();
                        promise->set_value(std::move(result));
                    }
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
                --pending;
                publishView();
            });
        }
        changed.notify_all();
        return future;
    }
    bool belongs(const Task& task, const SiteProject& project) const {
        return task.ownerId == "standalone" && task.projectId == project.id &&
               task.projectRevision == project.revision;
    }
    bool matches(const Task& task, const SiteProject& project, const SiteProjectEntry& item) const {
        return belongs(task, project) && task.requestId == item.requestId && task.spec.url == item.url &&
               task.spec.kind == TransferKind::Http && task.queueId == project.options.queueId &&
               task.spec.destination.lexically_normal() == item.destination.lexically_normal() &&
               task.spec.maxStoredBytes == item.maxStoredBytes && task.spec.sameOriginRedirectOnly &&
               task.spec.proxy == project.options.proxy &&
               task.spec.proxyAuthenticationRequired == project.options.proxyAuthenticationRequired &&
               !requiresOriginAuthentication(task.spec) && !task.originAuthenticationRequired;
    }
    void stopChildren(const SiteProject& project, bool cancel) {
        for (const auto& task : downloads.snapshot())
            if (belongs(task, project) && !isTerminal(task.state)) {
                if (cancel)
                    downloads.cancel(task.id);
                else if (isActive(task.state) || task.runIntent == RunIntent::Run)
                    downloads.pause(task.id);
            }
    }
    void forgetTimers(const SiteProject& project) {
        for (const auto& item : project.entries) {
            awaitingAck.erase(item.id);
            awaitingResume.erase(item.taskId);
        }
    }
    void fail(SiteProject& project, const std::string& message) {
        disarmSynchronization(project);
        if (running(project.state))
            project.resumePhase = project.state;
        project.state = SiteProjectState::NeedsAttention;
        project.error = message.substr(0, 4096);
        stopChildren(project, false);
        save(project);
    }
    void route(SiteProject& project, ProxyCredentials value) {
        if (!value.username.empty() || !value.password.empty()) {
            if (!project.options.proxyAuthenticationRequired)
                throw std::invalid_argument("Enable proxy authentication in the project options.");
            DownloadSpec spec;
            spec.url = project.options.rootUrl;
            spec.destination = project.options.outputDirectory / "route.bin";
            spec.proxy = project.options.proxy;
            spec.proxyAuthenticationRequired = true;
            spec.proxyUsername = value.username;
            spec.proxyPassword = value.password;
            if (const auto error = validateSpec(spec); !error.empty())
                throw std::invalid_argument(error);
            credentials[project.id] = std::move(value);
        }
        if (project.options.proxyAuthenticationRequired &&
            (!credentials.contains(project.id) || credentials.at(project.id).username.empty()))
            throw std::invalid_argument(
                "Reenter this project's required proxy credentials before downloading.");
    }
    void checkQueue(const SiteProject& project) {
        const auto queues = downloads.queues().get();
        if (std::none_of(queues.begin(), queues.end(),
                         [&](const auto& queue) { return queue.id == project.options.queueId; }))
            throw std::invalid_argument("The selected project queue is unavailable.");
    }
    void root(SiteProject& project) {
        SiteProjectEntry item;
        item.id = newTaskId();
        item.requestId = newTaskId();
        item.url = project.options.rootUrl;
        item.destination = repository->prepareRevision(project.id, project.revision) / (item.id + ".body");
        project.entries.push_back(std::move(item));
    }
    std::int64_t nextSynchronization(int minutes) const {
        const auto now = utcNow();
        if (now < 0 || now > 253402300799LL - minutes * 60LL)
            throw std::runtime_error("The synchronization clock is outside the supported range.");
        return now + minutes * 60LL;
    }
    void newRevision(SiteProject& project, SiteProject replacement) {
        if (project.revision >= 1000000)
            throw std::runtime_error("Project revision limit reached; create a new project.");
        ++replacement.revision;
        replacement.entries.clear();
        replacement.pendingPublication.reset();
        replacement.excludedCount = 0;
        replacement.error.clear();
        replacement.state = replacement.resumePhase = SiteProjectState::Previewing;
        checkQueue(replacement);
        route(replacement, {});
        root(replacement);
        save(replacement); // Durable new intent and allowance consumption precede all child dispatch.
        forgetTimers(project);
        project = std::move(replacement);
    }
    void synchronize(SiteProject& project) {
        if (project.state != SiteProjectState::Completed || project.syncIntervalMinutes == 0 ||
            project.syncRemainingRuns == 0 || project.syncNextAt == 0)
            return;
        const auto now = utcNow();
        if (now < project.syncLastAt) {
            auto deferred = project;
            deferred.syncNextAt = nextSynchronization(project.syncIntervalMinutes);
            deferred.syncLastAt = now;
            save(deferred);
            project = std::move(deferred);
        } else if (now >= project.syncNextAt) {
            auto replacement = project;
            --replacement.syncRemainingRuns;
            replacement.syncNextAt = 0;
            replacement.syncLastAt = now;
            replacement.automaticRevision = true;
            newRevision(project, std::move(replacement));
        }
    }
    void discover(SiteProject& project, const std::vector<SiteContentReference>& references, int depth) {
        std::set<std::string> known;
        int pages = 0;
        for (const auto& item : project.entries) {
            known.insert(item.url);
            if (!item.responseUrl.empty())
                known.insert(item.responseUrl);
            if (item.kind == SiteContentKind::Html)
                ++pages;
        }
        const auto directory = repository->prepareRevision(project.id, project.revision);
        auto ordered = references;
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            return left.kind != SiteContentKind::Html && right.kind == SiteContentKind::Html;
        });
        for (const auto& reference : ordered) {
            const auto url = canonicalSiteUrl(reference.url);
            const auto level = depth + (reference.kind == SiteContentKind::Html ? 1 : 0);
            if (url.empty())
                continue;
            if (known.contains(url)) {
                if (reference.kind != SiteContentKind::Html && allows(project.options, reference.kind))
                    for (auto& existing : project.entries)
                        if (existing.url == url && existing.id != project.entries.front().id &&
                            existing.kind == SiteContentKind::Html &&
                            existing.state == SiteProjectEntryState::Planned) {
                            existing.kind = reference.kind;
                            existing.depth = std::min(existing.depth, level);
                            --pages;
                            break;
                        }
                continue;
            }
            if (!sameSiteOrigin(project.options.rootUrl, url) || !includes(project.options, url) ||
                !allows(project.options, reference.kind) || level > project.options.maxDepth ||
                (reference.kind == SiteContentKind::Html && pages >= project.options.maxPages) ||
                project.entries.size() >= static_cast<std::size_t>(project.options.maxResources)) {
                known.insert(url);
                ++project.excludedCount;
                continue;
            }
            SiteProjectEntry item;
            item.id = newTaskId();
            item.requestId = newTaskId();
            item.url = url;
            item.kind = reference.kind;
            item.depth = level;
            item.destination = directory / (item.id + ".body");
            if (item.kind == SiteContentKind::Html)
                ++pages;
            known.insert(url);
            project.entries.push_back(std::move(item));
        }
    }
    void consume(SiteProject& project, std::size_t index, const Task& task) {
        auto& item = project.entries[index];
        if (task.responseUrl.empty() || !sameSiteOrigin(project.options.rootUrl, task.responseUrl))
            throw std::runtime_error("Project resource has no valid same-origin final response.");
        auto body = repository->readBody(project, item);
        if (body.body.size() > static_cast<std::size_t>(item.maxStoredBytes) ||
            task.downloaded != static_cast<std::int64_t>(body.body.size()))
            throw std::runtime_error("Project resource length differs from its completed native task.");
        item.bytes = static_cast<std::int64_t>(body.body.size());
        item.sha256 = body.sha256;
        item.responseUrl = canonicalSiteUrl(task.responseUrl);
        item.contentType = task.responseContentType;
        item.error.clear();
        item.state = SiteProjectEntryState::Ready;
        const auto observedKind = classifySiteContent(item.contentType);
        if (index != 0 && item.kind == SiteContentKind::Html &&
            observedKind != SiteContentKind::Unsupported && allows(project.options, observedKind))
            item.kind = observedKind;
        const auto kind = item.kind;
        const auto depth = item.depth;
        std::vector<SiteContentReference> references;
        try {
            if (classifySiteContent(item.contentType) != kind)
                throw std::invalid_argument("Observed content type does not match the planned resource.");
            SiteContentLimits limits;
            limits.maxReferences = 10000;
            limits.maxOutputBytes = static_cast<std::size_t>(
                std::min<std::int64_t>(project.options.maxOutputBytes, 16 * 1024 * 1024));
            auto result =
                processor->process({item.responseUrl, item.contentType, std::move(body.body)}, limits);
            if (document(kind)) {
                references = std::move(result.references);
                item.state = SiteProjectEntryState::Processed;
            }
        } catch (const std::invalid_argument& error) {
            if (index == 0)
                throw;
            item.state = SiteProjectEntryState::Skipped;
            item.error = error.what();
        }
        // discover may reallocate entries; do not retain the item reference across it.
        if (!references.empty())
            discover(project, references, depth);
        save(project);
    }
    void submit(SiteProject& project, SiteProjectEntry& item) {
        std::int64_t charged = 0;
        for (const auto& existing : project.entries)
            charged += accepted(existing.state) ? existing.bytes : existing.maxStoredBytes;
        const auto remaining = project.options.maxStoredBytes - charged;
        if (remaining <= 0)
            throw std::runtime_error("This revision's stored body budget is exhausted.");
        item.maxStoredBytes = std::min(remaining, document(item.kind) ? project.options.maxPageBytes
                                                                      : project.options.maxAssetBytes);
        item.state = SiteProjectEntryState::Dispatching;
        save(project); // Stable request and full reservation precede task creation.
        DownloadSpec spec;
        spec.url = item.url;
        spec.destination = item.destination;
        spec.startPaused = true;
        spec.maxStoredBytes = item.maxStoredBytes;
        spec.sameOriginRedirectOnly = true;
        spec.proxy = project.options.proxy;
        spec.proxyAuthenticationRequired = project.options.proxyAuthenticationRequired;
        spec.category = "Site project: " + project.options.name;
        if (credentials.contains(project.id)) {
            spec.proxyUsername = credentials.at(project.id).username;
            spec.proxyPassword = credentials.at(project.id).password;
        }
        item.taskId = downloads
                          .addProjectChild(std::move(spec), project.id, project.revision, item.requestId,
                                           project.options.queueId)
                          .get();
        awaitingAck[item.id] = Clock::now() + std::chrono::seconds(5);
        save(project); // ACK is durable, but publication of the native snapshot can follow it.
    }
    void exportRevision(SiteProject& project) {
        std::map<std::string, std::pair<std::string, SiteContentKind>> names;
        for (const auto& item : project.entries)
            if (item.state != SiteProjectEntryState::Skipped) {
                if (!accepted(item.state))
                    throw std::runtime_error("Project resources are still incomplete.");
                const auto name = fileName(project, item);
                names.try_emplace(item.url, name, item.kind);
                names.try_emplace(item.responseUrl, name, item.kind);
            }
        std::vector<SiteProjectArtifact> artifacts;
        std::int64_t total = 0;
        const auto tasks = downloads.snapshot();
        for (const auto& item : project.entries) {
            const auto found = std::find_if(tasks.begin(), tasks.end(),
                                            [&](const auto& task) { return task.id == item.taskId; });
            if (found == tasks.end() || !matches(*found, project, item) ||
                found->state != TaskState::Completed)
                throw std::runtime_error("A project child was removed or changed before publication.");
            if (item.state == SiteProjectEntryState::Skipped)
                continue;
            const auto body = repository->readBody(project, item);
            SiteContentLimits limits;
            limits.maxReferences = 10000;
            limits.maxOutputBytes = static_cast<std::size_t>(std::max<std::int64_t>(
                1, std::min<std::int64_t>(project.options.maxOutputBytes - total, 16 * 1024 * 1024)));
            const auto result =
                processor->process({item.responseUrl, item.contentType, body.body}, limits,
                                   [&](const SiteContentReference& reference) -> std::optional<std::string> {
                                       const auto name = names.find(reference.url);
                                       if (name == names.end() || name->second.second != reference.kind)
                                           return std::nullopt;
                                       return name->second.first;
                                   });
            if (result.body.size() > static_cast<std::size_t>(project.options.maxOutputBytes - total))
                throw std::runtime_error("Exported content exceeds this revision's output limit.");
            total += static_cast<std::int64_t>(result.body.size());
            artifacts.push_back({fileName(project, item), result.body});
        }
        project.pendingPublication = publisher->plan(project, artifacts);
        project.state = SiteProjectState::Publishing;
        save(project);
        publisher->commit(project, *project.pendingPublication, artifacts);
        auto completed = project;
        completed.published = completed.pendingPublication;
        completed.pendingPublication.reset();
        completed.state = SiteProjectState::Completed;
        completed.error.clear();
        completed.automaticRevision = false;
        if (completed.syncIntervalMinutes != 0 && completed.syncRemainingRuns > 0) {
            completed.syncNextAt = nextSynchronization(completed.syncIntervalMinutes);
            completed.syncLastAt = utcNow();
        } else
            disarmSynchronization(completed);
        save(completed);
        forgetTimers(project);
        project = std::move(completed);
    }
    using TaskMap = std::unordered_map<std::string, const Task*>;
    void step(SiteProject& project, const TaskMap& tasks) {
        if (project.state == SiteProjectState::Pausing || project.state == SiteProjectState::Cancelling) {
            const bool cancel = project.state == SiteProjectState::Cancelling;
            stopChildren(project, cancel);
            bool waiting = false;
            for (const auto& [id, task] : tasks)
                if (belongs(*task, project))
                    waiting =
                        waiting || (!isTerminal(task->state) &&
                                    (isActive(task->state) || task->runIntent == RunIntent::Run || cancel));
            if (!waiting) {
                if (cancel && project.pendingPublication) {
                    publisher->discardStage(project, *project.pendingPublication);
                    project.pendingPublication.reset();
                }
                project.state = cancel ? SiteProjectState::Cancelled : SiteProjectState::Paused;
                if (cancel)
                    forgetTimers(project);
                save(project);
            }
            return;
        }
        if (!running(project.state))
            return;
        std::size_t active = 0;
        for (std::size_t index = 0; index < project.entries.size(); ++index) {
            auto& item = project.entries[index];
            if (item.state != SiteProjectEntryState::Fetching &&
                item.state != SiteProjectEntryState::Dispatching)
                continue;
            ++active;
            const auto found = tasks.find(item.taskId);
            if (found == tasks.end()) {
                if (awaitingAck.contains(item.id) && Clock::now() < awaitingAck.at(item.id))
                    continue;
                throw std::runtime_error(
                    "A project child is missing. Cancel this revision before a fresh update.");
            }
            const auto& task = *found->second;
            if (!matches(task, project, item))
                throw std::runtime_error("A project child no longer matches its immutable plan.");
            awaitingAck.erase(item.id);
            if (task.state != TaskState::Paused)
                awaitingResume.erase(task.id);
            if (task.state == TaskState::Cancelled)
                throw std::runtime_error("A project child was explicitly cancelled.");
            if (task.state == TaskState::Failed || task.state == TaskState::NeedsAttention)
                throw std::runtime_error("Resource needs attention: " + task.error);
            if (task.state == TaskState::Completed) {
                consume(project, index, task);
                return;
            }
            if (item.state == SiteProjectEntryState::Dispatching) {
                item.state = SiteProjectEntryState::Fetching;
                save(project);
                if (task.state == TaskState::Paused) {
                    downloads.resume(task.id);
                    awaitingResume[task.id] = Clock::now() + std::chrono::seconds(5);
                }
            } else if (task.state == TaskState::Paused) {
                if (awaitingResume.contains(task.id) && Clock::now() < awaitingResume.at(task.id))
                    continue;
                project.resumePhase = project.state;
                project.state = SiteProjectState::Pausing;
                disarmSynchronization(project);
                save(project);
                return;
            } else
                awaitingResume.erase(task.id);
            if (task.downloaded < 0 || task.downloaded > item.maxStoredBytes)
                throw std::runtime_error("Native resource progress exceeds its stored body bound.");
            item.bytes = task.downloaded;
            item.error =
                task.state == TaskState::PolicyWaiting ? "Waiting for queue policy or daily budget." : "";
        }
        for (auto& item : project.entries)
            if (active < 4 && item.state == SiteProjectEntryState::Planned &&
                (project.state == SiteProjectState::Running || document(item.kind))) {
                submit(project, item);
                ++active;
            }
        const bool done = std::all_of(project.entries.begin(), project.entries.end(), [&](const auto& item) {
            return accepted(item.state) ||
                   (project.state == SiteProjectState::Previewing && !document(item.kind) &&
                    item.state == SiteProjectEntryState::Planned);
        });
        if (done) {
            if (project.state == SiteProjectState::Previewing) {
                project.state =
                    project.automaticRevision ? SiteProjectState::Running : SiteProjectState::PreviewReady;
                project.resumePhase =
                    project.state == SiteProjectState::Running ? project.state : SiteProjectState::Previewing;
                project.error.clear();
                save(project);
            } else
                exportRevision(project);
        } else if (Clock::now() - progressSaved[project.id] > std::chrono::seconds(1)) {
            progressSaved[project.id] = Clock::now();
            save(project);
        }
    }
    void reconcile(SiteProject& project) {
        const auto tasks = downloads.snapshot();
        for (auto& item : project.entries) {
            if (item.state == SiteProjectEntryState::Planned)
                continue;
            const auto task = std::find_if(tasks.begin(), tasks.end(), [&](const auto& value) {
                return belongs(value, project) && value.requestId == item.requestId;
            });
            if (task == tasks.end() || (!item.taskId.empty() && item.taskId != task->id) ||
                !matches(*task, project, item) || task->state == TaskState::Cancelled)
                throw std::runtime_error("Saved project child is missing, cancelled or changed; cancel this "
                                         "revision before updating.");
            item.taskId = task->id;
            if (accepted(item.state))
                repository->readBody(project, item);
        }
    }
    void loop(std::stop_token stop, std::promise<void> ready) {
        try {
            for (auto project : repository->load()) {
                if (project.state == SiteProjectState::Publishing && project.pendingPublication) {
                    try {
                        if (publisher->resolve(project, *project.pendingPublication)) {
                            project.published = project.pendingPublication;
                            project.pendingPublication.reset();
                            project.state = SiteProjectState::Completed;
                            project.error.clear();
                        } else {
                            project.state = SiteProjectState::NeedsAttention;
                            project.error =
                                "Publication interrupted. Cancel this revision before a fresh update.";
                        }
                    } catch (const std::exception&) {
                        project.state = SiteProjectState::NeedsAttention;
                        project.error = "Interrupted publication files changed. The prior verified revision "
                                        "remains available.";
                    }
                } else if (running(project.state) || project.state == SiteProjectState::Pausing) {
                    if (running(project.state))
                        project.resumePhase = project.state;
                    project.state = SiteProjectState::Paused;
                    project.error =
                        "Recovered after exit. Explicitly resume to verify saved project children.";
                }
                if (!siteProjectTerminal(project.state) && project.state != SiteProjectState::Cancelling) {
                    try {
                        reconcile(project);
                    } catch (const std::exception& error) {
                        project.state = SiteProjectState::NeedsAttention;
                        project.error = error.what();
                    }
                }
                if (project.state == SiteProjectState::NeedsAttention)
                    disarmSynchronization(project);
                if (project.state == SiteProjectState::Completed && project.automaticRevision) {
                    project.automaticRevision = false;
                    if (project.syncIntervalMinutes != 0 && project.syncRemainingRuns > 0) {
                        project.syncNextAt = nextSynchronization(project.syncIntervalMinutes);
                        project.syncLastAt = utcNow();
                    } else
                        disarmSynchronization(project);
                }
                projects.emplace(project.id, std::move(project));
            }
            for (auto& [id, project] : projects)
                save(project);
        } catch (const std::exception&) {
            std::lock_guard lock(viewMutex);
            storageError = "Project storage is unavailable. Existing project resources remain paused.";
        }
        publishView();
        ready.set_value();
        while (!stop.stop_requested()) {
            std::deque<std::function<void()>> work;
            {
                std::lock_guard lock(commandMutex);
                work.swap(commands);
            }
            for (auto& command : work)
                command();
            if (storageError.empty()) {
                const auto snapshot = downloads.snapshot();
                TaskMap tasks;
                for (const auto& task : snapshot)
                    tasks.emplace(task.id, &task);
                for (auto& [id, project] : projects) {
                    if (!storageError.empty())
                        break;
                    try {
                        synchronize(project);
                        step(project, tasks);
                    } catch (const std::exception& error) {
                        try {
                            fail(project, error.what());
                        } catch (...) {
                            stopChildren(project, false);
                            std::lock_guard lock(viewMutex);
                            storageError = "Cannot save project progress. Resources have been paused.";
                        }
                    }
                }
            }
            publishView();
            std::unique_lock lock(commandMutex);
            changed.wait_for(lock, stop, std::chrono::milliseconds(100), [&] { return !commands.empty(); });
        }
        for (auto& [id, project] : projects) {
            stopChildren(project, false);
            if (running(project.state) || project.state == SiteProjectState::Pausing)
                try {
                    if (running(project.state))
                        project.resumePhase = project.state;
                    project.state = SiteProjectState::Paused;
                    save(project);
                } catch (...) {
                }
        }
    }
};

SiteProjectService::SiteProjectService(DownloadService& service,
                                       std::unique_ptr<ISiteProjectRepository> repository,
                                       std::unique_ptr<ISiteContentProcessor> content,
                                       std::unique_ptr<ISiteProjectPublisher> output,
                                       std::function<std::int64_t()> utcNow)
    : impl_(std::make_unique<Impl>(service, std::move(repository), std::move(content), std::move(output),
                                   std::move(utcNow))) {}
SiteProjectService::~SiteProjectService() = default;
std::future<std::string> SiteProjectService::create(SiteProjectOptions options,
                                                    ProxyCredentials credentials) {
    return impl_->request(
        [p = impl_.get(), options = std::move(options), credentials = std::move(credentials)]() mutable {
            if (!p->storageError.empty())
                throw std::runtime_error(p->storageError);
            if (p->projects.size() >= 32)
                throw std::invalid_argument("At most 32 project histories can be retained.");
            options.rootUrl = canonicalSiteUrl(options.rootUrl);
            options.outputDirectory = options.outputDirectory.lexically_normal();
            if (const auto error = validateSiteProjectOptions(options); !error.empty())
                throw std::invalid_argument(error);
            normalizeFilters(options);
            if (!includes(options, options.rootUrl))
                throw std::invalid_argument("The root URL is excluded by the path filters.");
            SiteProject project;
            project.id = newTaskId();
            project.options = std::move(options);
            const auto id = project.id;
            try {
                p->checkQueue(project);
                p->route(project, std::move(credentials));
                std::filesystem::create_directories(project.options.outputDirectory);
                project.options.outputDirectory = std::filesystem::canonical(project.options.outputDirectory);
                p->root(project);
                p->save(project);
            } catch (...) {
                p->credentials.erase(id);
                throw;
            }
            p->projects.emplace(id, std::move(project));
            p->publishView();
            return id;
        });
}
std::future<void> SiteProjectService::preview(std::string id, std::int64_t revision,
                                              ProxyCredentials credentials) {
    return impl_->request(
        [p = impl_.get(), id = std::move(id), revision, credentials = std::move(credentials)]() mutable {
            auto& project = p->find(id, revision);
            if (project.state != SiteProjectState::Draft)
                throw std::invalid_argument("Preview requires a draft project.");
            p->route(project, std::move(credentials));
            p->checkQueue(project);
            project.state = project.resumePhase = SiteProjectState::Previewing;
            project.error.clear();
            p->save(project);
        });
}
std::future<void> SiteProjectService::start(std::string id, std::int64_t revision,
                                            ProxyCredentials credentials) {
    return impl_->request(
        [p = impl_.get(), id = std::move(id), revision, credentials = std::move(credentials)]() mutable {
            auto& project = p->find(id, revision);
            if (project.state != SiteProjectState::PreviewReady)
                throw std::invalid_argument("Inspect a completed preview before starting assets.");
            p->route(project, std::move(credentials));
            p->checkQueue(project);
            p->reconcile(project);
            project.state = project.resumePhase = SiteProjectState::Running;
            project.error.clear();
            p->save(project);
        });
}
std::future<void> SiteProjectService::pause(std::string id, std::int64_t revision) {
    return impl_->request([p = impl_.get(), id = std::move(id), revision] {
        auto& project = p->find(id, revision);
        if (project.state == SiteProjectState::Completed && project.syncIntervalMinutes != 0) {
            auto replacement = project;
            disarmSynchronization(replacement);
            p->save(replacement);
            project = std::move(replacement);
            return;
        }
        if (!running(project.state))
            throw std::invalid_argument("Only a running preview or export can be paused.");
        project.resumePhase = project.state;
        project.state = SiteProjectState::Pausing;
        disarmSynchronization(project);
        p->save(project);
        p->stopChildren(project, false);
    });
}
std::future<void> SiteProjectService::resume(std::string id, std::int64_t revision,
                                             ProxyCredentials credentials) {
    return impl_->request(
        [p = impl_.get(), id = std::move(id), revision, credentials = std::move(credentials)]() mutable {
            auto& project = p->find(id, revision);
            if (project.state != SiteProjectState::Paused)
                throw std::invalid_argument(
                    "Resume requires a paused revision. Cancel an invalid revision before updating.");
            p->route(project, std::move(credentials));
            p->checkQueue(project);
            p->reconcile(project);
            const auto tasks = p->downloads.snapshot();
            project.state = project.resumePhase;
            project.error.clear();
            p->save(project);
            try {
                for (const auto& task : tasks)
                    if (p->belongs(task, project) && !isTerminal(task.state)) {
                        auto spec = task.spec;
                        if (p->credentials.contains(id)) {
                            spec.proxyUsername = p->credentials.at(id).username;
                            spec.proxyPassword = p->credentials.at(id).password;
                        }
                        p->downloads.updateRequestWithCredentials(task.id, std::move(spec), false).get();
                        p->downloads.resume(task.id);
                        p->awaitingResume[task.id] = Clock::now() + std::chrono::seconds(5);
                    }
            } catch (...) {
                p->fail(project, "Cannot restore the required route for a saved project resource.");
                throw;
            }
        });
}
std::future<void> SiteProjectService::cancel(std::string id, std::int64_t revision) {
    return impl_->request([p = impl_.get(), id = std::move(id), revision] {
        auto& project = p->find(id, revision);
        if (siteProjectTerminal(project.state))
            throw std::invalid_argument("This revision is already finished.");
        project.state = SiteProjectState::Cancelling;
        disarmSynchronization(project);
        p->save(project);
        p->stopChildren(project, true);
    });
}
std::future<void> SiteProjectService::update(std::string id, std::int64_t revision,
                                             SiteProjectOptions options, ProxyCredentials credentials) {
    return impl_->request([p = impl_.get(), id = std::move(id), revision, options = std::move(options),
                           credentials = std::move(credentials)]() mutable {
        auto& project = p->find(id, revision);
        if (!siteProjectTerminal(project.state) || project.revision >= 1000000)
            throw std::invalid_argument("Finish or cancel the current revision before updating.");
        options.rootUrl = canonicalSiteUrl(options.rootUrl);
        options.outputDirectory = std::filesystem::canonical(options.outputDirectory);
        if (options.rootUrl != project.options.rootUrl ||
            options.outputDirectory != project.options.outputDirectory)
            throw std::invalid_argument("A project's root URL and output folder cannot change.");
        if (const auto error = validateSiteProjectOptions(options); !error.empty())
            throw std::invalid_argument(error);
        normalizeFilters(options);
        if (!includes(options, options.rootUrl))
            throw std::invalid_argument("The root URL is excluded by the path filters.");
        auto replacement = project;
        replacement.options = std::move(options);
        disarmSynchronization(replacement);
        if (replacement.options.proxy != project.options.proxy ||
            replacement.options.proxyAuthenticationRequired != project.options.proxyAuthenticationRequired)
            p->credentials.erase(id);
        p->route(replacement, std::move(credentials));
        p->newRevision(project, std::move(replacement));
    });
}
std::future<void> SiteProjectService::removeHistory(std::string id, std::int64_t revision) {
    return impl_->request([p = impl_.get(), id = std::move(id), revision] {
        auto& project = p->find(id, revision);
        if (!siteProjectTerminal(project.state))
            throw std::invalid_argument("Finish or cancel the revision before removing its history.");
        p->repository->erase(id);
        p->forgetTimers(project);
        p->progressSaved.erase(id);
        p->projects.erase(id);
        p->credentials.erase(id);
        p->dirtyDetails.insert(id);
    });
}
std::future<std::filesystem::path> SiteProjectService::openPath(std::string id) {
    return impl_->request([p = impl_.get(), id = std::move(id)] {
        if (!p->projects.contains(id) || !p->projects.at(id).published)
            throw std::invalid_argument("This project has no published revision.");
        const auto& project = p->projects.at(id);
        auto path = p->publisher->resolve(project, *project.published);
        if (!path)
            throw std::runtime_error("Published files are missing.");
        return *path;
    });
}
std::future<void> SiteProjectService::setSynchronization(std::string id, std::int64_t revision,
                                                         int intervalMinutes, int runs,
                                                         ProxyCredentials credentials) {
    return impl_->request([p = impl_.get(), id = std::move(id), revision, intervalMinutes, runs,
                           credentials = std::move(credentials)]() mutable {
        auto& project = p->find(id, revision);
        auto replacement = project;
        if (intervalMinutes == 0) {
            // Disarming recurrence does not stop already accepted work or revoke its asset consent.
            const bool automatic = replacement.automaticRevision;
            disarmSynchronization(replacement);
            replacement.automaticRevision = automatic;
        } else {
            if (project.state != SiteProjectState::Completed || intervalMinutes < 5 ||
                intervalMinutes > 10080 || runs < 1 || runs > 100)
                throw std::invalid_argument(
                    "Enable synchronization after a completed export: 5..10080 minutes, 1..100 runs.");
            p->route(replacement, std::move(credentials));
            p->checkQueue(replacement);
            replacement.syncIntervalMinutes = intervalMinutes;
            replacement.syncRemainingRuns = runs;
            replacement.syncNextAt = p->nextSynchronization(intervalMinutes);
            replacement.syncLastAt = p->utcNow();
            replacement.automaticRevision = false;
        }
        p->save(replacement);
        project = std::move(replacement);
    });
}
std::future<void> SiteProjectService::pauseAll(bool disarm) {
    return impl_->request([p = impl_.get(), disarm] {
        for (auto& [id, project] : p->projects) {
            const bool hadSchedule = project.syncIntervalMinutes != 0 || project.automaticRevision;
            if (disarm)
                disarmSynchronization(project);
            if (running(project.state)) {
                project.resumePhase = project.state;
                project.state = SiteProjectState::Pausing;
                p->save(project);
            } else if (disarm && hadSchedule)
                p->save(project);
            p->stopChildren(project, false);
        }
    });
}
std::vector<SiteProjectSummary> SiteProjectService::snapshot() const {
    std::lock_guard lock(impl_->viewMutex);
    return impl_->summaries;
}
std::optional<SiteProject> SiteProjectService::inspect(const std::string& id) const {
    std::lock_guard lock(impl_->viewMutex);
    const auto found = impl_->details.find(id);
    return found == impl_->details.end() ? std::nullopt : std::optional(*found->second);
}
std::string SiteProjectService::startupError() const {
    std::lock_guard lock(impl_->viewMutex);
    return impl_->storageError;
}
bool SiteProjectService::hasUnfinished() const {
    return impl_->unfinished.load() || impl_->pending.load() != 0;
}
} // namespace maskpilot
