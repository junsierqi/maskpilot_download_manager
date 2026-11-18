#include "../../src/application/src/queue_registry.h"
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <mutex>
#include <set>
#include <thread>
using namespace maskpilot;
namespace {
struct State {
    std::mutex mutex;
    std::vector<Task> saved;
    std::vector<DownloadQueue> queues{DownloadQueue{}};
    std::vector<std::string> starts;
    std::map<std::string, std::int64_t> rates;
    std::set<std::string> release, failFirst;
    bool rejectMove = false, consumePayload = false;
    int factories = 0;
    std::map<std::string, std::size_t> accepted;
    std::size_t saves = 0;
};
struct Tasks final : ITaskRepository {
    std::shared_ptr<State> s;
    explicit Tasks(std::shared_ptr<State> state) : s(std::move(state)) {}
    std::vector<Task> load() override {
        std::lock_guard lock(s->mutex);
        return s->saved;
    }
    void save(const Task& t) override {
        std::lock_guard lock(s->mutex);
        ++s->saves;
        auto it =
            std::find_if(s->saved.begin(), s->saved.end(), [&](const auto& old) { return old.id == t.id; });
        if (it == s->saved.end())
            s->saved.push_back(t);
        else
            *it = t;
    }
    void erase(const std::string& id) override {
        std::lock_guard lock(s->mutex);
        std::erase_if(s->saved, [&](const auto& t) { return t.id == id; });
    }
};
struct Queues final : IQueueRepository {
    std::shared_ptr<State> s;
    explicit Queues(std::shared_ptr<State> state) : s(std::move(state)) {}
    std::vector<DownloadQueue> loadQueues() override {
        std::lock_guard lock(s->mutex);
        return s->queues;
    }
    void saveQueue(const DownloadQueue& q, std::int64_t revision) override {
        std::lock_guard lock(s->mutex);
        auto it =
            std::find_if(s->queues.begin(), s->queues.end(), [&](const auto& old) { return old.id == q.id; });
        if (it == s->queues.end()) {
            if (revision)
                throw std::invalid_argument("stale");
            s->queues.push_back(q);
        } else {
            if (it->revision != revision)
                throw std::invalid_argument("stale");
            *it = q;
        }
    }
    void eraseQueue(const std::string& id, std::int64_t revision) override {
        std::lock_guard lock(s->mutex);
        auto it = std::find_if(s->queues.begin(), s->queues.end(),
                               [&](const auto& q) { return q.id == id && q.revision == revision; });
        if (it == s->queues.end())
            throw std::invalid_argument("stale");
        s->queues.erase(it);
    }
    void moveTasks(const std::vector<QueueMove>& moves, const std::string& target,
                   std::int64_t now) override {
        std::lock_guard lock(s->mutex);
        if (s->rejectMove)
            throw std::invalid_argument("conditional move rejected");
        for (auto& m : moves) {
            auto it = std::find_if(s->saved.begin(), s->saved.end(), [&](const auto& t) {
                return t.id == m.taskId && t.queueId == m.expectedQueueId;
            });
            if (it == s->saved.end())
                throw std::invalid_argument("stale");
        }
        for (auto& m : moves)
            for (auto& t : s->saved)
                if (t.id == m.taskId) {
                    t.queueId = target;
                    t.updatedAt = now;
                }
    }
};
struct Worker final : ITransferTransport {
    std::shared_ptr<State> s;
    explicit Worker(std::shared_ptr<State> state) : s(std::move(state)) {}
    void run(const Task& t, const std::filesystem::path&, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        {
            std::lock_guard lock(s->mutex);
            s->starts.push_back(t.id);
            if (s->failFirst.erase(t.id)) {
                TransferEvent failed;
                failed.outcome = TransferOutcome::Failed;
                failed.retryable = true;
                failed.retryAfter = 60;
                callback(failed);
                return;
            }
        }
        TransferEvent progress;
        progress.phase =
            t.spec.kind == TransferKind::BitTorrent ? TaskState::Seeding : TaskState::Downloading;
        callback(progress);
        bool complete = false;
        while (!control.pause && !control.cancel) {
            bool consume;
            {
                std::lock_guard lock(s->mutex);
                consume = s->consumePayload;
            }
            if (consume && control.payloadBudget) {
                const auto bytes = control.payloadBudget->accept(1024);
                {
                    std::lock_guard lock(s->mutex);
                    s->accepted[t.id] += bytes;
                }
                if (bytes < 1024)
                    control.pause = true;
            }
            {
                std::lock_guard lock(s->mutex);
                s->rates[t.id] = control.speedLimit.load();
                complete = s->release.contains(t.id);
            }
            if (complete)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        TransferEvent terminal;
        terminal.outcome = complete ? TransferOutcome::Complete
                                    : (control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused);
        callback(terminal);
    }
};
bool waitFor(const std::function<bool()>& predicate) {
    for (int i = 0; i < 400; ++i) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
Task find(DownloadService& service, const std::string& id) {
    for (auto& t : service.snapshot())
        if (t.id == id)
            return t;
    return {};
}
QueueConfiguration config(const DownloadQueue& q) {
    return {q.id,          q.name,     q.revision,          q.enabled, q.maxActive, q.downloadLimit,
            q.uploadLimit, q.schedule, q.dailyPayloadBudget};
}
struct FakeCivilTime final : IQueueCivilTime {
    std::atomic<std::int64_t> utc{600};
    std::int64_t utcNow() const override { return utc.load(); }
    std::optional<CivilMinute> at(const std::string& zone, std::int64_t instant) const override {
        if (zone != "UTC" && zone != "Test/Zone")
            return std::nullopt;
        auto minutes = instant / 60;
        auto day = minutes / 1440;
        auto minute = minutes % 1440;
        if (minute < 0) {
            minute += 1440;
            --day;
        }
        return CivilMinute{static_cast<int>((day % 7 + 7) % 7), static_cast<int>(minute)};
    }
};
struct Fixture {
    QTemporaryDir temporary;
    std::shared_ptr<State> s = std::make_shared<State>();
    std::shared_ptr<IQueueCivilTime> clock;
    std::shared_ptr<IQueueTrafficRepository> traffic;
    explicit Fixture(std::shared_ptr<IQueueCivilTime> time = {},
                     std::shared_ptr<IQueueTrafficRepository> ledger = {})
        : clock(std::move(time)), traffic(std::move(ledger)) {}
    DownloadService service{std::make_unique<Tasks>(s),
                            [state = s] {
                                {
                                    std::lock_guard lock(state->mutex);
                                    ++state->factories;
                                }
                                return std::make_unique<Worker>(state);
                            },
                            std::filesystem::path(temporary.path().toStdWString()) / L"parts",
                            {},
                            QueueDependencies{std::make_unique<Queues>(s), clock, {}, traffic}};
    DownloadQueue queue(std::string name, int activeSlots = 1) {
        QueueConfiguration c;
        c.name = std::move(name);
        c.maxActive = activeSlots;
        return service.configureQueue(c).get();
    }
    std::string add(const std::string& queue, bool paused = false, TransferKind kind = TransferKind::Http) {
        DownloadSpec spec;
        spec.url =
            kind == TransferKind::BitTorrent
                ? "magnet:?xt=urn:btih:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                : (kind == TransferKind::Media ? "https://example.test/watch" : "https://example.test/file");
        spec.destination = std::filesystem::path(temporary.path().toStdWString()) / (newTaskId() + ".bin");
        spec.startPaused = paused;
        spec.kind = kind;
        return service.add(spec, {}, false, queue).get();
    }
};
} // namespace
TEST(QueueRates, HierarchicalCapsRedistributeUnusedShares) {
    DownloadQueue a, b;
    a.id = "a";
    a.downloadLimit = 4000;
    b.id = "b";
    auto rates = allocateQueueRates(10000, {a, b}, {{"one", "a", 1000}, {"two", "a", 0}, {"three", "b", 0}});
    EXPECT_EQ(rates.at("one"), 1000);
    EXPECT_EQ(rates.at("two"), 3000);
    EXPECT_EQ(rates.at("three"), 6000);
}
TEST(QueueRates, TinyFiniteAllocationNeverBecomesUnlimited) {
    DownloadQueue q;
    EXPECT_THROW(allocateQueueRates(1, {q}, {{"a", "default"}, {"b", "default"}}), std::invalid_argument);
    auto rates = allocateQueueRates(3, {q}, {{"a", "default"}, {"b", "default"}});
    EXPECT_EQ(rates.at("a") + rates.at("b"), 3);
    EXPECT_GT(rates.at("a"), 0);
    EXPECT_GT(rates.at("b"), 0);
    EXPECT_EQ(allocateQueueRates(0, {q}, {{"a", "default"}}).at("a"), 0);
}
TEST(QueueRates, MediaReservationsRemainFixedAndCannotBeRetroactivelyReduced) {
    DownloadQueue q;
    auto rates = allocateQueueRates(3000, {q}, {{"media", "default", 0, 2000}, {"native", "default", 0, -1}});
    EXPECT_EQ(rates.at("media"), 2000);
    EXPECT_EQ(rates.at("native"), 1000);
    EXPECT_THROW(allocateQueueRates(2000, {q}, {{"media", "default", 0, 2000}, {"native", "default", 0, -1}}),
                 std::invalid_argument);
    EXPECT_THROW(allocateQueueRates(1000, {q}, {{"media", "default", 0, 0}}), std::invalid_argument);
}
TEST(QueueRates, UploadUsesIndependentQueueCap) {
    DownloadQueue q;
    q.downloadLimit = 8000;
    q.uploadLimit = 2000;
    auto rates = allocateQueueRates(3000, {q}, {{"a", "default"}, {"b", "default", 500}}, true);
    EXPECT_EQ(rates.at("a"), 1500);
    EXPECT_EQ(rates.at("b"), 500);
}
TEST(QueuePolicy, MissingRepositoryIsDefaultOnlyAndReadOnly) {
    QueueRegistry registry({});
    EXPECT_EQ(registry.list().size(), 1);
    QueueConfiguration c;
    c.name = "new";
    EXPECT_THROW(registry.prepare(c), std::invalid_argument);
    EXPECT_THROW(registry.get("missing"), std::invalid_argument);
}
TEST(QueuePolicy, DisablePreservesRunAndEnableDoesNotResumeManualPause) {
    Fixture f;
    auto q = f.queue("Work", 2);
    auto running = f.add(q.id);
    auto manual = f.add(q.id, true);
    ASSERT_TRUE(waitFor([&] { return find(f.service, running).state == TaskState::Downloading; }));
    auto c = config(q);
    c.enabled = false;
    q = f.service.configureQueue(c).get();
    ASSERT_TRUE(waitFor([&] { return find(f.service, running).state == TaskState::PolicyWaiting; }));
    EXPECT_EQ(find(f.service, running).runIntent, RunIntent::Run);
    EXPECT_EQ(find(f.service, manual).runIntent, RunIntent::Paused);
    c = config(q);
    c.enabled = true;
    f.service.configureQueue(c).get();
    ASSERT_TRUE(waitFor([&] { return find(f.service, running).state == TaskState::Downloading; }));
    EXPECT_EQ(find(f.service, manual).state, TaskState::Paused);
}
TEST(QueuePolicy, SlotsAndInactiveMovePreserveTaskAuthority) {
    Fixture f;
    f.service.configureLimits(3, 0).get();
    auto q = f.queue("Limited");
    auto one = f.add(q.id);
    auto two = f.add(q.id);
    ASSERT_TRUE(waitFor([&] { return find(f.service, one).state == TaskState::Downloading; }));
    EXPECT_EQ(find(f.service, two).state, TaskState::Queued);
    EXPECT_THROW(f.service.moveToQueue({one, two}, defaultQueueId).get(), std::invalid_argument);
    EXPECT_EQ(find(f.service, two).queueId, q.id);
    f.service.pause(one);
    ASSERT_TRUE(waitFor([&] { return find(f.service, one).state == TaskState::Paused; }));
    auto before = find(f.service, one);
    f.service.moveToQueue({one}, defaultQueueId).get();
    auto after = find(f.service, one);
    EXPECT_EQ(after.queueId, defaultQueueId);
    EXPECT_EQ(after.ownerId, before.ownerId);
    EXPECT_EQ(after.spec.url, before.spec.url);
    EXPECT_EQ(after.spec.destination, before.spec.destination);
    EXPECT_EQ(after.runIntent, RunIntent::Paused);
}
TEST(QueuePolicy, RejectedTransactionLeavesEveryMembershipUnchangedAndActorUsable) {
    Fixture f;
    auto q = f.queue("Target");
    auto a = f.add(defaultQueueId, true), b = f.add(defaultQueueId, true);
    {
        std::lock_guard lock(f.s->mutex);
        f.s->rejectMove = true;
    }
    EXPECT_THROW(f.service.moveToQueue({a, b}, q.id).get(), std::invalid_argument);
    EXPECT_EQ(find(f.service, a).queueId, defaultQueueId);
    EXPECT_EQ(find(f.service, b).queueId, defaultQueueId);
    EXPECT_NO_THROW(f.service.configureLimits(2, 1024).get());
    EXPECT_TRUE(f.service.lastError().empty());
}
TEST(QueuePolicy, StaleConfigurationAndNonemptyRemovalAreOrdinaryErrors) {
    Fixture f;
    auto q = f.queue("Target");
    auto c = config(q);
    c.name = "Renamed";
    f.service.configureQueue(c).get();
    EXPECT_THROW(f.service.configureQueue(c).get(), std::invalid_argument);
    auto task = f.add(q.id, true);
    EXPECT_THROW(f.service.removeQueue(q.id, q.revision + 1).get(), std::invalid_argument);
    EXPECT_TRUE(f.service.lastError().empty());
    EXPECT_EQ(find(f.service, task).queueId, q.id);
}
TEST(QueuePolicy, NativeAllocationsRedistributeAndRejectExhaustion) {
    Fixture f;
    auto q = f.queue("Two", 2);
    f.service.configureLimits(2, 2).get();
    auto a = f.add(q.id), b = f.add(q.id);
    ASSERT_TRUE(waitFor([&] {
        std::lock_guard lock(f.s->mutex);
        return f.s->rates[a] == 1 && f.s->rates[b] == 1;
    }));
    EXPECT_THROW(f.service.configureLimits(2, 1).get(), std::invalid_argument);
    f.service.pause(b);
    ASSERT_TRUE(waitFor([&] { return find(f.service, b).state == TaskState::Paused; }));
    ASSERT_TRUE(waitFor([&] {
        std::lock_guard lock(f.s->mutex);
        return f.s->rates[a] == 2;
    }));
    EXPECT_TRUE(f.service.lastError().empty());
}

TEST(QueuePolicy, SeedingConsumesAnActiveQueueSlot) {
    Fixture f;
    auto q = f.queue("Seeds");
    auto seed = f.add(q.id, false, TransferKind::BitTorrent);
    auto pending = f.add(q.id);
    ASSERT_TRUE(waitFor([&] { return find(f.service, seed).state == TaskState::Seeding; }));
    EXPECT_EQ(find(f.service, pending).state, TaskState::Queued);
    f.service.pause(seed);
    ASSERT_TRUE(waitFor([&] { return find(f.service, pending).state == TaskState::Downloading; }));
}
TEST(QueuePolicy, RotationUsesQueueTurnsAndPriorityWithinEachQueue) {
    Fixture f;
    f.service.configureLimits(1, 0).get();
    auto blocker = f.add(defaultQueueId);
    ASSERT_TRUE(waitFor([&] { return find(f.service, blocker).state == TaskState::Downloading; }));
    auto qa = f.queue("A"), qb = f.queue("B");
    // The default queue ran last. Opaque IDs can sort on either side of "default";
    // select the next queue in cyclic order rather than assuming both sort before it.
    const auto turn = [](const DownloadQueue& q) { return std::pair{q.id <= defaultQueueId, q.id}; };
    if (turn(qa) > turn(qb))
        std::swap(qa, qb);
    auto low = f.add(qa.id, true), high = f.add(qa.id, true), other = f.add(qb.id, true);
    f.service.setPriority(high, 10);
    f.service.resume(low);
    f.service.resume(high);
    f.service.resume(other);
    // An actor request orders all preceding controls before releasing the occupied slot.
    f.service.queues().get();
    {
        std::lock_guard lock(f.s->mutex);
        f.s->release.insert(blocker);
    }
    ASSERT_TRUE(waitFor([&] { return find(f.service, high).state == TaskState::Downloading; }));
    EXPECT_EQ(find(f.service, other).state, TaskState::Queued);
    {
        std::lock_guard lock(f.s->mutex);
        f.s->release.insert(high);
    }
    ASSERT_TRUE(waitFor([&] { return find(f.service, other).state == TaskState::Downloading; }));
    EXPECT_EQ(find(f.service, low).state, TaskState::Queued);
    {
        std::lock_guard lock(f.s->mutex);
        f.s->release.insert(other);
    }
    ASSERT_TRUE(waitFor([&] { return find(f.service, low).state == TaskState::Downloading; }));
}
TEST(QueuePolicy, MediaRateIsARealLaunchReservationAndReductionsRequirePause) {
    Fixture f;
    auto q = f.queue("Media", 2);
    f.service.configureLimits(2, 4096).get();
    auto a = f.add(q.id, false, TransferKind::Media), b = f.add(q.id, false, TransferKind::Media);
    ASSERT_TRUE(waitFor([&] {
        std::lock_guard lock(f.s->mutex);
        return f.s->rates[a] == 2048 && f.s->rates[b] == 2048;
    }));
    EXPECT_THROW(f.service.configureLimits(2, 3072).get(), std::invalid_argument);
    auto edit = config(q);
    edit.downloadLimit = 3072;
    EXPECT_THROW(f.service.configureQueue(edit).get(), std::invalid_argument);
    f.service.pause(b);
    ASSERT_TRUE(waitFor([&] { return find(f.service, b).state == TaskState::Paused; }));
    EXPECT_NO_THROW(f.service.configureLimits(2, 3072).get());
    {
        std::lock_guard lock(f.s->mutex);
        EXPECT_EQ(f.s->rates[a], 2048);
    }
    EXPECT_TRUE(f.service.lastError().empty());
}
TEST(QueuePolicy, DisabledScheduledTasksRetainTheirAbsoluteGate) {
    Fixture f;
    auto q = f.queue("Later");
    auto id = f.add(q.id, true);
    auto at = unixTime() + 3600;
    f.service.schedule(id, at, at + 3600);
    f.service.queues().get();
    auto edit = config(q);
    edit.enabled = false;
    q = f.service.configureQueue(edit).get();
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::PolicyWaiting; }));
    EXPECT_EQ(find(f.service, id).spec.scheduledAt, at);
    edit = config(q);
    edit.enabled = true;
    f.service.configureQueue(edit).get();
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::Scheduled; }));
    EXPECT_EQ(find(f.service, id).runIntent, RunIntent::Run);
}

TEST(QueuePolicy, QueueTogglePreservesRetryBackoff) {
    Fixture f;
    auto q = f.queue("Retry");
    auto id = f.add(q.id, true);
    {
        std::lock_guard lock(f.s->mutex);
        f.s->failFirst.insert(id);
    }
    f.service.resume(id);
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::RetryWaiting; }));
    const auto retry = find(f.service, id).retryAt;
    auto edit = config(q);
    edit.enabled = false;
    q = f.service.configureQueue(edit).get();
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::PolicyWaiting; }));
    edit = config(q);
    edit.enabled = true;
    f.service.configureQueue(edit).get();
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::RetryWaiting; }));
    EXPECT_EQ(find(f.service, id).retryAt, retry);
    EXPECT_EQ(find(f.service, id).runIntent, RunIntent::Run);
}

TEST(QueuePolicy, BandwidthWaitDoesNotPersistUnchangedStateOnEveryTick) {
    Fixture f;
    f.service.configureLimits(3, 1).get();
    const auto running = f.add(defaultQueueId);
    ASSERT_TRUE(waitFor([&] { return find(f.service, running).state == TaskState::Downloading; }));
    std::vector<std::string> waiting;
    for (int n = 0; n < 4; ++n)
        waiting.push_back(f.add(defaultQueueId));
    ASSERT_TRUE(waitFor([&] {
        return std::all_of(waiting.begin(), waiting.end(), [&](const auto& id) {
            return find(f.service, id).state == TaskState::PolicyWaiting;
        });
    }));
    std::size_t before;
    {
        std::lock_guard lock(f.s->mutex);
        before = f.s->saves;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    f.service.queues().get(); // Actor barrier after several unchanged scheduling ticks.
    {
        std::lock_guard lock(f.s->mutex);
        EXPECT_EQ(f.s->saves, before);
    }
    f.service.pause(running);
    ASSERT_TRUE(waitFor([&] { return find(f.service, running).state == TaskState::Paused; }));
    EXPECT_TRUE(waitFor([&] {
        return std::any_of(waiting.begin(), waiting.end(), [&](const auto& id) {
            return find(f.service, id).state == TaskState::Downloading;
        });
    }));
}

TEST(QueuePolicy, RecoveryStopsEnabledBandwidthWaitButPreservesExplicitDisabledQueueGate) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    auto state = std::make_shared<State>();
    DownloadQueue disabled;
    disabled.id = newTaskId();
    disabled.name = "Disabled at restart";
    disabled.enabled = false;
    state->queues.push_back(disabled);
    Task enabledTask;
    enabledTask.id = newTaskId();
    enabledTask.spec.url = "https://example.test/recovered-enabled";
    enabledTask.spec.destination = std::filesystem::path(temporary.path().toStdWString()) / L"enabled.bin";
    enabledTask.state = TaskState::PolicyWaiting;
    enabledTask.runIntent = RunIntent::Run;
    auto disabledTask = enabledTask;
    disabledTask.id = newTaskId();
    disabledTask.queueId = disabled.id;
    disabledTask.spec.destination.replace_filename(L"disabled.bin");
    state->saved = {enabledTask, disabledTask};
    DownloadService service(
        std::make_unique<Tasks>(state), [state] { return std::make_unique<Worker>(state); },
        std::filesystem::path(temporary.path().toStdWString()) / L"parts", {},
        QueueDependencies{std::make_unique<Queues>(state)});
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    EXPECT_EQ(find(service, enabledTask.id).state, TaskState::Paused);
    EXPECT_EQ(find(service, enabledTask.id).runIntent, RunIntent::Paused);
    EXPECT_EQ(find(service, disabledTask.id).state, TaskState::PolicyWaiting);
    {
        std::lock_guard lock(state->mutex);
        EXPECT_TRUE(state->starts.empty());
    }
    auto edit = config(disabled);
    edit.enabled = true;
    service.configureQueue(edit).get();
    ASSERT_TRUE(waitFor([&] { return find(service, disabledTask.id).state == TaskState::Downloading; }));
    EXPECT_EQ(find(service, enabledTask.id).state, TaskState::Paused);
}

TEST(QueueWindows, ClosurePreservesRunAndOpeningDoesNotResumeManualPauseOrCompletedTasks) {
    auto clock = std::make_shared<FakeCivilTime>();
    Fixture f(clock);
    auto q = f.queue("Window", 3);
    auto c = config(q);
    c.schedule = {true, 127, 10, 20, "Test/Zone"};
    q = f.service.configureQueue(c).get();
    auto running = f.add(q.id), manual = f.add(q.id, true), complete = f.add(q.id);
    ASSERT_TRUE(waitFor([&] {
        return find(f.service, running).state == TaskState::Downloading &&
               find(f.service, complete).state == TaskState::Downloading;
    }));
    {
        std::lock_guard lock(f.s->mutex);
        f.s->release.insert(complete);
    }
    ASSERT_TRUE(waitFor([&] { return find(f.service, complete).state == TaskState::Completed; }));
    clock->utc = 20 * 60;
    ASSERT_TRUE(waitFor([&] { return find(f.service, running).state == TaskState::PolicyWaiting; }));
    EXPECT_EQ(find(f.service, running).runIntent, RunIntent::Run);
    std::size_t saves;
    {
        std::lock_guard lock(f.s->mutex);
        saves = f.s->saves;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    f.service.queues().get();
    {
        std::lock_guard lock(f.s->mutex);
        EXPECT_EQ(f.s->saves, saves);
    }
    clock->utc = (1440 + 10) * 60;
    ASSERT_TRUE(waitFor([&] { return find(f.service, running).state == TaskState::Downloading; }));
    EXPECT_EQ(find(f.service, manual).state, TaskState::Paused);
    EXPECT_EQ(find(f.service, complete).state, TaskState::Completed);
    f.service.pause(running);
    ASSERT_TRUE(waitFor([&] { return find(f.service, running).state == TaskState::Paused; }));
    clock->utc = (1440 + 20) * 60;
    f.service.queues().get();
    clock->utc = (2880 + 10) * 60;
    f.service.queues().get();
    EXPECT_EQ(find(f.service, running).runIntent, RunIntent::Paused);
}
TEST(QueueWindows, BackwardAndForwardJumpsRespectWindowAndAbsoluteOneShotGates) {
    auto clock = std::make_shared<FakeCivilTime>();
    Fixture f(clock);
    auto q = f.queue("Jumps");
    auto c = config(q);
    c.schedule = {true, 127, 10, 20, "UTC"};
    q = f.service.configureQueue(c).get();
    auto id = f.add(q.id, true);
    f.service.schedule(id, 12 * 60, 14 * 60);
    f.service.queues().get();
    EXPECT_EQ(find(f.service, id).state, TaskState::Scheduled);
    clock->utc = 13 * 60;
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::Downloading; }));
    clock->utc = 9 * 60;
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::PolicyWaiting; }));
    clock->utc = 11 * 60;
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::Scheduled; }));
    clock->utc = 13 * 60;
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::Downloading; }));
    clock->utc = 14 * 60;
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::Paused; }));
    EXPECT_EQ(find(f.service, id).runIntent, RunIntent::Paused);
}
TEST(QueueWindows, RetryDeadlinesUseTheSameClockAsCivilAdmission) {
    auto clock = std::make_shared<FakeCivilTime>();
    Fixture f(clock);
    auto q = f.queue("Retry clock");
    auto id = f.add(q.id, true);
    {
        std::lock_guard lock(f.s->mutex);
        f.s->failFirst.insert(id);
    }
    f.service.resume(id);
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::RetryWaiting; }));
    EXPECT_EQ(find(f.service, id).retryAt, 660);
    clock->utc = 659;
    f.service.queues().get();
    EXPECT_EQ(find(f.service, id).state, TaskState::RetryWaiting);
    clock->utc = 660;
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::Downloading; }));
}
TEST(QueueWindows, DefaultClockIsUtcOnlyAndUnavailableSavedZonesFailClosed) {
    auto state = std::make_shared<State>();
    auto q = DownloadQueue{};
    q.schedule = {true, 127, 0, 0, "Unavailable/Zone"};
    state->queues = {q};
    QueueRegistry registry(QueueDependencies{std::make_unique<Queues>(state)});
    EXPECT_FALSE(registry.available(defaultQueueId, 0));
    auto c = config(q);
    EXPECT_THROW(registry.prepare(c), std::invalid_argument);
    c.schedule.enabled = false;
    EXPECT_NO_THROW(registry.prepare(c));
    state->queues.front().schedule = {true, 8, 0, 1, "UTC"};
    QueueRegistry utc(QueueDependencies{std::make_unique<Queues>(state)});
    EXPECT_TRUE(utc.available(defaultQueueId, 0));
    EXPECT_FALSE(utc.available(defaultQueueId, 60));
}
TEST(QueueWindows, StartupClosedWindowHoldsRunAndFirstDueWorkerUsesInitialLimits) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    auto state = std::make_shared<State>();
    auto clock = std::make_shared<FakeCivilTime>();
    auto scheduled = DownloadQueue{};
    scheduled.schedule = {true, 127, 11, 20, "Test/Zone"};
    state->queues = {scheduled};
    Task task;
    task.id = newTaskId();
    task.spec.url = "https://example.test/window";
    task.spec.destination = std::filesystem::path(temporary.path().toStdWString()) / L"one.bin";
    task.state = TaskState::Downloading;
    task.runIntent = RunIntent::Run;
    auto second = task;
    second.id = newTaskId();
    second.spec.destination.replace_filename(L"two.bin");
    state->saved = {task, second};
    DownloadService service(
        std::make_unique<Tasks>(state), [state] { return std::make_unique<Worker>(state); },
        std::filesystem::path(temporary.path().toStdWString()) / L"parts", {},
        QueueDependencies{std::make_unique<Queues>(state), clock, InitialQueueLimits{1, 7, 0}});
    EXPECT_EQ(find(service, task.id).state, TaskState::PolicyWaiting);
    EXPECT_EQ(find(service, task.id).runIntent, RunIntent::Run);
    {
        std::lock_guard lock(state->mutex);
        EXPECT_TRUE(state->starts.empty());
    }
    clock->utc = 11 * 60;
    ASSERT_TRUE(waitFor([&] {
        std::lock_guard lock(state->mutex);
        return state->starts.size() == 1 && state->rates[state->starts.front()] == 7;
    }));
    {
        std::lock_guard lock(state->mutex);
        EXPECT_EQ(state->starts.size(), 1);
    }
}
TEST(QueueWindows, InvalidInitialLimitsRejectBeforeAnyWorkerCanStart) {
    QTemporaryDir temporary;
    auto state = std::make_shared<State>();
    EXPECT_THROW((DownloadService(
                     std::make_unique<Tasks>(state), [state] { return std::make_unique<Worker>(state); },
                     std::filesystem::path(temporary.path().toStdWString()), {},
                     QueueDependencies{std::make_unique<Queues>(state), {}, InitialQueueLimits{0, 0, 0}})),
                 std::invalid_argument);
    std::lock_guard lock(state->mutex);
    EXPECT_TRUE(state->starts.empty());
}

namespace {
struct BudgetLedger final : IQueueTrafficRepository {
    struct Run {
        std::int64_t period = 0, reserved = 0, observed = 0;
        bool closed = false;
    };
    std::mutex mutex;
    std::map<std::string, Run> runs;
    std::map<std::int64_t, TrafficUsage> periods;
    std::int64_t floor = 0, limit = 10;
    bool fail = false;
    int abandoned = 0;
    std::int64_t period(std::int64_t utc) {
        floor = std::max(floor, utc / 86400 * 86400);
        return floor;
    }
    TrafficRun openRun(const std::string&, const std::string&, std::int64_t utc) override {
        std::lock_guard lock(mutex);
        auto id = newTaskId();
        auto day = period(utc);
        runs[id].period = day;
        return {id, day};
    }
    TrafficGrant reserve(const std::string& id, std::int64_t observed, std::int64_t desired,
                         std::int64_t utc) override {
        std::lock_guard lock(mutex);
        if (fail)
            throw std::runtime_error("ledger fault");
        auto day = period(utc);
        auto& r = runs.at(id);
        if (r.closed)
            throw std::runtime_error("closed");
        if (r.period != day)
            return {r.reserved, day};
        auto& u = periods[day];
        u.observed += std::max<std::int64_t>(0, observed - r.observed);
        r.observed = std::max(r.observed, observed);
        auto extra = std::min(std::max<std::int64_t>(0, desired - r.reserved),
                              std::max<std::int64_t>(0, limit - u.charged));
        r.reserved += extra;
        u.charged += extra;
        return {r.reserved, day};
    }
    void closeRun(const std::string& id, std::int64_t observed) override {
        std::lock_guard lock(mutex);
        auto& r = runs.at(id);
        if (r.closed)
            return;
        auto& u = periods[r.period];
        u.observed += std::max<std::int64_t>(0, observed - r.observed);
        u.charged -= r.reserved - observed;
        r.observed = observed;
        r.closed = true;
    }
    TrafficUsage usage(const std::string&, std::int64_t utc) override {
        std::lock_guard lock(mutex);
        auto day = period(utc);
        auto result = periods[day];
        result.period = day;
        return result;
    }
    std::int64_t currentPeriod(std::int64_t utc) override {
        std::lock_guard lock(mutex);
        return period(utc);
    }
    void abandonOpenRuns() override {
        std::lock_guard lock(mutex);
        ++abandoned;
        for (auto& [id, r] : runs)
            r.closed = true;
    }
};
} // namespace
TEST(QueueTraffic, ExhaustionPreservesRunAndNextDayResumesButManualPauseDominates) {
    auto clock = std::make_shared<FakeCivilTime>();
    auto ledger = std::make_shared<BudgetLedger>();
    Fixture f(clock, ledger);
    auto q = f.queue("Budget");
    auto c = config(q);
    c.dailyPayloadBudget = 10;
    f.service.configureQueue(c).get();
    {
        std::lock_guard lock(f.s->mutex);
        f.s->consumePayload = true;
    }
    auto id = f.add(q.id);
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::PolicyWaiting; }));
    EXPECT_EQ(find(f.service, id).runIntent, RunIntent::Run);
    EXPECT_EQ(f.service.queueTraffic(q.id).get().charged, 10);
    EXPECT_EQ(f.service.queueTraffic(q.id).get().observed, 10);
    clock->utc = 86400 + 600;
    ASSERT_TRUE(waitFor([&] {
        std::lock_guard lock(f.s->mutex);
        return f.s->starts.size() >= 2;
    }));
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::PolicyWaiting; }));
    f.service.pause(id);
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::Paused; }));
    std::size_t starts;
    {
        std::lock_guard lock(f.s->mutex);
        starts = f.s->starts.size();
        EXPECT_EQ(f.s->accepted[id], 20);
    }
    clock->utc = 2 * 86400 + 600;
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    f.service.queues().get();
    {
        std::lock_guard lock(f.s->mutex);
        EXPECT_EQ(f.s->starts.size(), starts);
    }
    EXPECT_EQ(find(f.service, id).runIntent, RunIntent::Paused);
    EXPECT_EQ(ledger->abandoned, 1);
}
TEST(QueueTraffic, StrictBudgetRejectsExternalEnginesBeforeFactoryCreation) {
    auto ledger = std::make_shared<BudgetLedger>();
    Fixture f({}, ledger);
    auto q = f.queue("Native only");
    auto c = config(q);
    c.dailyPayloadBudget = 10;
    f.service.configureQueue(c).get();
    auto media = f.add(q.id, false, TransferKind::Media);
    auto torrent = f.add(q.id, false, TransferKind::BitTorrent);
    EXPECT_EQ(find(f.service, media).state, TaskState::NeedsAttention);
    EXPECT_EQ(find(f.service, torrent).state, TaskState::NeedsAttention);
    DownloadSpec local;
    local.kind = TransferKind::BitTorrent;
    local.url = "torrent:/unread-owned-fixture.torrent";
    local.destination = std::filesystem::path(f.temporary.path().toStdWString()) / L"local-torrent";
    EXPECT_THROW(f.service.add(local, {}, false, q.id).get(), std::invalid_argument);
    EXPECT_EQ(f.service.snapshot().size(), 2);
    std::lock_guard lock(f.s->mutex);
    EXPECT_EQ(f.s->factories, 0);
    EXPECT_TRUE(f.s->starts.empty());
}
TEST(QueueTraffic, BudgetPolicyEditsRequireInactiveJobsAndMissingLedgerIsExplicit) {
    Fixture unavailable;
    auto q = unavailable.queue("No accounting");
    auto c = config(q);
    c.dailyPayloadBudget = 10;
    EXPECT_THROW(unavailable.service.configureQueue(c).get(), std::invalid_argument);
    auto ledger = std::make_shared<BudgetLedger>();
    Fixture f({}, ledger);
    q = f.queue("Accounting");
    c = config(q);
    c.dailyPayloadBudget = 10;
    q = f.service.configureQueue(c).get();
    auto id = f.add(q.id);
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::Downloading; }));
    c = config(q);
    c.dailyPayloadBudget = 0;
    EXPECT_THROW(f.service.configureQueue(c).get(), std::invalid_argument);
    f.service.pause(id);
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::Paused; }));
    EXPECT_NO_THROW(f.service.configureQueue(c).get());
}
TEST(QueueTraffic, AccountingFailureBecomesAttentionAndStopsFurtherAdmission) {
    auto ledger = std::make_shared<BudgetLedger>();
    ledger->fail = true;
    Fixture f({}, ledger);
    auto q = f.queue("Fault");
    auto c = config(q);
    c.dailyPayloadBudget = 10;
    f.service.configureQueue(c).get();
    {
        std::lock_guard lock(f.s->mutex);
        f.s->consumePayload = true;
    }
    auto id = f.add(q.id);
    ASSERT_TRUE(waitFor([&] { return find(f.service, id).state == TaskState::NeedsAttention; }));
    EXPECT_FALSE(f.service.lastError().empty());
    EXPECT_THROW(f.add(defaultQueueId), std::runtime_error);
    EXPECT_EQ(f.service.queueTraffic(q.id).get().charged, 0);
}

TEST(QueueTraffic, RecoveryRetainsBudgetWaitAcrossRestartWithoutStartingBeforeAllowance) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    auto state = std::make_shared<State>();
    auto clock = std::make_shared<FakeCivilTime>();
    auto ledger = std::make_shared<BudgetLedger>();
    state->queues.front().dailyPayloadBudget = 10;
    state->consumePayload = true;
    ledger->periods[0] = {0, 10, 10};
    Task task;
    task.id = newTaskId();
    task.spec.url = "https://example.test/budget-recovery";
    task.spec.destination = std::filesystem::path(temporary.path().toStdWString()) / L"one.bin";
    task.state = TaskState::PolicyWaiting;
    task.runIntent = RunIntent::Run;
    state->saved = {task};
    auto factory = [state] {
        std::lock_guard lock(state->mutex);
        ++state->factories;
        return std::make_unique<Worker>(state);
    };
    {
        DownloadService service(
            std::make_unique<Tasks>(state), factory,
            std::filesystem::path(temporary.path().toStdWString()) / L"parts", {},
            QueueDependencies{std::make_unique<Queues>(state), clock, InitialQueueLimits{1, 7, 0}, ledger});
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        EXPECT_EQ(find(service, task.id).state, TaskState::PolicyWaiting);
        EXPECT_EQ(find(service, task.id).runIntent, RunIntent::Run);
        {
            std::lock_guard lock(state->mutex);
            EXPECT_TRUE(state->starts.empty());
            EXPECT_EQ(state->factories, 0);
        }
        clock->utc = 86400 + 600;
        ASSERT_TRUE(waitFor([&] {
            std::lock_guard lock(state->mutex);
            return state->starts.size() == 1;
        }));
        ASSERT_TRUE(waitFor([&] { return find(service, task.id).state == TaskState::PolicyWaiting; }));
    }
    clock->utc = 2 * 86400 + 600;
    DownloadService reopened(
        std::make_unique<Tasks>(state), factory,
        std::filesystem::path(temporary.path().toStdWString()) / L"parts", {},
        QueueDependencies{std::make_unique<Queues>(state), clock, InitialQueueLimits{1, 7, 0}, ledger});
    ASSERT_TRUE(waitFor([&] {
        std::lock_guard lock(state->mutex);
        return state->starts.size() == 2;
    }));
    ASSERT_TRUE(waitFor([&] { return find(reopened, task.id).state == TaskState::PolicyWaiting; }));
    EXPECT_EQ(find(reopened, task.id).runIntent, RunIntent::Run);
}
