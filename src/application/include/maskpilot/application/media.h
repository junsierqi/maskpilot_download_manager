#pragma once
#include <functional>
#include <maskpilot/domain/task.h>
#include <stop_token>

namespace maskpilot {
class IMediaInspector {
  public:
    virtual ~IMediaInspector() = default;
    virtual MediaInventory inspect(const DownloadSpec&, std::stop_token) = 0;
};
struct MediaInspectionRequest {
    DownloadSpec specification;
    std::string taskId;
    std::string queueId = defaultQueueId;
};
using MediaInspectionFunction = std::function<MediaInventory(MediaInspectionRequest, std::stop_token)>;
} // namespace maskpilot
