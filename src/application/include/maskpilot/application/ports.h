#pragma once
#include <atomic>
#include <functional>
#include <maskpilot/domain/task.h>
#include <memory>

namespace maskpilot {
// Shared numeric part/offset cache namespace; bounds both recovery and exact owned cleanup.
inline constexpr int maxRecoveryExtents = 256;
class ITaskRepository {
  public:
    virtual ~ITaskRepository() = default;
    virtual std::vector<Task> load() = 0;
    virtual void save(const Task& task) = 0;
    virtual void erase(const std::string& id) = 0;
};
enum class TransferOutcome { Progress, Complete, Paused, Cancelled, Failed, Attention, SelectionRequired };
struct TransferEvent {
    TransferOutcome outcome = TransferOutcome::Progress;
    TaskState phase = TaskState::Downloading;
    std::int64_t downloaded = 0, total = -1, speed = 0;
    bool resumable = false;
    bool retryable = false;
    int retryAfter = 0;
    std::string error;
    std::shared_ptr<const TransferDescription> description;
    TorrentMetadata torrentMetadata;
    std::int64_t uploaded = 0, uploadSpeed = 0;
    int peers = 0, seeds = 0;
    bool torrentPublished = false;
    std::string responseUrl;
};
class INativePayloadBudget {
  public:
    virtual ~INativePayloadBudget() = default;
    virtual std::size_t accept(std::size_t bytes) noexcept = 0;
    virtual void finish() noexcept = 0;
    virtual bool exhausted() const noexcept = 0;
    virtual bool failed() const noexcept = 0;
};
struct TransferControl {
    std::atomic_bool pause{false}, cancel{false};
    std::atomic<std::int64_t> speedLimit{0};
    std::atomic<std::int64_t> uploadLimit{0};
    std::atomic_bool finish{false};
    std::shared_ptr<INativePayloadBudget> payloadBudget;
};
class ITransferTransport {
  public:
    virtual ~ITransferTransport() = default;
    // Bounded local metadata inspection only; must never start network activity.
    virtual TransferDescription describe(const DownloadSpec&) { return {}; }
    // Capture a bounded local source without network access; protocol adapter owns platform checks.
    virtual TorrentMetadata captureMetadata(const DownloadSpec& spec) { return spec.torrent.metadata; }
    // Called only after the worker releases its resources. Preserve all published/user files.
    virtual void discard(const Task&, const std::filesystem::path&) {}
    virtual void run(const Task& task, const std::filesystem::path& workDirectory, TransferControl& control,
                     const std::function<void(TransferEvent)>& callback) = 0;
};
using TransportFactory = std::function<std::unique_ptr<ITransferTransport>()>;
} // namespace maskpilot
