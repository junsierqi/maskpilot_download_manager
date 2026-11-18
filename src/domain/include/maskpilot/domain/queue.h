#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace maskpilot {
inline constexpr char defaultQueueId[] = "default";
inline constexpr std::size_t maxDownloadQueues = 256;
inline constexpr std::int64_t minimumQueueRate = 1024;
inline constexpr std::int64_t maxDailyPayloadBudget = 1LL << 50;
enum class RunIntent { Run, Paused };
struct WeeklySchedule {
    bool enabled = false;
    int weekdays = 127; // Monday is bit 0.
    int startMinute = 0, endMinute = 0;
    std::string timeZone = "UTC";
};
// Equal endpoints mean 24 hours from the selected starting weekday's local time.
bool weeklyWindowContains(const WeeklySchedule&, int weekday, int minute);
struct DownloadQueue {
    std::string id = defaultQueueId;
    std::string name = "Default";
    std::int64_t revision = 1;
    bool enabled = true;
    int maxActive = 8;
    std::int64_t downloadLimit = 0, uploadLimit = 0;
    WeeklySchedule schedule;
    std::int64_t dailyPayloadBudget = 0;
};
bool isQueueId(const std::string&);
std::string validateDownloadQueue(const DownloadQueue&);
} // namespace maskpilot
