#pragma once
#include <maskpilot/application/traffic.h>
#include <maskpilot/domain/queue.h>
#include <memory>
#include <optional>
#include <vector>

namespace maskpilot {
struct QueueConfiguration {
    std::string id, name;
    std::int64_t expectedRevision = 0;
    bool enabled = true;
    int maxActive = 8;
    std::int64_t downloadLimit = 0, uploadLimit = 0;
    WeeklySchedule schedule;
    std::int64_t dailyPayloadBudget = 0;
};
struct QueueMove {
    std::string taskId, expectedQueueId;
};
class IQueueRepository {
  public:
    virtual ~IQueueRepository() = default;
    virtual std::vector<DownloadQueue> loadQueues() = 0;
    virtual void saveQueue(const DownloadQueue&, std::int64_t expectedRevision) = 0;
    virtual void eraseQueue(const std::string& id, std::int64_t expectedRevision) = 0;
    // Conditional membership changes and updatedAt commit together, or none commit.
    virtual void moveTasks(const std::vector<QueueMove>&, const std::string& targetQueue,
                           std::int64_t updatedAt) = 0;
};
struct CivilMinute {
    int weekday = 0, minute = 0;
};
class IQueueCivilTime {
  public:
    virtual ~IQueueCivilTime() = default;
    virtual std::int64_t utcNow() const = 0;
    virtual std::optional<CivilMinute> at(const std::string& zone, std::int64_t utc) const = 0;
};
struct InitialQueueLimits {
    int concurrentTasks = 3;
    std::int64_t downloadLimit = 0, uploadLimit = 0;
};
struct QueueDependencies {
    std::unique_ptr<IQueueRepository> repository;
    std::shared_ptr<IQueueCivilTime> clock;
    std::optional<InitialQueueLimits> initialLimits;
    std::shared_ptr<IQueueTrafficRepository> traffic;
};
} // namespace maskpilot
