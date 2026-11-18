#include <algorithm>
#include <maskpilot/domain/download_context.h>
#include <maskpilot/domain/queue.h>

namespace maskpilot {
bool isQueueId(const std::string& id) { return id == defaultQueueId || isOpaqueId(id); }
std::string validateDownloadQueue(const DownloadQueue& queue) {
    if (!isQueueId(queue.id))
        return "Invalid download queue identity.";
    if (queue.name.empty() || queue.name.size() > 256 || queue.name.front() == ' ' ||
        queue.name.back() == ' ' || std::any_of(queue.name.begin(), queue.name.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7f;
        }))
        return "Queue name must be trimmed, nonempty and at most 256 bytes without control characters.";
    if (queue.revision < 1 || queue.maxActive < 1 || queue.maxActive > 8)
        return "Invalid download queue revision or concurrency limit.";
    for (const auto rate : {queue.downloadLimit, queue.uploadLimit})
        if (rate != 0 && rate < minimumQueueRate)
            return "Queue rates must be unlimited or at least 1024 bytes per second.";
    return {};
}
} // namespace maskpilot
