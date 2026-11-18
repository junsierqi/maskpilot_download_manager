#include <algorithm>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <sqlite3.h>

namespace {
constexpr std::int64_t day = 86400;
struct Temp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-traffic-store-" + maskpilot::newTaskId());
    Temp() { std::filesystem::create_directories(path); }
    ~Temp() {
        std::error_code error;
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().string().starts_with("maskpilot-traffic-store-"))
            std::filesystem::remove_all(path, error);
    }
};
struct Fixture {
    Temp temporary;
    maskpilot::SqliteStore store = maskpilot::makeSqliteStore(temporary.path / "tasks.db");
    explicit Fixture(std::int64_t budget = 100) {
        auto q = store.queues->loadQueues()[0];
        ++q.revision;
        q.dailyPayloadBudget = budget;
        store.queues->saveQueue(q, 1);
    }
    maskpilot::Task add(std::string queue = maskpilot::defaultQueueId) {
        maskpilot::Task task;
        task.id = maskpilot::newTaskId();
        task.queueId = std::move(queue);
        task.state = maskpilot::TaskState::Paused;
        task.runIntent = maskpilot::RunIntent::Paused;
        task.spec.url = "https://example.test/file";
        task.spec.destination = temporary.path / task.id;
        store.tasks->save(task);
        return task;
    }
};
} // namespace

TEST(TrafficStorage, MigratesRealVersionEightWithBackupAndBudgetCASDefaults) {
    Temp temporary;
    const auto path = temporary.path / "tasks.db";
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> close(raw, sqlite3_close);
    for (const auto* name :
         {"0001_initial.sql", "0002_media.sql", "0003_ftp.sql", "0004_torrents.sql", "0005_proxy.sql",
          "0006_contexts.sql", "0007_queues.sql", "0008_queue_windows.sql"}) {
        std::ifstream input(root / "database" / "migrations" / name);
        ASSERT_TRUE(input.good()) << name;
        const std::string sql((std::istreambuf_iterator<char>(input)), {});
        ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << name;
    }
    ASSERT_EQ(sqlite3_exec(
                  raw,
                  "INSERT INTO store_metadata VALUES('installation_id','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa');",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
    close.reset();
    {
        auto store = maskpilot::makeSqliteStore(path);
        EXPECT_EQ(store.contexts->installationId(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        auto q = store.queues->loadQueues()[0];
        EXPECT_EQ(q.dailyPayloadBudget, 0);
        EXPECT_FALSE(q.schedule.enabled);
        ++q.revision;
        q.dailyPayloadBudget = maskpilot::maxDailyPayloadBudget;
        store.queues->saveQueue(q, 1);
        EXPECT_THROW(store.queues->saveQueue(q, 1), std::exception);
        ++q.revision;
        q.dailyPayloadBudget = maskpilot::maxDailyPayloadBudget + 1;
        EXPECT_THROW(store.queues->saveQueue(q, 2), std::exception);
        q.dailyPayloadBudget = -1;
        EXPECT_THROW(store.queues->saveQueue(q, 2), std::exception);
    }
    auto reopened = maskpilot::makeSqliteStore(path);
    EXPECT_EQ(reopened.queues->loadQueues()[0].dailyPayloadBudget, maskpilot::maxDailyPayloadBudget);
    EXPECT_EQ(reopened.traffic->usage(maskpilot::defaultQueueId, day).charged, 0);
    int backups = 0;
    for (const auto& file : std::filesystem::directory_iterator(temporary.path)) {
        if (file.path().filename().string().find(".before-v9-") == std::string::npos)
            continue;
        ++backups;
        sqlite3* backup = nullptr;
        ASSERT_EQ(sqlite3_open_v2(file.path().string().c_str(), &backup, SQLITE_OPEN_READONLY, nullptr),
                  SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> closed(backup, sqlite3_close);
        sqlite3_stmt* q = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(backup, "PRAGMA user_version", -1, &q, nullptr), SQLITE_OK);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> finalized(q, sqlite3_finalize);
        ASSERT_EQ(sqlite3_step(q), SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(q, 0), 8);
    }
    EXPECT_EQ(backups, 1);
}

TEST(TrafficStorage, ConcurrentLastCreditReservationsAndDuplicateHighwatersNeverDebitTwice) {
    Fixture fixture;
    const auto a = fixture.add(), b = fixture.add();
    const auto first = fixture.store.traffic->openRun(a.id, a.queueId, day + 1);
    const auto second = fixture.store.traffic->openRun(b.id, b.queueId, day + 1);
    auto one = std::async(std::launch::async,
                          [&] { return fixture.store.traffic->reserve(first.id, 0, 80, day + 2); });
    auto two = std::async(std::launch::async,
                          [&] { return fixture.store.traffic->reserve(second.id, 0, 80, day + 2); });
    const auto grantedA = one.get().reserved, grantedB = two.get().reserved;
    EXPECT_EQ(grantedA + grantedB, 100);
    EXPECT_EQ(fixture.store.traffic->reserve(first.id, 0, 80, day + 3).reserved, grantedA);
    EXPECT_EQ(fixture.store.traffic->reserve(first.id, grantedA / 2, grantedA, day + 3).reserved, grantedA);
    EXPECT_EQ(fixture.store.traffic->reserve(first.id, 0, 0, day + 3).reserved, grantedA);
    auto usage = fixture.store.traffic->usage(a.queueId, day + 4);
    EXPECT_EQ(usage.charged, 100);
    EXPECT_EQ(usage.observed, grantedA / 2);
    EXPECT_THROW(fixture.store.traffic->reserve(first.id, grantedA + 1, 100, day + 4), std::exception);
    EXPECT_THROW(fixture.store.traffic->reserve(first.id, 0, maskpilot::maxDailyPayloadBudget + 1, day + 4),
                 std::exception);
    fixture.store.traffic->closeRun(first.id, grantedA / 2);
    fixture.store.traffic->closeRun(first.id, 0);
    fixture.store.traffic->closeRun(second.id, grantedB);
    usage = fixture.store.traffic->usage(a.queueId, day + 4);
    EXPECT_EQ(usage.charged, grantedA / 2 + grantedB);
    EXPECT_EQ(usage.observed, usage.charged);
    EXPECT_THROW(fixture.store.traffic->reserve(first.id, 0, 100, day + 4), std::exception);
    EXPECT_THROW(fixture.store.traffic->closeRun(first.id, grantedA), std::exception);
}

TEST(TrafficStorage, CrashOutstandingReservationsRemainChargedAfterAbandonAndReopen) {
    Fixture fixture;
    const auto task = fixture.add();
    const auto run = fixture.store.traffic->openRun(task.id, task.queueId, day);
    EXPECT_EQ(fixture.store.traffic->reserve(run.id, 0, 80, day).reserved, 80);
    fixture.store.traffic->reserve(run.id, 25, 80, day);
    fixture.store = {};
    fixture.store = maskpilot::makeSqliteStore(fixture.temporary.path / "tasks.db");
    fixture.store.traffic->abandonOpenRuns();
    fixture.store.traffic->abandonOpenRuns();
    auto usage = fixture.store.traffic->usage(task.queueId, day);
    EXPECT_EQ(usage.charged, 80);
    EXPECT_EQ(usage.observed, 25);
    EXPECT_THROW(fixture.store.traffic->closeRun(run.id, 25), std::exception);
    EXPECT_THROW(fixture.store.traffic->reserve(run.id, 25, 100, day), std::exception);
    const auto next = fixture.store.traffic->openRun(task.id, task.queueId, day);
    EXPECT_EQ(fixture.store.traffic->reserve(next.id, 0, 80, day).reserved, 20);
    fixture.store.traffic->closeRun(next.id, 20);
    EXPECT_EQ(fixture.store.traffic->usage(task.queueId, day).charged, 100);
}

TEST(TrafficStorage, MonotoneDayAndLateOldRunCloseNeverRefundOrMintCurrentPeriod) {
    Fixture fixture;
    const auto task = fixture.add();
    const auto old = fixture.store.traffic->openRun(task.id, task.queueId, day + 1);
    fixture.store.traffic->reserve(old.id, 0, 100, day + 2);
    EXPECT_EQ(fixture.store.traffic->currentPeriod(2 * day + 5), 2 * day);
    const auto mismatch = fixture.store.traffic->reserve(old.id, 30, 100, day + 3);
    EXPECT_EQ(mismatch.period, 2 * day);
    EXPECT_EQ(mismatch.reserved, 100);
    const auto current = fixture.store.traffic->openRun(task.id, task.queueId, day + 10);
    EXPECT_EQ(current.period, 2 * day);
    EXPECT_EQ(fixture.store.traffic->reserve(current.id, 0, 100, day + 10).reserved, 100);
    fixture.store.traffic->closeRun(old.id, 40);
    EXPECT_EQ(fixture.store.traffic->usage(task.queueId, day).charged, 100);
    fixture.store.traffic->closeRun(current.id, 75);
    EXPECT_EQ(fixture.store.traffic->usage(task.queueId, day).charged, 75);
    auto observer = maskpilot::makeSqliteStore(fixture.temporary.path / "tasks.db");
    EXPECT_EQ(observer.traffic->currentPeriod(0), 2 * day);
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open_v2((fixture.temporary.path / "tasks.db").string().c_str(), &raw,
                              SQLITE_OPEN_READONLY, nullptr),
              SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> close(raw, sqlite3_close);
    sqlite3_stmt* q = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw, "SELECT charged,observed FROM queue_traffic_usage WHERE period=86400",
                                 -1, &q, nullptr),
              SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> finalize(q, sqlite3_finalize);
    ASSERT_EQ(sqlite3_step(q), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(q, 0), 40);
    EXPECT_EQ(sqlite3_column_int64(q, 1), 40);
}

TEST(TrafficStorage, MoveDeleteAndBudgetEditsRetainHistoryAndRunBinding) {
    Fixture fixture;
    auto task = fixture.add();
    const auto run = fixture.store.traffic->openRun(task.id, task.queueId, day);
    fixture.store.traffic->reserve(run.id, 0, 90, day);
    maskpilot::DownloadQueue other;
    other.id = maskpilot::newTaskId();
    other.name = "Other";
    other.dailyPayloadBudget = 50;
    fixture.store.queues->saveQueue(other, 0);
    fixture.store.queues->moveTasks({{task.id, task.queueId}}, other.id, 13);
    EXPECT_THROW(fixture.store.traffic->openRun(task.id, task.queueId, day), std::exception);
    fixture.store.tasks->erase(task.id);
    fixture.store.traffic->closeRun(run.id, 60);
    EXPECT_EQ(fixture.store.traffic->usage(maskpilot::defaultQueueId, day).charged, 60);
    auto item = fixture.add(other.id);
    const auto otherRun = fixture.store.traffic->openRun(item.id, other.id, day);
    fixture.store.traffic->reserve(otherRun.id, 0, 50, day);
    fixture.store.tasks->erase(item.id);
    fixture.store.queues->eraseQueue(other.id, 1);
    fixture.store.traffic->closeRun(otherRun.id, 35);
    EXPECT_EQ(fixture.store.traffic->usage(other.id, day).charged, 35);
    EXPECT_EQ(fixture.store.traffic->usage(maskpilot::defaultQueueId, day).charged, 60);
    auto policy = fixture.store.queues->loadQueues()[0];
    const auto revision = policy.revision;
    ++policy.revision;
    policy.dailyPayloadBudget = 0;
    fixture.store.queues->saveQueue(policy, revision);
    const auto freshTask = fixture.add();
    EXPECT_THROW(fixture.store.traffic->openRun(freshTask.id, freshTask.queueId, day), std::exception);
    ++policy.revision;
    policy.dailyPayloadBudget = 100;
    fixture.store.queues->saveQueue(policy, revision + 1);
    const auto fresh = fixture.store.traffic->openRun(freshTask.id, freshTask.queueId, day);
    EXPECT_EQ(fixture.store.traffic->reserve(fresh.id, 0, 100, day).reserved, 40);
    fixture.store.traffic->closeRun(fresh.id, 40);
    EXPECT_EQ(fixture.store.traffic->usage(maskpilot::defaultQueueId, day).charged, 100);
    EXPECT_THROW(fixture.store.traffic->openRun(maskpilot::newTaskId(), maskpilot::defaultQueueId, day),
                 std::exception);
    EXPECT_THROW(fixture.store.traffic->closeRun(maskpilot::newTaskId(), 0), std::exception);
    EXPECT_THROW(fixture.store.traffic->currentPeriod(-1), std::exception);
}
