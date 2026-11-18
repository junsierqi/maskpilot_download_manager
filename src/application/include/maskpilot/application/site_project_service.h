#pragma once
#include <future>
#include <maskpilot/application/download_service.h>
#include <maskpilot/application/site_content.h>
#include <maskpilot/domain/site_project.h>
#include <memory>

namespace maskpilot {
struct SiteProjectBody {
    std::string body, sha256;
};
struct SiteProjectArtifact {
    std::string name, body;
};
class ISiteProjectRepository {
  public:
    virtual ~ISiteProjectRepository() = default;
    virtual std::vector<SiteProject> load() = 0;
    virtual void save(const SiteProject&) = 0;
    virtual void erase(const std::string& id) = 0;
    virtual std::filesystem::path prepareRevision(const std::string& id, std::int64_t revision) = 0;
    virtual SiteProjectBody readBody(const SiteProject&, const SiteProjectEntry&) = 0;
};
class ISiteProjectPublisher {
  public:
    virtual ~ISiteProjectPublisher() = default;
    // Plan contains hashes and an opaque stage token; save it before commit performs filesystem writes.
    virtual SiteProjectPublication plan(const SiteProject&, const std::vector<SiteProjectArtifact>&) = 0;
    virtual void commit(const SiteProject&, const SiteProjectPublication&,
                        const std::vector<SiteProjectArtifact>&) = 0;
    // Absent target returns nullopt; an existing target must match marker and complete hash inventory.
    virtual std::optional<std::filesystem::path> resolve(const SiteProject&,
                                                         const SiteProjectPublication&) = 0;
    virtual void discardStage(const SiteProject&, const SiteProjectPublication&) = 0;
};
class SiteProjectService {
  public:
    SiteProjectService(DownloadService&, std::unique_ptr<ISiteProjectRepository>,
                       std::unique_ptr<ISiteContentProcessor>, std::unique_ptr<ISiteProjectPublisher>);
    ~SiteProjectService();
    SiteProjectService(const SiteProjectService&) = delete;
    SiteProjectService& operator=(const SiteProjectService&) = delete;
    std::future<std::string> create(SiteProjectOptions, ProxyCredentials = {});
    std::future<void> preview(std::string id, std::int64_t revision, ProxyCredentials = {});
    std::future<void> start(std::string id, std::int64_t revision, ProxyCredentials = {});
    std::future<void> pause(std::string id, std::int64_t revision);
    std::future<void> resume(std::string id, std::int64_t revision, ProxyCredentials = {});
    std::future<void> cancel(std::string id, std::int64_t revision);
    std::future<void> update(std::string id, std::int64_t revision, SiteProjectOptions,
                             ProxyCredentials = {});
    std::future<void> removeHistory(std::string id, std::int64_t revision);
    std::future<std::filesystem::path> openPath(std::string id);
    std::future<void> pauseAll();
    std::vector<SiteProjectSummary> snapshot() const;
    std::optional<SiteProject> inspect(const std::string& id) const;
    std::string startupError() const;
    bool hasUnfinished() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot
