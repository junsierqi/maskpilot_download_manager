#include "../../src/application/src/browser_recovery_registry.h"
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>

using namespace maskpilot;
namespace {
Task pausedTask() {
    Task task;
    task.id = newTaskId();
    task.spec.url = "https://example.test/expired";
    task.spec.destination = std::filesystem::temp_directory_path() / "recovery-test.bin";
    task.spec.referrer = "https://example.test/page";
    task.state = TaskState::Paused;
    task.runIntent = RunIntent::Paused;
    return task;
}
BrowserRecoveryMaterial fresh() {
    return {"https://example.test/fresh", "session=fresh-only", "https://example.test/page", "fixture-UA"};
}
} // namespace
TEST(BrowserRecovery, PreservesTrustedFieldsAndAcknowledgesOnlyCommittedIdenticalRetry) {
    BrowserRecoveryRegistry registry;
    auto task = pausedTask();
    task.queueId = newTaskId();
    task.spec.proxy = "http://127.0.0.1:9090";
    task.spec.proxyUsername = "proxy-user";
    task.spec.proxyPassword = "proxy-secret";
    task.spec.proxyAuthenticationRequired = true;
    task.proxyCredentialRef = "saved-native-reference";
    task.spec.expectedSha256 = std::string(64, 'a');
    task.spec.maxStoredBytes = 8192;
    task.spec.sameOriginRedirectOnly = true;
    task.spec.authorization = "Bearer old-source-secret";
    task.spec.headers = {{"X-Old-Secret", "must-not-follow-new-address"}};
    task.originAuthenticationRequired = true;
    task.downloaded = 1024;
    task.resumable = true;
    const auto session = registry.begin(task, 1000);
    EXPECT_EQ(session.pageUrl, task.spec.referrer);
    EXPECT_EQ(session.expiresAt, 1600);
    int commits = 0;
    auto commit = [&](Task& candidate) {
        ++commits;
        EXPECT_EQ(task.spec.url, "https://example.test/expired");
        registry.invalidate(task.id); // Central successful save invalidates outstanding sessions.
        task = candidate;
    };
    registry.apply(session.token, task, fresh(), 1001, commit);
    EXPECT_EQ(commits, 1);
    EXPECT_EQ(task.id, session.taskId);
    EXPECT_EQ(task.spec.destination.filename(), "recovery-test.bin");
    EXPECT_EQ(task.queueId.size(), 32u);
    EXPECT_EQ(task.spec.proxy, "http://127.0.0.1:9090");
    EXPECT_EQ(task.spec.proxyPassword, "proxy-secret");
    EXPECT_EQ(task.proxyCredentialRef, "saved-native-reference");
    EXPECT_EQ(task.spec.expectedSha256, std::string(64, 'a'));
    EXPECT_EQ(task.spec.maxStoredBytes, 8192);
    EXPECT_TRUE(task.spec.sameOriginRedirectOnly);
    EXPECT_TRUE(task.spec.authorization.empty());
    EXPECT_TRUE(task.spec.headers.empty());
    EXPECT_EQ(task.spec.cookie, "session=fresh-only");
    EXPECT_TRUE(task.originAuthenticationRequired);
    EXPECT_EQ(task.downloaded, 0);
    EXPECT_FALSE(task.resumable);
    EXPECT_EQ(task.state, TaskState::Paused);
    registry.apply(session.token, task, fresh(), 1002, commit);
    EXPECT_EQ(commits, 1);
    auto changed = fresh();
    changed.cookie = "different";
    EXPECT_THROW(registry.apply(session.token, task, changed, 1002, commit), std::invalid_argument);
    registry.invalidate(task.id);
    EXPECT_THROW(registry.apply(session.token, task, fresh(), 1002, commit), std::invalid_argument);
}
TEST(BrowserRecovery, PersistenceFailureKeepsOriginalAndAllowsRetryWithoutCredentialDowngrade) {
    BrowserRecoveryRegistry registry;
    auto task = pausedTask();
    task.originAuthenticationRequired = true;
    const auto session = registry.begin(task, 1000);
    auto anonymous = fresh();
    anonymous.cookie.clear();
    int commits = 0;
    const auto commit = [&](Task& candidate) {
        ++commits;
        task = candidate;
    };
    EXPECT_THROW(registry.apply(session.token, task, anonymous, 1001, commit), std::invalid_argument);
    EXPECT_EQ(commits, 0);
    EXPECT_THROW(registry.apply(session.token, task, fresh(), 1001,
                                [](Task&) { throw std::runtime_error("owned fixture persistence failure"); }),
                 std::runtime_error);
    EXPECT_EQ(task.spec.url, "https://example.test/expired");
    registry.apply(session.token, task, fresh(), 1001, commit);
    EXPECT_EQ(commits, 1);
}
TEST(BrowserRecovery, RejectsCrossOwnerProjectProtocolTerminalActiveAndManualIntentViolations) {
    BrowserRecoveryRegistry registry;
    for (int variant = 0; variant < 8; ++variant) {
        auto task = pausedTask();
        switch (variant) {
        case 0:
            task.ownerId = newTaskId();
            break;
        case 1:
            task.projectId = newTaskId();
            break;
        case 2:
            task.spec.kind = TransferKind::Ftp;
            task.spec.url = "ftp://example.test/file";
            break;
        case 3:
            task.spec.kind = TransferKind::Media;
            break;
        case 4:
            task.state = TaskState::Completed;
            break;
        case 5:
            task.state = TaskState::Cancelled;
            break;
        case 6:
            task.state = TaskState::Downloading;
            break;
        case 7:
            task.runIntent = RunIntent::Run;
            break;
        }
        EXPECT_THROW(registry.begin(task, 1000), std::invalid_argument) << variant;
    }
    auto task = pausedTask();
    auto session = registry.begin(task, 1000);
    auto other = pausedTask();
    EXPECT_THROW(registry.apply(session.token, other, fresh(), 1001, [](Task&) {}), std::invalid_argument);
    for (const auto& url :
         {"file:///C:/Windows/win.ini", "ftp://example.test/file", "http://u:p@example.test/file"}) {
        auto material = fresh();
        material.url = url;
        EXPECT_THROW(registry.apply(session.token, task, material, 1001, [](Task&) {}),
                     std::invalid_argument);
    }
    auto material = fresh();
    material.cookie = "cookie\x01invalid";
    EXPECT_THROW(registry.apply(session.token, task, material, 1001, [](Task&) {}), std::invalid_argument);
}
TEST(BrowserRecovery, CapacityExpiryRollbackCancellationAndReplacingSessionAreBounded) {
    BrowserRecoveryRegistry registry;
    auto task = pausedTask();
    const auto first = registry.begin(task, 1000);
    const auto replacement = registry.begin(task, 1001);
    EXPECT_THROW(registry.taskId(first.token, 1001), std::invalid_argument);
    EXPECT_EQ(registry.taskId(replacement.token, 1600), task.id);
    EXPECT_THROW(registry.taskId(replacement.token, 1601), std::invalid_argument);
    const auto rollback = registry.begin(task, 2000);
    EXPECT_THROW(registry.taskId(rollback.token, 1999), std::invalid_argument);
    EXPECT_THROW(registry.taskId(rollback.token, 2001), std::invalid_argument);
    std::vector<std::string> tokens;
    for (int i = 0; i < 64; ++i)
        tokens.push_back(registry.begin(pausedTask(), 3000).token);
    EXPECT_THROW(registry.begin(pausedTask(), 3000), std::invalid_argument);
    registry.cancel(tokens.front());
    registry.cancel(tokens.front());
    EXPECT_NO_THROW(registry.begin(pausedTask(), 3000));
    EXPECT_NO_THROW(registry.begin(pausedTask(), 3600));
}
TEST(BrowserRecovery, ActualServiceMutationAndQueueMoveInvalidatePendingCapability) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    auto store = makeSqliteStore(root / "tasks.db");
    DownloadService service(
        std::move(store.tasks), [] { return makeCurlTransfer(); }, root / "parts", {},
        QueueDependencies{std::move(store.queues), makeQueueCivilTime(), {}, store.traffic});
    auto spec = pausedTask().spec;
    spec.startPaused = true;
    spec.destination = root / "payload.bin";
    const auto id = service.add(spec).get();
    auto session = service.beginBrowserRecovery(id).get();
    service.setPriority(id, 4);
    EXPECT_THROW(service.applyBrowserRecovery(session.token, fresh()).get(), std::invalid_argument);
    QueueConfiguration configuration;
    configuration.name = "Recovery queue";
    const auto queue = service.configureQueue(configuration).get();
    session = service.beginBrowserRecovery(id).get();
    service.moveToQueue({id}, queue.id).get();
    EXPECT_THROW(service.applyBrowserRecovery(session.token, fresh()).get(), std::invalid_argument);
    session = service.beginBrowserRecovery(id).get();
    EXPECT_EQ(service.applyBrowserRecovery(session.token, fresh()).get(), id);
    const auto tasks = service.snapshot();
    ASSERT_EQ(tasks.size(), 1u);
    EXPECT_EQ(tasks.front().queueId, queue.id);
    EXPECT_EQ(tasks.front().spec.destination, spec.destination);
    EXPECT_EQ(tasks.front().state, TaskState::Paused);
    service.pause(id);
    EXPECT_THROW(service.applyBrowserRecovery(session.token, fresh()).get(), std::invalid_argument);
}
