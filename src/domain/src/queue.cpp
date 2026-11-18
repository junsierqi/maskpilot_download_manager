#include <algorithm>
#include <maskpilot/domain/download_context.h>
#include <maskpilot/domain/queue.h>

namespace maskpilot {
bool isQueueId(const std::string& id) { return id == defaultQueueId || isOpaqueId(id); }
bool weeklyWindowContains(const WeeklySchedule& schedule, int weekday, int minute) {
    if (weekday < 0 || weekday > 6 || minute < 0 || minute >= 1440 || schedule.weekdays < 1 ||
        schedule.weekdays > 127 || schedule.startMinute < 0 || schedule.startMinute >= 1440 ||
        schedule.endMinute < 0 || schedule.endMinute >= 1440)
        return false;
    if (!schedule.enabled)
        return true;
    const auto selected = [&](int day) { return (schedule.weekdays & (1 << day)) != 0; };
    if (schedule.endMinute > schedule.startMinute)
        return selected(weekday) && minute >= schedule.startMinute && minute < schedule.endMinute;
    return (selected(weekday) && minute >= schedule.startMinute) ||
           (selected((weekday + 6) % 7) && minute < schedule.endMinute);
}
std::string validateDownloadQueue(const DownloadQueue& queue) {
    if (!isQueueId(queue.id))
        return "Invalid download queue identity.";
    if (queue.name.empty() || queue.name.size() > 256 || queue.name.front() == ' ' ||
        queue.name.back() == ' ' || std::any_of(queue.name.begin(), queue.name.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7f;
        }))
        return "Queue name must be trimmed, nonempty and at most 256 bytes without control characters.";
    if (queue.revision < 1 || queue.maxActive < 1 || queue.maxActive > 8)
        return "Invalid download queue revision or concurrency limit.";
    for (const auto rate : {queue.downloadLimit, queue.uploadLimit})
        if (rate != 0 && rate < minimumQueueRate)
            return "Queue rates must be unlimited or at least 1024 bytes per second.";
    const auto& schedule = queue.schedule;
    if (queue.dailyPayloadBudget < 0 || queue.dailyPayloadBudget > maxDailyPayloadBudget)
        return "Daily native payload budget must be between zero and one PiB.";
    if (schedule.weekdays < 1 || schedule.weekdays > 127 || schedule.startMinute < 0 ||
        schedule.startMinute >= 1440 || schedule.endMinute < 0 || schedule.endMinute >= 1440)
        return "Weekly queue days and times are invalid.";
    const auto& zone = schedule.timeZone;
    if (zone.empty() || zone.size() > 128 || zone.front() == '/' || zone.back() == '/' ||
        zone.find("//") != std::string::npos || zone == "." || zone == ".." || zone.starts_with("./") ||
        zone.starts_with("../") || zone.ends_with("/.") || zone.ends_with("/..") ||
        zone.find("/./") != std::string::npos || zone.find("/../") != std::string::npos ||
        std::any_of(zone.begin(), zone.end(), [](unsigned char c) {
            return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
                     c == '_' || c == '-' || c == '+' || c == '.');
        }))
        return "Use a bounded IANA time zone identifier without whitespace or control characters.";
    return {};
}
} // namespace maskpilot
