#pragma once
#include <cstdint>
#include <filesystem>
#include <maskpilot/domain/queue.h>
#include <maskpilot/domain/torrent.h>
#include <maskpilot/domain/version.h>
#include <string>
#include <utility>
#include <vector>

namespace maskpilot {
enum class TransferKind { Http, Ftp, Media, BitTorrent };
enum class TaskState {
    Queued,
    Scheduled,
    Probing,
    Downloading,
    Pausing,
    Paused,
    RetryWaiting,
    Verifying,
    Finalizing,
    Completed,
    Failed,
    Cancelling,
    Cancelled,
    NeedsAttention,
    FetchingMetadata,
    Seeding,
    AwaitingSelection,
    PolicyWaiting,
    Count
};
struct DownloadSpec {
    std::string url;
    std::filesystem::path destination;
    std::string category = "General";
    std::string referrer;
    std::string userAgent = defaultUserAgent;
    std::string cookie;
    std::string authorization;
    // Persist the requirement, never the source credentials. Missing material fails before contact.
    bool originAuthenticationRequired = false;
    std::string proxy;
    // Proxy secrets are process-local. The persisted marker prevents anonymous fallback after restart.
    std::string proxyUsername, proxyPassword;
    bool proxyAuthenticationRequired = false;
    std::string expectedSha256;
    // Credentials are process-local; only transport options are persisted.
    std::string ftpUsername, ftpPassword;
    bool ftpExplicitTls = false, ftpActiveMode = false;
    std::filesystem::path ftpCaBundle;
    std::vector<std::pair<std::string, std::string>> headers;
    int connections = 4;
    int priority = 0;
    int maxRetries = 3;
    std::int64_t speedLimit = 0;
    std::int64_t scheduledAt = 0;
    std::int64_t stopAt = 0;
    bool startPaused = false;
    TransferKind kind = TransferKind::Http;
    TorrentOptions torrent;
    bool audioOnly = false;
    std::string mediaFormat = "bestvideo+bestaudio/best";
    std::filesystem::path mediaDownloader;
    std::filesystem::path mediaMuxer;
};
struct Task {
    std::string id;
    // Trusted server metadata; never accepted from a browser DownloadSpec.
    std::string ownerId = "standalone";
    // Queue membership is trusted server metadata independent of the source owner and route.
    std::string queueId = defaultQueueId;
    RunIntent runIntent = RunIntent::Run;
    std::string requestId;
    std::string requestName;
    std::int64_t contextRevision = 0;
    std::string proxyCredentialRef;
    bool originAuthenticationRequired = false;
    // Per-start URL-specific capability; intentionally not persisted or exported.
    std::string originGrantId;
    DownloadSpec spec;
    TaskState state = TaskState::Queued;
    std::int64_t downloaded = 0;
    std::int64_t total = -1;
    std::int64_t bytesPerSecond = 0;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
    std::int64_t retryAt = 0;
    std::uint64_t generation = 0;
    int retries = 0;
    bool resumable = false;
    std::string error;
    TransferDescription torrent;
    std::int64_t uploaded = 0, uploadSpeed = 0;
    int peers = 0, seeds = 0;
    bool torrentPublished = false;
};
struct Segment {
    std::int64_t begin = 0, end = 0;
};
std::vector<Segment> splitSegments(std::int64_t length, int count);
std::string validateSpec(const DownloadSpec& spec);
bool hasOriginMaterial(const DownloadSpec& spec);
bool requiresOriginAuthentication(const DownloadSpec& spec);
std::string validateOriginAuthentication(const DownloadSpec& spec, bool requireMaterial = false);
bool isFtpUrl(const std::string& url);
bool isTorrentUrl(const std::string& url);
TransferKind sourceKind(const DownloadSpec& spec);
bool isActive(TaskState state);
bool isTerminal(TaskState state);
const char* stateName(TaskState state);
std::int64_t unixTime();
std::string newTaskId();
} // namespace maskpilot
