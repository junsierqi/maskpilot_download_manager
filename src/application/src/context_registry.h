#pragma once
#include <map>
#include <maskpilot/application/download_context.h>
#include <optional>

namespace maskpilot {
// Private helper. All methods run on DownloadService's sole coordinator thread.
class ContextRegistry {
  public:
    explicit ContextRegistry(ContextDependencies dependencies);
    ~ContextRegistry();
    void initialize(const std::vector<Task>& savedTasks);
    DownloadContext configure(ContextConfiguration, ProxyCredentials, bool remember);
    std::vector<DownloadContext> list() const;
    std::string issueClient(const std::string& ownerId);
    std::string ownerFor(const std::string& token) const;
    Task prepareScoped(const std::string& token, const ScopedDownloadDraft&) const;
    DownloadSpec resolve(const Task&) const;
    std::string issueGrant(const std::string& ownerId, std::string url, std::string cookie,
                           std::string authorization, std::int64_t expiresAt);
    void beginRevoke(const std::string& ownerId);
    void finishRevoke(const std::string& ownerId);
    void rebind(Task&, const std::string& grantId) const;
    void rememberTask(Task&);
    void discardUncommitted(const Task&);
    bool enabled(const std::string& ownerId) const;
    std::string warning() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot
