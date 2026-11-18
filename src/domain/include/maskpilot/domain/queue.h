#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace maskpilot {
inline constexpr char defaultQueueId[] = "default";
inline constexpr std::size_t maxDownloadQueues = 256;
inline constexpr std::int64_t minimumQueueRate = 1024;
enum class RunIntent { Run, Paused };
struct DownloadQueue {
    std::string id = defaultQueueId;
    std::string name = "Default";
    std::int64_t revision = 1;
    bool enabled = true;
    int maxActive = 8;
    std::int64_t downloadLimit = 0, uploadLimit = 0;
};
bool isQueueId(const std::string&);
std::string validateDownloadQueue(const DownloadQueue&);
} // namespace maskpilot
