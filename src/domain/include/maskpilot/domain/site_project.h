#pragma once
#include <cstdint>
#include <filesystem>
#include <maskpilot/domain/queue.h>
#include <maskpilot/domain/site_content.h>
#include <optional>
#include <string>
#include <vector>

namespace maskpilot {
enum class SiteProjectState {
    Draft,
    Previewing,
    PreviewReady,
    Running,
    Pausing,
    Paused,
    Publishing,
    Completed,
    NeedsAttention,
    Cancelling,
    Cancelled,
    Count
};
enum class SiteProjectEntryState { Planned, Dispatching, Fetching, Ready, Processed, Skipped, Count };
struct SiteProjectOptions {
    std::string name = "Static site", rootUrl, queueId = defaultQueueId;
    std::filesystem::path outputDirectory;
    int maxDepth = 2, maxPages = 32, maxResources = 256;
    std::int64_t maxPageBytes = 2 * 1024 * 1024, maxAssetBytes = 8 * 1024 * 1024;
    std::int64_t maxStoredBytes = 128 * 1024 * 1024, maxOutputBytes = 256 * 1024 * 1024;
    std::vector<std::string> includePrefixes, excludePrefixes;
    bool styles = true, images = true, fonts = true, media = false;
    std::string proxy;
    bool proxyAuthenticationRequired = false;
};
struct SiteProjectEntry {
    std::string id, requestId, taskId, url;
    SiteContentKind kind = SiteContentKind::Html;
    int depth = 0;
    SiteProjectEntryState state = SiteProjectEntryState::Planned;
    std::filesystem::path destination;
    std::int64_t maxStoredBytes = 0, bytes = 0;
    std::string sha256, responseUrl, contentType, error;
};
struct SiteProjectFile {
    std::string name, sha256;
    std::int64_t bytes = 0;
};
struct SiteProjectPublication {
    std::int64_t revision = 0;
    std::string stageToken, indexName = "index.html";
    std::vector<SiteProjectFile> files;
};
struct SiteProject {
    std::string id;
    SiteProjectOptions options;
    std::int64_t revision = 1;
    SiteProjectState state = SiteProjectState::Draft, resumePhase = SiteProjectState::Previewing;
    std::vector<SiteProjectEntry> entries;
    std::optional<SiteProjectPublication> published, pendingPublication;
    std::int64_t excludedCount = 0;
    int syncIntervalMinutes = 0, syncRemainingRuns = 0;
    std::int64_t syncNextAt = 0, syncLastAt = 0;
    bool automaticRevision = false;
    std::string error;
};
struct SiteProjectSummary {
    std::string id, name, rootUrl, queueId, error;
    SiteProjectState state = SiteProjectState::Draft;
    std::int64_t revision = 0, publishedRevision = 0, storedBytes = 0, reservedBytes = 0;
    std::size_t documents = 0, resources = 0, ready = 0, skipped = 0;
    bool needsProxyCredentials = false;
};
std::string validateSiteProjectOptions(const SiteProjectOptions&);
std::string validateSiteProject(const SiteProject&);
bool siteProjectTerminal(SiteProjectState);
const char* siteProjectStateName(SiteProjectState);
const char* siteProjectEntryStateName(SiteProjectEntryState);
} // namespace maskpilot
