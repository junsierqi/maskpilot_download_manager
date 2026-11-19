#include "payload_budget.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
namespace maskpilot {
namespace {
class NativePayloadBudget final : public INativePayloadBudget {
    std::shared_ptr<IQueueTrafficRepository> repository_;
    std::function<std::int64_t()> now_;
    TrafficRun run_;
    std::mutex mutex_;
    std::int64_t cumulative_ = 0, reserved_ = 0;
    bool finished_ = false;
    std::atomic_bool exhausted_{false}, failed_{false};

  public:
    NativePayloadBudget(std::shared_ptr<IQueueTrafficRepository> repository, const std::string& task,
                        const std::string& queue, std::function<std::int64_t()> now)
        : repository_(std::move(repository)), now_(std::move(now)) {
        if (!repository_ || !now_)
            throw std::invalid_argument("Native payload accounting is unavailable.");
        run_ = repository_->openRun(task, queue, now_());
    }
    std::size_t accept(std::size_t bytes) noexcept override {
        std::lock_guard lock(mutex_);
        if (!bytes || finished_ || failed_ || exhausted_)
            return 0;
        try {
            const auto utc = now_();
            if (repository_->currentPeriod(utc) != run_.period && !run_.rolling) {
                exhausted_ = true;
                return 0;
            }
            if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - cumulative_))
                throw std::runtime_error("Payload accounting overflow.");
            const auto requested = static_cast<std::int64_t>(bytes);
            if (reserved_ - cumulative_ < requested) {
                if (reserved_ > std::numeric_limits<std::int64_t>::max() - 65536)
                    throw std::runtime_error("Payload reservation overflow.");
                const auto grant = repository_->reserve(
                    run_.id, cumulative_,
                    std::min(maxDailyPayloadBudget, std::max(reserved_ + 65536, cumulative_ + requested)),
                    utc);
                if (grant.period != run_.period && !run_.rolling) {
                    exhausted_ = true;
                    return 0;
                }
                if (grant.reserved < reserved_ || grant.reserved < cumulative_)
                    throw std::runtime_error("Payload reservation regressed.");
                reserved_ = grant.reserved;
            }
            const auto accepted = std::min(requested, reserved_ - cumulative_);
            cumulative_ += accepted;
            if (accepted < requested)
                exhausted_ = true;
            return static_cast<std::size_t>(accepted);
        } catch (...) {
            failed_ = true;
            return 0;
        }
    }
    void finish() noexcept override {
        std::lock_guard lock(mutex_);
        if (finished_)
            return;
        finished_ = true;
        try {
            repository_->closeRunAt(run_.id, cumulative_, now_());
        } catch (...) {
            failed_ = true;
        }
    }
    bool exhausted() const noexcept override { return exhausted_.load(); }
    bool failed() const noexcept override { return failed_.load(); }
};
} // namespace
std::shared_ptr<INativePayloadBudget>
makeNativePayloadBudget(std::shared_ptr<IQueueTrafficRepository> repository, const std::string& taskId,
                        const std::string& queueId, std::function<std::int64_t()> now) {
    return std::make_shared<NativePayloadBudget>(std::move(repository), taskId, queueId, std::move(now));
}
} // namespace maskpilot
