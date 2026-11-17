#pragma once
#include <atomic>
#include <functional>
#include <maskpilot/domain/task.h>
#include <memory>

namespace maskpilot {
class ITaskRepository {
  public:
    virtual ~ITaskRepository() = default;
    virtual std::vector<Task> load() = 0;
    virtual void save(const Task& task) = 0;
    virtual void erase(const std::string& id) = 0;
};
enum class TransferOutcome { Progress, Complete, Paused, Cancelled, Failed, Attention };
struct TransferEvent {
    TransferOutcome outcome = TransferOutcome::Progress;
    TaskState phase = TaskState::Downloading;
    std::int64_t downloaded = 0, total = -1, speed = 0;
    bool resumable = false;
    bool retryable = false;
    int retryAfter = 0;
    std::string error;
};
struct TransferControl {
    std::atomic_bool pause{false}, cancel{false};
    std::atomic<std::int64_t> speedLimit{0};
};
class ITransferTransport {
  public:
    virtual ~ITransferTransport() = default;
    virtual void run(const Task& task, const std::filesystem::path& workDirectory, TransferControl& control,
                     const std::function<void(TransferEvent)>& callback) = 0;
};
using TransportFactory = std::function<std::unique_ptr<ITransferTransport>()>;
} // namespace maskpilot
