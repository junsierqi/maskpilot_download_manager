#pragma once
#include <map>
#include <maskpilot/application/queue.h>
#include <maskpilot/domain/task.h>
namespace maskpilot {
class QueueRegistry {
  public:
    explicit QueueRegistry(QueueDependencies);
    const DownloadQueue& get(const std::string&) const;
    std::vector<DownloadQueue> list() const;
    DownloadQueue prepare(QueueConfiguration) const;
    void commit(const DownloadQueue&, std::int64_t expected);
    void remove(const std::string&, std::int64_t);
    void move(const std::vector<QueueMove>&, const std::string&, std::int64_t);

  private:
    std::unique_ptr<IQueueRepository> repository_;
    std::map<std::string, DownloadQueue> queues_;
};
// A fixed rate of -1 denotes a native task; 0 is an unlimited media reservation.
struct RateClaim {
    std::string id, queueId;
    std::int64_t cap = 0, fixed = -1;
};
std::map<std::string, std::int64_t> allocateQueueRates(std::int64_t global, const std::vector<DownloadQueue>&,
                                                       const std::vector<RateClaim>&, bool upload = false);
std::int64_t mediaReservation(std::int64_t global, int activeSlots, const DownloadQueue&,
                              std::int64_t taskCap);
} // namespace maskpilot
