#pragma once
#include <future>
#include <maskpilot/application/ports.h>
#include <optional>

namespace maskpilot {
class DownloadService {
  public:
    DownloadService(std::unique_ptr<ITaskRepository> repository, TransportFactory factory,
                    std::filesystem::path workDirectory);
    ~DownloadService();
    DownloadService(const DownloadService&) = delete;
    DownloadService& operator=(const DownloadService&) = delete;
    // Resolves only after the task is durably persisted. Safe for browser handoff acknowledgement.
    std::future<std::string> add(DownloadSpec spec, std::string requestId = {});
    void pause(const std::string& id);
    void resume(const std::string& id);
    void cancel(const std::string& id);
    void remove(const std::string& id);
    void setLimits(int concurrentTasks, std::int64_t globalBytesPerSecond);
    void setPriority(const std::string& id, int priority);
    void updateUrl(const std::string& id, std::string url);
    void updateRequest(const std::string& id, DownloadSpec spec);
    void restart(const std::string& id);
    void schedule(const std::string& id, std::int64_t startAt, std::int64_t stopAt);
    std::vector<Task> snapshot() const;
    std::string lastError() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot
