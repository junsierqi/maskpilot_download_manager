#pragma once
#include <future>
#include <maskpilot/application/download_context.h>
#include <maskpilot/application/ports.h>
#include <maskpilot/application/queue.h>
#include <optional>

namespace maskpilot {
class DownloadService {
  public:
    DownloadService(std::unique_ptr<ITaskRepository> repository, TransportFactory factory,
                    std::filesystem::path workDirectory, ContextDependencies contexts = {},
                    QueueDependencies queues = {});
    ~DownloadService();
    DownloadService(const DownloadService&) = delete;
    DownloadService& operator=(const DownloadService&) = delete;
    // Resolves only after the task is durably persisted. Safe for browser handoff acknowledgement.
    std::future<std::string> add(DownloadSpec spec, std::string requestId = {}, bool rememberProxy = false,
                                 std::string queueId = defaultQueueId);
    // Trusted local queue administration; never accept these fields through scoped drafts.
    std::future<std::vector<DownloadQueue>> queues();
    std::future<TrafficUsage> queueTraffic(std::string queueId);
    std::future<DownloadQueue> configureQueue(QueueConfiguration);
    std::future<void> moveToQueue(std::vector<std::string> taskIds, std::string queueId);
    std::future<void> removeQueue(std::string queueId, std::int64_t expectedRevision);
    std::future<void> configureLimits(int concurrentTasks, std::int64_t globalBytesPerSecond,
                                      std::int64_t globalUploadBytesPerSecond = 0);
    // Trusted in-process administration. The IPC adapter must authenticate its own admin authority.
    std::future<DownloadContext> configureContext(ContextConfiguration, ProxyCredentials = {},
                                                  bool rememberProxy = false);
    std::future<std::vector<DownloadContext>> contexts();
    std::future<std::string> issueClient(const std::string& ownerId);
    std::future<std::string> issueOriginGrant(const std::string& ownerId, std::string url, std::string cookie,
                                              std::string authorization, std::int64_t expiresAt);
    std::future<void> revokeContext(const std::string& ownerId);
    std::future<void> rebindTask(const std::string& taskId, std::string grantId = {});
    // Only these narrow operations are exposed to browser context clients.
    std::future<std::string> addScoped(std::string token, ScopedDownloadDraft, std::string requestId);
    std::future<ScopedTaskPage> listScoped(std::string token, std::size_t limit = 1000,
                                           std::string afterId = {});
    std::future<void> controlScoped(std::string token, ScopedTaskCommand, std::string taskId);
    void pause(const std::string& id);
    void resume(const std::string& id);
    void cancel(const std::string& id);
    void remove(const std::string& id);
    void setLimits(int concurrentTasks, std::int64_t globalBytesPerSecond,
                   std::int64_t globalUploadBytesPerSecond = 0);
    std::future<std::string> selectTorrentFiles(const std::string& id, std::vector<int> indices,
                                                bool allFiles = false);
    void stopSeeding(const std::string& id);
    // Validate/capture an entire imported list without changing tasks or blocking the coordinator.
    std::future<std::vector<DownloadSpec>> prepareImport(std::vector<DownloadSpec> specs);
    void setPriority(const std::string& id, int priority);
    void updateUrl(const std::string& id, std::string url);
    void updateRequest(const std::string& id, DownloadSpec spec);
    // Complete trusted desktop edit; explicit persistence and source-requirement choices.
    std::future<void> updateRequestWithCredentials(const std::string& id, DownloadSpec spec,
                                                   bool rememberProxy);
    std::future<void> forgetSavedProxyCredentials(const std::string& id);
    std::future<void> retryCredentialCleanup();
    void restart(const std::string& id);
    void schedule(const std::string& id, std::int64_t startAt, std::int64_t stopAt);
    std::vector<Task> snapshot() const;
    std::string lastError() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot
