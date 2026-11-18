#include <algorithm>
#include <maskpilot/domain/download_context.h>
#include <maskpilot/domain/site_project.h>
#include <maskpilot/domain/task.h>
#include <set>

namespace maskpilot {
namespace {
bool plain(const std::string& value, std::size_t maximum, bool multiline = false) {
    return value.size() <= maximum && std::all_of(value.begin(), value.end(), [&](unsigned char ch) {
               return (ch >= 0x20 && ch != 0x7f) || (multiline && (ch == '\n' || ch == '\t' || ch == '\r'));
           });
}
bool hash(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'f') || (ch >= '0' && ch <= '9');
           });
}
bool fileName(const std::string& value) {
    if (value == "index.html")
        return true;
    if (value.size() < 36 || value.size() > 64 || value[1] != '-' || value[34] != '.' ||
        std::string_view("pcifm").find(value[0]) == std::string_view::npos ||
        !isOpaqueId(value.substr(2, 32)))
        return false;
    static const std::set<std::string> extensions = {"html", "css",  "png",  "jpg",   "gif", "webp", "avif",
                                                     "bmp",  "ico",  "woff", "woff2", "ttf", "otf",  "mp4",
                                                     "m4a",  "webm", "ogg",  "mp3",   "wav", "flac"};
    return extensions.contains(value.substr(35));
}
bool publication(const SiteProjectPublication& value, const SiteProject& project) {
    if (value.revision < 1 || value.revision > project.revision || !isOpaqueId(value.stageToken) ||
        value.indexName != "index.html" || value.files.empty() || value.files.size() > 1000)
        return false;
    std::set<std::string> names;
    std::int64_t total = 0;
    const auto maximum =
        value.revision == project.revision ? project.options.maxOutputBytes : 512LL * 1024 * 1024;
    for (const auto& file : value.files) {
        if (!fileName(file.name) || !hash(file.sha256) || file.bytes < 0 || file.bytes > maximum - total ||
            !names.insert(file.name).second)
            return false;
        total += file.bytes;
    }
    return names.contains("index.html");
}
} // namespace
std::string validateSiteProjectOptions(const SiteProjectOptions& options) {
    if (options.name.empty() || !plain(options.name, 128) || !isQueueId(options.queueId) ||
        !options.outputDirectory.is_absolute() || options.outputDirectory.native().size() > 32768 ||
        options.maxDepth < 0 || options.maxDepth > 8 || options.maxPages < 1 || options.maxPages > 128 ||
        options.maxResources < options.maxPages || options.maxResources > 1000 || options.maxPageBytes < 1 ||
        options.maxPageBytes > 8 * 1024 * 1024 || options.maxAssetBytes < 1 ||
        options.maxAssetBytes > 8 * 1024 * 1024 || options.maxStoredBytes < 1 ||
        options.maxStoredBytes > 512LL * 1024 * 1024 || options.maxOutputBytes < 1 ||
        options.maxOutputBytes > 512LL * 1024 * 1024 || options.includePrefixes.size() > 16 ||
        options.excludePrefixes.size() > 16)
        return "Invalid site project options or bounds.";
    DownloadSpec source;
    source.url = options.rootUrl;
    source.destination = options.outputDirectory / "index.html";
    source.proxy = options.proxy;
    source.proxyAuthenticationRequired = options.proxyAuthenticationRequired;
    if (sourceKind(source) != TransferKind::Http || !validateSpec(source).empty())
        return "Choose a valid public HTTP(S) root and proxy route.";
    for (const auto* prefixes : {&options.includePrefixes, &options.excludePrefixes})
        for (const auto& prefix : *prefixes)
            if (prefix.empty() || prefix.front() != '/' || !plain(prefix, 512))
                return "Path filters must be bounded prefixes beginning with /.";
    return {};
}
std::string validateSiteProject(const SiteProject& project) {
    if (const auto error = validateSiteProjectOptions(project.options); !error.empty())
        return error;
    if (!isOpaqueId(project.id) || project.revision < 1 || project.revision > 1000000 ||
        project.state < SiteProjectState::Draft || project.state >= SiteProjectState::Count ||
        (project.resumePhase != SiteProjectState::Previewing &&
         project.resumePhase != SiteProjectState::Running) ||
        project.entries.size() > static_cast<std::size_t>(project.options.maxResources) ||
        project.excludedCount < 0 || project.excludedCount > 20000000 || !plain(project.error, 4096, true))
        return "Invalid persistent site project state.";
    std::set<std::string> ids, requests, tasks, urls;
    std::int64_t stored = 0;
    std::size_t pages = 0;
    for (const auto& item : project.entries) {
        if (!isOpaqueId(item.id) || !isOpaqueId(item.requestId) ||
            (!item.taskId.empty() && (!isOpaqueId(item.taskId) || !tasks.insert(item.taskId).second)) ||
            !ids.insert(item.id).second || !requests.insert(item.requestId).second ||
            !urls.insert(item.url).second || item.kind < SiteContentKind::Html ||
            item.kind >= SiteContentKind::Unsupported || item.depth < 0 ||
            item.depth > project.options.maxDepth || item.state < SiteProjectEntryState::Planned ||
            item.state >= SiteProjectEntryState::Count || !item.destination.is_absolute() ||
            item.destination.native().size() > 32768 || item.maxStoredBytes < 0 ||
            item.maxStoredBytes > 8 * 1024 * 1024 || item.bytes < 0 || item.bytes > item.maxStoredBytes ||
            !plain(item.error, 4096, true) || !validHttpResponseContentType(item.contentType) ||
            (!item.sha256.empty() && !hash(item.sha256)) ||
            (!item.contentType.empty() && item.responseUrl.empty()))
            return "Invalid persistent site project entry.";
        DownloadSpec source;
        source.url = item.url;
        source.destination = item.destination;
        if (sourceKind(source) != TransferKind::Http || !validateSpec(source).empty())
            return "Invalid site project resource URL.";
        if (!item.responseUrl.empty()) {
            source.url = item.responseUrl;
            if (sourceKind(source) != TransferKind::Http || !validateSpec(source).empty())
                return "Invalid observed site project resource URL.";
        }
        const bool done = item.state == SiteProjectEntryState::Ready ||
                          item.state == SiteProjectEntryState::Processed ||
                          item.state == SiteProjectEntryState::Skipped;
        const auto charge = done ? item.bytes : item.maxStoredBytes;
        if (charge > project.options.maxStoredBytes - stored)
            return "Site project stored body reservations exceed the budget.";
        stored += charge;
        if (item.kind == SiteContentKind::Html &&
            ++pages > static_cast<std::size_t>(project.options.maxPages))
            return "Site project page count exceeds its limit.";
        if (item.state != SiteProjectEntryState::Planned &&
            (item.maxStoredBytes == 0 ||
             (item.taskId.empty() && item.state != SiteProjectEntryState::Dispatching)))
            return "Missing durable site project child binding.";
        if ((item.state == SiteProjectEntryState::Ready || item.state == SiteProjectEntryState::Processed) &&
            (item.sha256.empty() || item.responseUrl.empty()))
            return "Missing verified site project content observation.";
    }
    if ((project.published && !publication(*project.published, project)) ||
        (project.pendingPublication && (!publication(*project.pendingPublication, project) ||
                                        project.pendingPublication->revision != project.revision)))
        return "Invalid immutable site publication inventory.";
    if (!project.entries.empty() &&
        (project.entries.front().url != project.options.rootUrl || project.entries.front().depth != 0 ||
         project.entries.front().kind != SiteContentKind::Html))
        return "Invalid root entry in the site project plan.";
    if ((project.state == SiteProjectState::PreviewReady || project.state == SiteProjectState::Completed) &&
        (project.entries.empty() || project.entries.front().state != SiteProjectEntryState::Processed))
        return "Site project has no verified root document.";
    if (project.state == SiteProjectState::Publishing && !project.pendingPublication)
        return "Missing durable publication plan.";
    if (project.state == SiteProjectState::Completed &&
        (!project.published || project.published->revision != project.revision))
        return "Completed site project has no current publication.";
    return {};
}
bool siteProjectTerminal(SiteProjectState state) {
    return state == SiteProjectState::Completed || state == SiteProjectState::Cancelled;
}
const char* siteProjectStateName(SiteProjectState state) {
    static constexpr const char* names[] = {"Draft",           "Previewing", "Preview ready", "Running",
                                            "Pausing",         "Paused",     "Publishing",    "Completed",
                                            "Needs attention", "Cancelling", "Cancelled"};
    return state >= SiteProjectState::Draft && state < SiteProjectState::Count
               ? names[static_cast<int>(state)]
               : "Invalid";
}
const char* siteProjectEntryStateName(SiteProjectEntryState state) {
    static constexpr const char* names[] = {"Planned", "Dispatching", "Fetching",
                                            "Ready",   "Processed",   "Skipped"};
    return state >= SiteProjectEntryState::Planned && state < SiteProjectEntryState::Count
               ? names[static_cast<int>(state)]
               : "Invalid";
}
} // namespace maskpilot
