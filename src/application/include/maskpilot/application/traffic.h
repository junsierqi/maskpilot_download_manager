#pragma once
#include <cstdint>
#include <string>

namespace maskpilot {
struct TrafficRun {
    std::string id;
    std::int64_t period = 0; // UTC day start, epoch seconds; immutable for this run.
};
struct TrafficGrant {
    std::int64_t reserved = 0; // Absolute run reservation highwater, not an additional credit amount.
    std::int64_t period = 0;   // Current effective UTC day; different from run day means stop.
};
struct TrafficUsage {
    std::int64_t period = 0, charged = 0, observed = 0;
};
// Worker-safe facade: calls commit before returning, never call the scheduling actor.
class IQueueTrafficRepository {
  public:
    virtual ~IQueueTrafficRepository() = default;
    virtual TrafficRun openRun(const std::string& taskId, const std::string& queueId, std::int64_t utc) = 0;
    // Repeated/stale cumulative values never debit twice or reduce observed usage.
    virtual TrafficGrant reserve(const std::string& runId, std::int64_t cumulativeObserved,
                                 std::int64_t desiredReservedTotal, std::int64_t utc) = 0;
    virtual void closeRun(const std::string& runId, std::int64_t finalObserved) = 0;
    virtual TrafficUsage usage(const std::string& queueId, std::int64_t utc) = 0;
    virtual std::int64_t currentPeriod(std::int64_t utc) = 0;
    virtual void abandonOpenRuns() = 0;
};
} // namespace maskpilot
