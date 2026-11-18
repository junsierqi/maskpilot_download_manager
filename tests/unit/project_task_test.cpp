#include <algorithm>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <thread>

using namespace maskpilot;
namespace {
struct ProjectTaskTemp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-project-task-" + newTaskId());
    ProjectTaskTemp() { std::filesystem::create_directories(path); }
    ~ProjectTaskTemp() {
        std::error_code error;
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().string().starts_with("maskpilot-project-task-"))
            std::filesystem::remove_all(path, error);
    }
    DownloadSpec spec() const {
        DownloadSpec result;
        result.url = "https://example.test/site";
        result.destination = path / "resource.body";
        result.maxStoredBytes = 4096;
        result.sameOriginRedirectOnly = true;
        return result;
    }
};
bool awaitProjectTask(const std::function<bool()>& condition) {
    for (int n = 0; n < 400; ++n) {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}
class ProjectTaskTransport final : public ITransferTransport {
  public:
    void run(const Task&, const std::filesystem::path&, TransferControl&,
             const std::function<void(TransferEvent)>& callback) override {
        TransferEvent event;
        event.outcome = TransferOutcome::Complete;
        callback(event);
    }
};
} // namespace

TEST(ProjectTask, PausedFirstAndExactParentReplayCannotBeClaimedByPlainOrDifferentRevision) {
    ProjectTaskTemp temporary;
    std::atomic_int factories = 0;
    DownloadService service(
        makeSqliteRepository(temporary.path / "tasks.db"),
        [&] {
            ++factories;
            return std::make_unique<ProjectTaskTransport>();
        },
        temporary.path / "parts");
    const auto project = newTaskId(), request = newTaskId();
    auto spec = temporary.spec();
    const auto id = service.addProjectChild(spec, project, 1, request).get();
    EXPECT_EQ(service.addProjectChild(spec, project, 1, request).get(), id);
    EXPECT_THROW(service.addProjectChild(spec, project, 2, request).get(), std::invalid_argument);
    EXPECT_THROW(service.addProjectChild(spec, newTaskId(), 1, request).get(), std::invalid_argument);
    spec.startPaused = true;
    EXPECT_THROW(service.add(spec, request).get(), std::invalid_argument);
    ASSERT_TRUE(awaitProjectTask([&] { return service.snapshot().size() == 1; }));
    const auto saved = service.snapshot().front();
    EXPECT_EQ(saved.projectId, project);
    EXPECT_EQ(saved.projectRevision, 1);
    EXPECT_EQ(saved.state, TaskState::Paused);
    EXPECT_EQ(saved.runIntent, RunIntent::Paused);
    EXPECT_EQ(factories.load(), 0);
    auto invalid = spec;
    invalid.maxStoredBytes = 0;
    EXPECT_THROW(service.addProjectChild(invalid, project, 1, newTaskId()).get(), std::invalid_argument);
    invalid = spec;
    invalid.sameOriginRedirectOnly = false;
    EXPECT_THROW(service.addProjectChild(invalid, project, 1, newTaskId()).get(), std::invalid_argument);
    invalid = spec;
    invalid.cookie = "owned-secret";
    EXPECT_THROW(service.addProjectChild(invalid, project, 1, newTaskId()).get(), std::invalid_argument);
    EXPECT_EQ(factories.load(), 0);
}

TEST(ProjectTask, RequestEditingCannotRemoveAssociationHttpBoundsOrPoisonStorage) {
    ProjectTaskTemp temporary;
    DownloadService service(
        makeSqliteRepository(temporary.path / "tasks.db"),
        [] { return std::make_unique<ProjectTaskTransport>(); }, temporary.path / "parts");
    const auto id = service.addProjectChild(temporary.spec(), newTaskId(), 1, newTaskId()).get();
    auto replacement = temporary.spec();
    replacement.maxStoredBytes = 0;
    replacement.sameOriginRedirectOnly = false;
    EXPECT_THROW(service.updateRequestWithCredentials(id, replacement, false).get(), std::invalid_argument);
    service.updateUrl(id, "ftp://example.test/file");
    service.queues().get();
    ASSERT_TRUE(awaitProjectTask([&] { return !service.snapshot().empty(); }));
    EXPECT_EQ(service.snapshot().front().spec.url, temporary.spec().url);
    EXPECT_TRUE(service.lastError().empty());
    replacement = temporary.spec();
    replacement.url = "https://example.test/edited";
    service.updateRequestWithCredentials(id, replacement, false).get();
    ASSERT_TRUE(awaitProjectTask([&] { return service.snapshot().front().spec.url == replacement.url; }));
    EXPECT_FALSE(service.snapshot().front().projectId.empty());
    EXPECT_TRUE(service.lastError().empty());
}

TEST(ProjectTask, PolicyWaitingRecoveryPausesParentChildrenBeforeQueueCanOpen) {
    ProjectTaskTemp temporary;
    const auto database = temporary.path / "tasks.db";
    {
        auto store = makeSqliteStore(database);
        DownloadService initial(
            std::move(store.tasks), [] { return std::make_unique<ProjectTaskTransport>(); },
            temporary.path / "parts", {}, QueueDependencies{std::move(store.queues)});
        const auto queue = initial.queues().get().front();
        QueueConfiguration edit;
        edit.id = queue.id;
        edit.name = queue.name;
        edit.expectedRevision = queue.revision;
        edit.enabled = false;
        initial.configureQueue(edit).get();
    }
    std::string childId, ordinaryId;
    {
        auto store = makeSqliteStore(database);
        Task child;
        child.id = childId = newTaskId();
        child.requestId = newTaskId();
        child.spec = temporary.spec();
        child.state = TaskState::PolicyWaiting;
        child.runIntent = RunIntent::Run;
        child.projectId = newTaskId();
        child.projectRevision = 1;
        store.tasks->save(child);
        Task ordinary;
        ordinary.id = ordinaryId = newTaskId();
        ordinary.spec = temporary.spec();
        ordinary.spec.destination = temporary.path / "ordinary.bin";
        ordinary.state = TaskState::PolicyWaiting;
        ordinary.runIntent = RunIntent::Run;
        store.tasks->save(ordinary);
    }
    std::atomic_int factories = 0;
    auto store = makeSqliteStore(database);
    DownloadService recovered(
        std::move(store.tasks),
        [&] {
            ++factories;
            return std::make_unique<ProjectTaskTransport>();
        },
        temporary.path / "parts", {}, QueueDependencies{std::move(store.queues)});
    const auto task = [&](const std::string& id) {
        const auto rows = recovered.snapshot();
        return *std::find_if(rows.begin(), rows.end(), [&](const auto& value) { return value.id == id; });
    };
    EXPECT_EQ(task(childId).state, TaskState::Paused);
    EXPECT_EQ(task(childId).runIntent, RunIntent::Paused);
    EXPECT_EQ(task(ordinaryId).state, TaskState::PolicyWaiting);
    EXPECT_EQ(factories.load(), 0);
    const auto queue = recovered.queues().get().front();
    QueueConfiguration edit;
    edit.id = queue.id;
    edit.name = queue.name;
    edit.expectedRevision = queue.revision;
    recovered.configureQueue(edit).get();
    ASSERT_TRUE(awaitProjectTask([&] { return task(ordinaryId).state == TaskState::Completed; }));
    EXPECT_EQ(task(childId).state, TaskState::Paused);
    EXPECT_EQ(factories.load(), 1);
}
