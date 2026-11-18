#include "../../src/application/src/payload_budget.h"
#include <algorithm>
#include <atomic>
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
using namespace maskpilot;
namespace {
struct Ledger final : IQueueTrafficRepository {
    struct Run {
        std::int64_t period = 0, reserved = 0, observed = 0;
        bool closed = false;
    };
    std::mutex mutex;
    std::map<std::string, Run> runs;
    std::map<std::int64_t, TrafficUsage> periods;
    std::int64_t limit = 1000000, floor = 0;
    int reserves = 0, closes = 0;
    bool failReserve = false, failClose = false, failPeriod = false;
    std::int64_t day(std::int64_t utc) {
        auto period = utc / 86400 * 86400;
        if (utc < 0 && utc % 86400)
            period -= 86400;
        floor = std::max(floor, period);
        return floor;
    }
    TrafficRun openRun(const std::string&, const std::string&, std::int64_t utc) override {
        std::lock_guard lock(mutex);
        auto id = std::to_string(runs.size() + 1);
        auto period = day(utc);
        runs[id].period = period;
        return {id, period};
    }
    TrafficGrant reserve(const std::string& id, std::int64_t observed, std::int64_t desired,
                         std::int64_t utc) override {
        std::lock_guard lock(mutex);
        ++reserves;
        if (failReserve)
            throw std::runtime_error("fault");
        if (desired > maxDailyPayloadBudget)
            throw std::runtime_error("oversized reservation");
        auto period = day(utc);
        auto& r = runs.at(id);
        if (r.closed)
            throw std::runtime_error("closed");
        if (r.period != period)
            return {r.reserved, period};
        if (observed > r.reserved)
            throw std::runtime_error("unreserved");
        auto& u = periods[period];
        u.observed += std::max<std::int64_t>(0, observed - r.observed);
        r.observed = std::max(r.observed, observed);
        auto extra = std::min(std::max<std::int64_t>(0, desired - r.reserved),
                              std::max<std::int64_t>(0, limit - u.charged));
        r.reserved += extra;
        u.charged += extra;
        return {r.reserved, period};
    }
    void closeRun(const std::string& id, std::int64_t observed) override {
        std::lock_guard lock(mutex);
        ++closes;
        if (failClose)
            throw std::runtime_error("fault");
        auto& r = runs.at(id);
        if (r.closed)
            return;
        if (observed > r.reserved)
            throw std::runtime_error("unreserved");
        auto& u = periods[r.period];
        u.observed += std::max<std::int64_t>(0, observed - r.observed);
        u.charged -= r.reserved - observed;
        r.observed = observed;
        r.closed = true;
    }
    TrafficUsage usage(const std::string&, std::int64_t utc) override {
        std::lock_guard lock(mutex);
        auto p = day(utc);
        auto u = periods[p];
        u.period = p;
        return u;
    }
    std::int64_t currentPeriod(std::int64_t utc) override {
        std::lock_guard lock(mutex);
        if (failPeriod)
            throw std::runtime_error("fault");
        return day(utc);
    }
    void abandonOpenRuns() override {
        std::lock_guard lock(mutex);
        for (auto& [id, r] : runs)
            r.closed = true;
    }
};
} // namespace
TEST(PayloadBudget, DurablePrechargePrecedesAcceptanceAndCleanFinishRefundsOnlyUnusedCredit) {
    auto ledger = std::make_shared<Ledger>();
    auto budget = makeNativePayloadBudget(ledger, "task", "default", [] { return 0; });
    EXPECT_EQ(budget->accept(7), 7);
    EXPECT_EQ(ledger->usage("default", 0).charged, 65536);
    EXPECT_EQ(ledger->usage("default", 0).observed, 0);
    budget->finish();
    EXPECT_EQ(ledger->usage("default", 0).charged, 7);
    EXPECT_EQ(ledger->usage("default", 0).observed, 7);
    budget->finish();
    EXPECT_EQ(ledger->closes, 1);
    EXPECT_EQ(budget->accept(1), 0);
}
TEST(PayloadBudget, ExactLastCreditIsSharedAcrossConcurrentCallbacks) {
    auto ledger = std::make_shared<Ledger>();
    ledger->limit = 10003;
    auto budget = makeNativePayloadBudget(ledger, "task", "default", [] { return 0; });
    std::atomic<std::size_t> accepted = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i)
        workers.emplace_back([&] {
            for (int n = 0; n < 10; ++n)
                accepted += budget->accept(1000);
        });
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(accepted.load(), 10003);
    EXPECT_TRUE(budget->exhausted());
    EXPECT_FALSE(budget->failed());
    budget->finish();
    EXPECT_EQ(ledger->usage("default", 0).charged, 10003);
    EXPECT_EQ(ledger->usage("default", 0).observed, 10003);
}
TEST(PayloadBudget, DifferentRunsCannotSpendEachOthersReservedCredits) {
    auto ledger = std::make_shared<Ledger>();
    ledger->limit = 100;
    auto first = makeNativePayloadBudget(ledger, "one", "default", [] { return 0; });
    auto second = makeNativePayloadBudget(ledger, "two", "default", [] { return 0; });
    EXPECT_EQ(first->accept(10), 10);
    EXPECT_EQ(second->accept(10), 0);
    first->finish();
    auto third = makeNativePayloadBudget(ledger, "three", "default", [] { return 0; });
    EXPECT_EQ(third->accept(100), 90);
    third->finish();
    second->finish();
    EXPECT_EQ(ledger->usage("default", 0).observed, 100);
}
TEST(PayloadBudget, UnreportedCrashReservationRemainsCharged) {
    auto ledger = std::make_shared<Ledger>();
    ledger->limit = 100;
    {
        auto budget = makeNativePayloadBudget(ledger, "one", "default", [] { return 0; });
        EXPECT_EQ(budget->accept(10), 10);
    }
    ledger->abandonOpenRuns();
    auto after = makeNativePayloadBudget(ledger, "two", "default", [] { return 0; });
    EXPECT_EQ(after->accept(1), 0);
    EXPECT_EQ(ledger->usage("default", 0).charged, 100);
    EXPECT_EQ(ledger->usage("default", 0).observed, 0);
    after->finish();
}
TEST(PayloadBudget, NewUtcDayCannotSpendCachedOldDayCreditsAndRollbackCannotMintAllowance) {
    auto ledger = std::make_shared<Ledger>();
    ledger->limit = 100;
    std::atomic<std::int64_t> utc = 0;
    auto budget = makeNativePayloadBudget(ledger, "one", "default", [&] { return utc.load(); });
    EXPECT_EQ(budget->accept(10), 10);
    utc = 86400;
    EXPECT_EQ(budget->accept(10), 0);
    EXPECT_TRUE(budget->exhausted());
    budget->finish();
    EXPECT_EQ(ledger->periods[0].charged, 10);
    utc = 0;
    auto next = makeNativePayloadBudget(ledger, "two", "default", [&] { return utc.load(); });
    EXPECT_EQ(next->accept(100), 100);
    next->finish();
    EXPECT_EQ(ledger->usage("default", 0).period, 86400);
    EXPECT_EQ(ledger->usage("default", 0).charged, 100);
}
TEST(PayloadBudget, ReservationAndPeriodFailuresAcceptNothingAndCloseFailureIsVisible) {
    auto ledger = std::make_shared<Ledger>();
    ledger->failReserve = true;
    auto budget = makeNativePayloadBudget(ledger, "one", "default", [] { return 0; });
    EXPECT_EQ(budget->accept(1), 0);
    EXPECT_TRUE(budget->failed());
    EXPECT_FALSE(budget->exhausted());
    budget->finish();
    ledger->failReserve = false;
    auto second = makeNativePayloadBudget(ledger, "two", "default", [] { return 0; });
    EXPECT_EQ(second->accept(5), 5);
    ledger->failPeriod = true;
    EXPECT_EQ(second->accept(1), 0);
    EXPECT_TRUE(second->failed());
    ledger->failClose = true;
    second->finish();
    EXPECT_TRUE(second->failed());
    EXPECT_EQ(ledger->usage("default", 0).charged, 65536);
}

TEST(PayloadBudget, MaximumDailyBoundaryExhaustsWithoutAnOversizedReservationRequest) {
    auto ledger = std::make_shared<Ledger>();
    ledger->limit = maxDailyPayloadBudget;
    auto budget = makeNativePayloadBudget(ledger, "task", "default", [] { return 0; });
    EXPECT_EQ(budget->accept(static_cast<std::size_t>(maxDailyPayloadBudget)),
              static_cast<std::size_t>(maxDailyPayloadBudget));
    EXPECT_EQ(budget->accept(1), 0);
    EXPECT_TRUE(budget->exhausted());
    EXPECT_FALSE(budget->failed());
    budget->finish();
    EXPECT_EQ(ledger->usage("default", 0).charged, maxDailyPayloadBudget);
}
