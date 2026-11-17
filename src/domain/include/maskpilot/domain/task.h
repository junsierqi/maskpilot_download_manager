#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace maskpilot {
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
    NeedsAttention
};
struct DownloadSpec {
    std::string url;
    std::filesystem::path destination;
    std::string category = "General";
    std::string referrer;
    std::string userAgent = "MaskPilot/0.1";
    std::string cookie;
    std::string authorization;
    std::string proxy;
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
    bool media = false;
    bool audioOnly = false;
    std::string mediaFormat = "bestvideo+bestaudio/best";
    std::filesystem::path mediaDownloader;
    std::filesystem::path mediaMuxer;
};
struct Task {
    std::string id;
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
};
struct Segment {
    std::int64_t begin = 0, end = 0;
};
std::vector<Segment> splitSegments(std::int64_t length, int count);
std::string validateSpec(const DownloadSpec& spec);
bool isFtpUrl(const std::string& url);
bool isActive(TaskState state);
bool isTerminal(TaskState state);
const char* stateName(TaskState state);
std::int64_t unixTime();
std::string newTaskId();
} // namespace maskpilot
