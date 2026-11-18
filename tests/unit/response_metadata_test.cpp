#include "../../src/ui/src/models/task_list_codec.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <mutex>
#include <thread>

using namespace maskpilot;
namespace {
struct ResponseTemp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-response-lifecycle-" + newTaskId());
    ResponseTemp() { std::filesystem::create_directories(path); }
    ~ResponseTemp() {
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().string().starts_with("maskpilot-response-lifecycle-")) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
    Task saved() const {
        Task task;
        task.id = newTaskId();
        task.spec.url = "https://example.test/source";
        task.spec.destination = path / "page.html";
        task.spec.startPaused = true;
        task.state = TaskState::Paused;
        task.runIntent = RunIntent::Paused;
        task.responseUrl = "https://example.test/old";
        task.responseContentType = "text/html; charset=UTF-8";
        return task;
    }
};
bool waitResponse(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
class ResponseTransport final : public ITransferTransport {
  public:
    using Action =
        std::function<void(const Task&, TransferControl&, const std::function<void(TransferEvent)>&)>;
    explicit ResponseTransport(Action action) : action_(std::move(action)) {}
    void run(const Task& task, const std::filesystem::path&, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        action_(task, control, callback);
    }

  private:
    Action action_;
};
TransportFactory neverResponseTransfer(std::atomic_int& calls) {
    return [&]() -> std::unique_ptr<ITransferTransport> {
        ++calls;
        throw std::runtime_error("No transfer is permitted in this lifecycle fixture");
    };
}
} // namespace

TEST(ResponseContentType, PrintableAsciiBoundIsNotMimeOrExtensionGuessing) {
    EXPECT_TRUE(validHttpResponseContentType(""));
    EXPECT_TRUE(validHttpResponseContentType("Text/HTML; charset=\"UTF-8\""));
    EXPECT_TRUE(validHttpResponseContentType("unknown-but-printable"));
    EXPECT_TRUE(validHttpResponseContentType(std::string(1024, 'x')));
    for (const auto& invalid :
         {std::string(1025, 'x'), std::string("text/html\n"), std::string("text/html\r"),
          std::string("text/\thtml"), std::string("text/html\0x", 11), std::string("text/\xc3\xa9"),
          std::string("text/\x7f")})
        EXPECT_FALSE(validHttpResponseContentType(invalid));
}

TEST(ResponseContentTypeCodec, ObservationIsNeverExportedOrAdoptedFromAnyListVersion) {
    ResponseTemp temporary;
    const auto task = temporary.saved();
    const auto bytes = ui::encodeTaskList({task});
    EXPECT_FALSE(bytes.contains("responseContentType"));
    EXPECT_FALSE(bytes.contains("response_content_type"));
    EXPECT_FALSE(bytes.contains("text/html"));
    for (const auto* key : {"responseContentType", "response_content_type"}) {
        for (const auto version : {1, 2}) {
            auto root = QJsonDocument::fromJson(bytes).object();
            root.insert("version", version);
            auto value = root.value("downloads").toArray()[0].toObject();
            value.insert(key, "text/html");
            root.insert("downloads", QJsonArray{value});
            EXPECT_THROW(ui::decodeTaskList(QJsonDocument(root).toJson(),
                                            [](const QString& url) {
                                                DownloadSpec spec;
                                                spec.url = url.toStdString();
                                                return spec;
                                            }),
                         std::exception);
        }
    }
}

TEST(ResponseContentTypeLifecycle, UrlAndRequestEditsClearBothObservationsBeforeDurableSave) {
    ResponseTemp temporary;
    for (int operation = 0; operation < 3; ++operation) {
        const auto database = temporary.path / ("edit-" + std::to_string(operation) + ".db");
        auto repository = makeSqliteRepository(database);
        const auto saved = temporary.saved();
        repository->save(saved);
        std::atomic_int calls = 0;
        {
            DownloadService service(std::move(repository), neverResponseTransfer(calls),
                                    temporary.path / "cache");
            if (operation < 2)
                service.updateUrl(saved.id,
                                  operation == 0 ? "https://example.test/new" : "ftp://example.test/new");
            else {
                auto edited = saved.spec;
                edited.category = "Changed";
                service.updateRequest(saved.id, edited);
            }
            ASSERT_TRUE(waitResponse([&] {
                const auto tasks = service.snapshot();
                return tasks.size() == 1 && tasks[0].responseUrl.empty() &&
                       tasks[0].responseContentType.empty();
            }));
            EXPECT_EQ(calls.load(), 0);
        }
        const auto loaded = makeSqliteRepository(database)->load();
        ASSERT_EQ(loaded.size(), 1u);
        EXPECT_TRUE(loaded[0].responseUrl.empty());
        EXPECT_TRUE(loaded[0].responseContentType.empty());
    }
}

TEST(ResponseContentTypeLifecycle, MissingCredentialsClearMimeDuringFailedPreflightWithoutFactory) {
    ResponseTemp temporary;
    const auto database = temporary.path / "preflight.db";
    auto repository = makeSqliteRepository(database);
    auto saved = temporary.saved();
    saved.spec.originAuthenticationRequired = true;
    repository->save(saved);
    std::atomic_int calls = 0;
    {
        DownloadService service(std::move(repository), neverResponseTransfer(calls),
                                temporary.path / "cache");
        service.resume(saved.id);
        ASSERT_TRUE(waitResponse([&] {
            const auto tasks = service.snapshot();
            return !tasks.empty() && tasks[0].state == TaskState::NeedsAttention;
        }));
        EXPECT_TRUE(service.snapshot()[0].responseContentType.empty());
        EXPECT_TRUE(service.snapshot()[0].responseUrl.empty());
        EXPECT_EQ(calls.load(), 0);
    }
    const auto loaded = makeSqliteRepository(database)->load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_TRUE(loaded[0].responseContentType.empty());
}

TEST(ResponseContentTypeLifecycle, RestartClearsMimeWhileDisabledQueuePreventsAnyTransport) {
    ResponseTemp temporary;
    auto store = makeSqliteStore(temporary.path / "restart.db");
    const auto saved = temporary.saved();
    store.tasks->save(saved);
    QueueDependencies dependencies;
    dependencies.repository = std::move(store.queues);
    std::atomic_int calls = 0;
    DownloadService service(std::move(store.tasks), neverResponseTransfer(calls), temporary.path / "cache",
                            ContextDependencies{}, std::move(dependencies));
    const auto queue = service.queues().get().front();
    QueueConfiguration edit;
    edit.id = queue.id;
    edit.name = queue.name;
    edit.expectedRevision = queue.revision;
    edit.enabled = false;
    service.configureQueue(edit).get();
    service.restart(saved.id);
    ASSERT_TRUE(waitResponse([&] {
        const auto tasks = service.snapshot();
        return !tasks.empty() && tasks[0].responseContentType.empty();
    }));
    EXPECT_TRUE(service.snapshot()[0].responseUrl.empty());
    EXPECT_EQ(calls.load(), 0);
}

TEST(ResponseContentTypeLifecycle, InvalidMimeOrMimeWithoutUrlCannotCompleteAndLateValidEventCannotRepairIt) {
    ResponseTemp temporary;
    for (const bool missingUrl : {false, true}) {
        auto factory = [missingUrl]() {
            return std::make_unique<ResponseTransport>(
                [missingUrl](const Task&, TransferControl&, const auto& callback) {
                    TransferEvent event;
                    event.outcome = TransferOutcome::Complete;
                    event.responseUrl = missingUrl ? "" : "https://example.test/final";
                    event.responseContentType = missingUrl ? "text/html" : "text/html\nInjected";
                    callback(event);
                    event.responseUrl = "https://example.test/late";
                    event.responseContentType = "text/plain";
                    callback(event);
                });
        };
        DownloadService service(
            makeSqliteRepository(temporary.path / (missingUrl ? "missing.db" : "unsafe.db")), factory,
            temporary.path / "cache");
        auto spec = temporary.saved().spec;
        spec.startPaused = false;
        service.add(spec).get();
        ASSERT_TRUE(waitResponse([&] {
            const auto tasks = service.snapshot();
            return !tasks.empty() && tasks[0].state == TaskState::NeedsAttention;
        }));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        EXPECT_TRUE(service.snapshot()[0].responseUrl.empty());
        EXPECT_TRUE(service.snapshot()[0].responseContentType.empty());
        EXPECT_EQ(service.snapshot()[0].state, TaskState::NeedsAttention);
    }
}

TEST(ResponseContentTypeLifecycle, CurrentEmptyTypeClearsPriorTypeAndOldGenerationCannotPoisonNextRun) {
    ResponseTemp temporary;
    std::atomic_int runs = 0;
    std::atomic_bool release = false, secondStarted = false;
    std::mutex mutex;
    std::function<void(TransferEvent)> oldCallback;
    auto factory = [&]() {
        return std::make_unique<ResponseTransport>(
            [&](const Task& task, TransferControl& control, const auto& callback) {
                EXPECT_TRUE(task.responseContentType.empty());
                EXPECT_TRUE(task.responseUrl.empty());
                const auto run = ++runs;
                TransferEvent event;
                event.responseUrl = "https://example.test/final";
                event.responseContentType = "text/html";
                if (run == 1) {
                    {
                        std::lock_guard lock(mutex);
                        oldCallback = callback;
                    }
                    callback(event);
                    event.responseContentType.clear();
                } else {
                    secondStarted = true;
                    while (!release && !control.cancel)
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    event.responseContentType = "text/css";
                }
                event.outcome = run == 1 ? TransferOutcome::Paused : TransferOutcome::Complete;
                callback(event);
            });
    };
    DownloadService service(makeSqliteRepository(temporary.path / "generations.db"), factory,
                            temporary.path / "cache");
    auto spec = temporary.saved().spec;
    spec.startPaused = false;
    const auto id = service.add(spec).get();
    ASSERT_TRUE(waitResponse([&] {
        const auto tasks = service.snapshot();
        return !tasks.empty() && tasks[0].state == TaskState::Paused;
    }));
    EXPECT_TRUE(service.snapshot()[0].responseContentType.empty());
    service.resume(id);
    ASSERT_TRUE(waitResponse([&] { return secondStarted.load(); }));
    {
        std::lock_guard lock(mutex);
        TransferEvent stale;
        stale.responseUrl = "https://example.test/stale";
        stale.responseContentType = "application/javascript";
        oldCallback(stale);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(service.snapshot()[0].responseUrl.empty());
    EXPECT_TRUE(service.snapshot()[0].responseContentType.empty());
    release = true;
    ASSERT_TRUE(waitResponse([&] {
        const auto tasks = service.snapshot();
        return !tasks.empty() && tasks[0].state == TaskState::Completed;
    }));
    EXPECT_EQ(service.snapshot()[0].responseContentType, "text/css");
    EXPECT_EQ(runs.load(), 2);
}
