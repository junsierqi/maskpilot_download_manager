#include <QDateTime>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>

using namespace maskpilot;
namespace {
std::int64_t instant(const char* iso) {
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODate).toSecsSinceEpoch();
}
} // namespace
TEST(QueueSchedule, HalfOpenOvernightAndAnchoredFullDayUseStartingWeekday) {
    WeeklySchedule s{true, 1, 9 * 60, 10 * 60, "UTC"};
    EXPECT_FALSE(weeklyWindowContains(s, 0, 539));
    EXPECT_TRUE(weeklyWindowContains(s, 0, 540));
    EXPECT_TRUE(weeklyWindowContains(s, 0, 599));
    EXPECT_FALSE(weeklyWindowContains(s, 0, 600));
    EXPECT_FALSE(weeklyWindowContains(s, 1, 540));
    s.startMinute = 23 * 60;
    s.endMinute = 60;
    s.weekdays = 1 << 6;
    EXPECT_TRUE(weeklyWindowContains(s, 6, 1380));
    EXPECT_TRUE(weeklyWindowContains(s, 0, 59));
    EXPECT_FALSE(weeklyWindowContains(s, 0, 60));
    EXPECT_FALSE(weeklyWindowContains(s, 6, 59));
    s.weekdays = 1;
    s.startMinute = s.endMinute = 540;
    EXPECT_FALSE(weeklyWindowContains(s, 0, 539));
    EXPECT_TRUE(weeklyWindowContains(s, 0, 540));
    EXPECT_TRUE(weeklyWindowContains(s, 1, 539));
    EXPECT_FALSE(weeklyWindowContains(s, 1, 540));
}
TEST(QueueSchedule, ActualCivilAdapterSkipsSpringGapAndIncludesBothFallOccurrences) {
    const auto clock = makeQueueCivilTime();
    const std::string zone = "America/New_York";
    const auto before = clock->at(zone, instant("2026-03-08T06:59:00Z"));
    const auto after = clock->at(zone, instant("2026-03-08T07:00:00Z"));
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    EXPECT_EQ(before->weekday, 6);
    EXPECT_EQ(before->minute, 119);
    EXPECT_EQ(after->minute, 180);
    WeeklySchedule gap{true, 64, 135, 165, zone};
    for (auto utc = instant("2026-03-08T06:00:00Z"); utc < instant("2026-03-08T08:00:00Z"); utc += 60) {
        const auto civil = clock->at(zone, utc);
        ASSERT_TRUE(civil);
        EXPECT_FALSE(weeklyWindowContains(gap, civil->weekday, civil->minute));
    }
    WeeklySchedule fold{true, 64, 75, 105, zone};
    for (const auto* utc : {"2026-11-01T05:30:00Z", "2026-11-01T06:30:00Z"}) {
        const auto civil = clock->at(zone, instant(utc));
        ASSERT_TRUE(civil);
        EXPECT_EQ(civil->minute, 90);
        EXPECT_TRUE(weeklyWindowContains(fold, civil->weekday, civil->minute));
    }
    const auto between = clock->at(zone, instant("2026-11-01T06:00:00Z"));
    ASSERT_TRUE(between);
    EXPECT_FALSE(weeklyWindowContains(fold, between->weekday, between->minute));
}
TEST(QueueSchedule, ActualCivilAdapterRejectsUnavailableZoneAndUsesExplicitZoneAcrossMidnight) {
    const auto clock = makeQueueCivilTime();
    EXPECT_FALSE(clock->at("Invented/Unavailable", instant("2026-11-15T16:30:00Z")));
    const auto local = clock->at("Asia/Shanghai", instant("2026-11-15T16:30:00Z"));
    ASSERT_TRUE(local);
    EXPECT_EQ(local->weekday, 0);
    EXPECT_EQ(local->minute, 30);
    EXPECT_NEAR(static_cast<double>(clock->utcNow()), static_cast<double>(QDateTime::currentSecsSinceEpoch()),
                2);
}
TEST(QueueSchedule, InvalidScheduleFieldsNeverPassDomainValidation) {
    DownloadQueue q;
    EXPECT_TRUE(validateDownloadQueue(q).empty());
    for (const int mask : {0, -1, 128}) {
        q.schedule.weekdays = mask;
        EXPECT_FALSE(validateDownloadQueue(q).empty());
    }
    q.schedule.weekdays = 127;
    q.schedule.startMinute = 1440;
    EXPECT_FALSE(validateDownloadQueue(q).empty());
    q.schedule.startMinute = 0;
    q.schedule.endMinute = -1;
    EXPECT_FALSE(validateDownloadQueue(q).empty());
    q.schedule.endMinute = 0;
    for (const auto* zone : {"", "UTC\n", "../UTC", "zone with spaces"}) {
        q.schedule.timeZone = zone;
        EXPECT_FALSE(validateDownloadQueue(q).empty());
    }
}
