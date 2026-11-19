#pragma once
#include <maskpilot/application/credential_vault.h>
#include <maskpilot/domain/download_context.h>
#include <maskpilot/domain/task.h>
#include <memory>
#include <vector>

namespace maskpilot {
class IContextRepository {
  public:
    virtual ~IContextRepository() = default;
    virtual std::string installationId() = 0;
    virtual std::vector<DownloadContext> loadContexts() = 0;
    virtual void saveContext(const DownloadContext&) = 0;
    virtual std::vector<CredentialRecord> loadCredentialRecords() = 0;
    // Durable ownership journal MUST precede a native vault write.
    virtual void saveCredentialRecord(const CredentialRecord&) = 0;
    virtual void eraseCredentialRecord(const std::string& reference) = 0;
};
struct ContextDependencies {
    std::unique_ptr<IContextRepository> repository;
    std::unique_ptr<ICredentialVault> vault;
};
struct ContextConfiguration {
    std::string id, externalKey, label;
    std::filesystem::path downloadRoot;
    std::string proxyEndpoint;
    bool proxyAuthenticationRequired = false, originAuthenticationRequired = false;
    std::int64_t expectedRevision = 0;
    // Explicit trusted administration; ordinary edits cannot undo revocation.
    bool reactivate = false;
};
struct ScopedDownloadDraft {
    std::string url, filename, originGrantId;
    bool startPaused = false;
};
enum class ScopedTaskCommand { Pause, Resume, Cancel, Remove };
struct ScopedTaskSummary {
    std::string id, name;
    TaskState state = TaskState::Paused;
    std::int64_t downloaded = 0, total = -1;
    bool browserRecoveryAvailable = false;
};
struct ScopedTaskPage {
    std::vector<ScopedTaskSummary> tasks;
    std::string nextCursor;
};
} // namespace maskpilot
