#pragma once
#include <maskpilot/domain/task.h>
#include <memory>
namespace maskpilot::media_process {
// One owned parser operation; source secrets remain process-local.
class MediaOriginGateway final {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit MediaOriginGateway(const DownloadSpec&);
    ~MediaOriginGateway();
    DownloadSpec routedRequest() const;
};
} // namespace maskpilot::media_process
