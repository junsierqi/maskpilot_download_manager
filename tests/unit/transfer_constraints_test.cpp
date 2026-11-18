#include "../../src/ui/src/models/task_list_codec.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <limits>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <mutex>
#include <thread>

using namespace maskpilot;
namespace {
class ConstraintRepository final : public ITaskRepository {
  public:
    std::vector<Task> values;
    std::vector<Task> load() override { return values; }
    void save(const Task& task) override {
        auto found = std::find_if(values.begin(), values.end(),
                                  [&](const auto& value) { return value.id == task.id; });
        if (found == values.end())
            values.push_back(task);
        else
            *found = task;
    }
    void erase(const std::string& id) override {
        std::erase_if(values, [&](const auto& task) { return task.id == id; });
    }
};
struct ConstraintTemp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-constraints-" + newTaskId());
    ConstraintTemp() { std::filesystem::create_directories(path); }
    ~ConstraintTemp() {
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().string().starts_with("maskpilot-constraints-")) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
    DownloadSpec spec() const {
        DownloadSpec value;
        value.url = "https://example.test/file";
        value.destination = path / "file.bin";
        value.startPaused = true;
        return value;
    }
};
bool untilConstraint(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
class ConstraintTransfer final : public ITransferTransport {
  public:
    std::function<void(const Task&, TransferControl&, const std::function<void(TransferEvent)>&)> action;
    explicit ConstraintTransfer(decltype(action) fn) : action(std::move(fn)) {}
    void run(const Task& task, const std::filesystem::path&, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        action(task, control, callback);
    }
};
} // namespace

TEST(TransferConstraints, ValidatesExactBoundsAndHttpOnlyPolicy) {
    ConstraintTemp temporary;
    auto spec = temporary.spec();
    EXPECT_TRUE(validateSpec(spec).empty());
    spec.maxStoredBytes = 9007199254740991LL;
    spec.sameOriginRedirectOnly = true;
    EXPECT_TRUE(validateSpec(spec).empty());
    for (auto invalid : {-1LL, 9007199254740992LL, std::numeric_limits<long long>::max()}) {
        spec.maxStoredBytes = invalid;
        EXPECT_FALSE(validateSpec(spec).empty());
    }
    spec.maxStoredBytes = 1;
    for (const auto& url :
         {"ftp://example.test/file", "magnet:?xt=urn:btih:0123456789012345678901234567890123456789"}) {
        spec.url = url;
        EXPECT_FALSE(validateSpec(spec).empty());
    }
    spec = temporary.spec();
    spec.kind = TransferKind::Media;
    spec.maxStoredBytes = 1;
    EXPECT_FALSE(validateSpec(spec).empty());
    spec.maxStoredBytes = 0;
    spec.sameOriginRedirectOnly = true;
    EXPECT_FALSE(validateSpec(spec).empty());
}

TEST(TransferConstraintsCodec, RoundTripsExactBoundWithoutAcceptingObservedAuthority) {
    ConstraintTemp temporary;
    Task task;
    task.spec = temporary.spec();
    task.spec.maxStoredBytes = 9007199254740991LL;
    task.spec.sameOriginRedirectOnly = true;
    task.responseUrl = "https://example.test/observed-final";
    const auto bytes = ui::encodeTaskList({task});
    EXPECT_FALSE(bytes.contains("responseUrl"));
    EXPECT_FALSE(bytes.contains("observed-final"));
    const auto parsed = ui::decodeTaskList(bytes, [](const QString& url) {
        DownloadSpec spec;
        spec.url = url.toStdString();
        return spec;
    });
    ASSERT_EQ(parsed.downloads.size(), 1u);
    EXPECT_EQ(parsed.downloads[0].maxStoredBytes, 9007199254740991LL);
    EXPECT_TRUE(parsed.downloads[0].sameOriginRedirectOnly);
    EXPECT_TRUE(parsed.downloads[0].startPaused);
}

TEST(TransferConstraintsCodec, RejectsMalformedLimitsLegacyFlagsAndObservedUrlInput) {
    auto object = QJsonObject{{"url", "https://example.test/file"},
                              {"destination", "file.bin"},
                              {"proxyAuthenticationRequired", false},
                              {"originAuthenticationRequired", false}};
    auto parse = [](const QJsonObject& value, int version = 2) {
        return ui::decodeTaskList(
            QJsonDocument(QJsonObject{{"version", version}, {"downloads", QJsonArray{value}}}).toJson(),
            [](const QString& url) {
                DownloadSpec spec;
                spec.url = url.toStdString();
                return spec;
            });
    };
    for (const auto& invalid : {QJsonValue(-1), QJsonValue(1.5), QJsonValue("4096"), QJsonValue(true),
                                QJsonValue(9007199254740992.0)}) {
        auto bad = object;
        bad.insert("maxStoredBytes", invalid);
        EXPECT_THROW(parse(bad), std::exception);
    }
    auto bad = object;
    bad.insert("sameOriginRedirectOnly", 1);
    EXPECT_THROW(parse(bad), std::exception);
    bad = object;
    bad.insert("responseUrl", "https://example.test/observed");
    EXPECT_THROW(parse(bad), std::exception);
    bad = object;
    bad.insert("maxStoredBytes", 1);
    EXPECT_THROW(parse(bad, 1), std::exception);
    bad.insert("url", "ftp://example.test/file");
    EXPECT_THROW(parse(bad), std::exception);
}

TEST(TransferConstraintsCodec, OrdinaryListsOmitNewDefaultsAndLegacyImportStaysUnrestricted) {
    ConstraintTemp temporary;
    Task task;
    task.spec = temporary.spec();
    const auto bytes = ui::encodeTaskList({task});
    EXPECT_FALSE(bytes.contains("maxStoredBytes"));
    EXPECT_FALSE(bytes.contains("sameOriginRedirectOnly"));
    auto document = QJsonDocument::fromJson(bytes).object();
    document.insert("version", 1);
    const auto parsed = ui::decodeTaskList(QJsonDocument(document).toJson(), [](const QString& url) {
        DownloadSpec spec;
        spec.url = url.toStdString();
        return spec;
    });
    ASSERT_EQ(parsed.downloads.size(), 1u);
    EXPECT_EQ(parsed.downloads[0].maxStoredBytes, 0);
    EXPECT_FALSE(parsed.downloads[0].sameOriginRedirectOnly);
}

TEST(RequestConstraints, ExactReplayRejectsChangedCapOriginHashAndProxyRequirementBeforeFactories) {
    ConstraintTemp temporary;
    std::atomic_int factories = 0;
    DownloadService service(
        std::make_unique<ConstraintRepository>(),
        [&]() -> std::unique_ptr<ITransferTransport> {
            ++factories;
            throw std::runtime_error("Unexpected network factory");
        },
        temporary.path / "cache");
    auto original = temporary.spec();
    original.expectedSha256 = std::string(64, 'a');
    original.proxy = "http://127.0.0.1:8181";
    const auto request = newTaskId();
    const auto id = service.add(original, request).get();
    EXPECT_EQ(service.add(original, request).get(), id);
    auto upper = original;
    upper.expectedSha256 = std::string(64, 'A');
    EXPECT_EQ(service.add(upper, request).get(), id);
    for (int field = 0; field != 4; ++field) {
        auto changed = original;
        if (field == 0)
            changed.maxStoredBytes = 1024;
        if (field == 1)
            changed.sameOriginRedirectOnly = true;
        if (field == 2)
            changed.expectedSha256 = std::string(64, 'b');
        if (field == 3)
            changed.proxyAuthenticationRequired = true;
        ASSERT_TRUE(validateSpec(changed).empty()) << field;
        EXPECT_THROW(service.add(changed, request).get(), std::invalid_argument) << field;
    }
    EXPECT_EQ(service.snapshot().size(), 1u);
    EXPECT_EQ(factories.load(), 0);
}

TEST(RequestConstraints, MediaReplayCannotReuseDifferentExplicitOrAutomaticOutput) {
    ConstraintTemp temporary;
    DownloadService service(
        std::make_unique<ConstraintRepository>(),
        []() -> std::unique_ptr<ITransferTransport> { throw std::runtime_error("Unexpected media factory"); },
        temporary.path / "cache");
    auto original = temporary.spec();
    original.kind = TransferKind::Media;
    original.mediaSelection = {MediaSelectionMode::VideoAudio, "video", "audio", MediaContainer::Mkv};
    const auto request = newTaskId();
    const auto id = service.add(original, request).get();
    EXPECT_EQ(service.add(original, request).get(), id);
    for (int field = 0; field != 4; ++field) {
        auto changed = original;
        if (field == 0)
            changed.mediaSelection.primaryFormat = "other-video";
        if (field == 1)
            changed.mediaSelection.audioFormat = "other-audio";
        if (field == 2)
            changed.mediaSelection.container = MediaContainer::Mp4;
        if (field == 3)
            changed.mediaSelection = {};
        EXPECT_THROW(service.add(changed, request).get(), std::invalid_argument) << field;
    }
    auto automatic = original;
    automatic.mediaSelection = {};
    const auto automaticRequest = newTaskId();
    service.add(automatic, automaticRequest).get();
    auto audio = automatic;
    audio.audioOnly = true;
    EXPECT_THROW(service.add(audio, automaticRequest).get(), std::invalid_argument);
    automatic.mediaFormat = "best[height<=720]";
    EXPECT_THROW(service.add(automatic, automaticRequest).get(), std::invalid_argument);
}

TEST(ResponseMetadata, RefreshUrlClearsObservationBeforeSavingHttpOrFtpReplacement) {
    ConstraintTemp temporary;
    for (const auto* url : {"https://example.test/replaced", "ftp://example.test/replaced"}) {
        const auto database = temporary.path / (isFtpUrl(url) ? "ftp.db" : "http.db");
        auto repository = makeSqliteRepository(database);
        Task saved;
        saved.id = newTaskId();
        saved.spec = temporary.spec();
        saved.state = TaskState::Paused;
        saved.runIntent = RunIntent::Paused;
        saved.responseUrl = "https://example.test/old";
        repository->save(saved);
        {
            DownloadService service(
                std::move(repository),
                []() -> std::unique_ptr<ITransferTransport> {
                    throw std::runtime_error("Paused edit must not start a transfer");
                },
                temporary.path / "cache");
            service.updateUrl(saved.id, url);
            ASSERT_TRUE(untilConstraint([&] {
                const auto tasks = service.snapshot();
                return tasks.size() == 1 && tasks[0].spec.url == url && tasks[0].responseUrl.empty();
            }));
            const auto tasks = service.snapshot();
            ASSERT_EQ(tasks.size(), 1u);
            EXPECT_EQ(tasks[0].spec.url, url);
            EXPECT_TRUE(tasks[0].responseUrl.empty());
        }
        const auto reopened = makeSqliteRepository(database)->load();
        ASSERT_EQ(reopened.size(), 1u);
        EXPECT_EQ(reopened[0].spec.url, url);
        EXPECT_TRUE(reopened[0].responseUrl.empty());
    }
}

TEST(ResponseMetadata, ExplicitRestartClearsObservationWhileQueueRemainsDisabled) {
    ConstraintTemp temporary;
    auto store = makeSqliteStore(temporary.path / "restart.db");
    Task saved;
    saved.id = newTaskId();
    saved.spec = temporary.spec();
    saved.state = TaskState::Paused;
    saved.runIntent = RunIntent::Paused;
    saved.responseUrl = "https://example.test/old";
    store.tasks->save(saved);
    QueueDependencies dependencies;
    dependencies.repository = std::move(store.queues);
    std::atomic_int factories = 0;
    DownloadService service(
        std::move(store.tasks),
        [&]() -> std::unique_ptr<ITransferTransport> {
            ++factories;
            throw std::runtime_error("Disabled queue must not start a transfer");
        },
        temporary.path / "cache", ContextDependencies{}, std::move(dependencies));
    const auto queue = service.queues().get().front();
    QueueConfiguration edit;
    edit.id = queue.id;
    edit.name = queue.name;
    edit.expectedRevision = queue.revision;
    edit.enabled = false;
    service.configureQueue(edit).get();
    service.restart(saved.id);
    ASSERT_TRUE(untilConstraint([&] {
        const auto tasks = service.snapshot();
        return tasks.size() == 1 && tasks[0].responseUrl.empty();
    }));
    EXPECT_TRUE(service.snapshot()[0].responseUrl.empty());
    EXPECT_EQ(factories.load(), 0);
}

TEST(ResponseMetadata, TerminalOnlyObservationIsSavedAndNewRunSeesNoOldMetadata) {
    ConstraintTemp temporary;
    auto repository = std::make_unique<ConstraintRepository>();
    Task saved;
    saved.id = newTaskId();
    saved.spec = temporary.spec();
    saved.state = TaskState::Paused;
    saved.runIntent = RunIntent::Paused;
    saved.responseUrl = "https://example.test/old";
    repository->values.push_back(saved);
    std::atomic_int calls = 0;
    DownloadService service(
        std::move(repository),
        [&]() {
            return std::make_unique<ConstraintTransfer>(
                [&](const Task& task, TransferControl&, const auto& callback) {
                    EXPECT_TRUE(task.responseUrl.empty());
                    ++calls;
                    TransferEvent complete;
                    complete.outcome = TransferOutcome::Complete;
                    complete.responseUrl = "https://example.test/docs/index.html";
                    callback(complete);
                });
        },
        temporary.path / "cache");
    service.resume(saved.id);
    ASSERT_TRUE(untilConstraint([&] {
        const auto tasks = service.snapshot();
        return !tasks.empty() && tasks[0].state == TaskState::Completed;
    }));
    EXPECT_EQ(service.snapshot()[0].responseUrl, "https://example.test/docs/index.html");
    EXPECT_EQ(calls.load(), 1);
}

TEST(ResponseMetadata, FailedCredentialPreflightClearsOldResponseWithoutStartingTransport) {
    ConstraintTemp temporary;
    auto repository = std::make_unique<ConstraintRepository>();
    Task saved;
    saved.id = newTaskId();
    saved.spec = temporary.spec();
    saved.spec.proxy = "http://127.0.0.1:1";
    saved.spec.proxyAuthenticationRequired = true;
    saved.state = TaskState::Paused;
    saved.runIntent = RunIntent::Paused;
    saved.responseUrl = "https://example.test/old";
    repository->values.push_back(saved);
    std::atomic_int factories = 0;
    DownloadService service(
        std::move(repository),
        [&]() -> std::unique_ptr<ITransferTransport> {
            ++factories;
            throw std::runtime_error("Unexpected factory before credentials");
        },
        temporary.path / "cache");
    service.resume(saved.id);
    ASSERT_TRUE(untilConstraint([&] {
        const auto tasks = service.snapshot();
        return !tasks.empty() && tasks[0].state == TaskState::NeedsAttention;
    }));
    EXPECT_TRUE(service.snapshot()[0].responseUrl.empty());
    EXPECT_EQ(factories.load(), 0);
}

TEST(ResponseMetadata, InvalidObservationFailsClosedAndLateEventCannotResurrectCompletion) {
    ConstraintTemp temporary;
    auto factory = []() {
        return std::make_unique<ConstraintTransfer>([](const Task&, TransferControl&, const auto& callback) {
            TransferEvent invalid;
            invalid.outcome = TransferOutcome::Complete;
            invalid.responseUrl = "file:///C:/private.txt";
            callback(invalid);
            TransferEvent late;
            late.responseUrl = "https://example.test/late";
            callback(late);
        });
    };
    DownloadService service(std::make_unique<ConstraintRepository>(), factory, temporary.path / "cache");
    auto spec = temporary.spec();
    spec.startPaused = false;
    const auto id = service.add(spec).get();
    ASSERT_TRUE(untilConstraint([&] {
        const auto tasks = service.snapshot();
        return !tasks.empty() && tasks[0].state == TaskState::NeedsAttention;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto tasks = service.snapshot();
    ASSERT_EQ(tasks.size(), 1u);
    EXPECT_EQ(tasks[0].id, id);
    EXPECT_EQ(tasks[0].state, TaskState::NeedsAttention);
    EXPECT_TRUE(tasks[0].responseUrl.empty());
}
