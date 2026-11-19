#include "../support/local_http_server.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <thread>

using namespace maskpilot;
namespace {
CompletionScanOptions options(const std::string& mode = "pass") {
    CompletionScanOptions value;
    value.enabled = true;
    value.program = std::filesystem::path(MASKPILOT_TEST_SCAN_PROBE);
    value.arguments = {"--mode", mode, "{file}"};
    value.timeoutSeconds = 3;
    return value;
}
bool pump(const std::function<bool()>& predicate, int timeout = 15000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return predicate();
}
void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
    ASSERT_TRUE(file.good());
}
} // namespace

TEST(CompletionScanOptions, RejectsAmbiguousArgumentsBoundsAndFalseSavedApproval) {
    auto value = options();
    EXPECT_TRUE(validateCompletionScanOptions(value).empty());
    for (const auto arguments :
         {std::vector<std::string>{}, std::vector<std::string>{"{file}", "{file}"},
          std::vector<std::string>{"prefix{file}"}, std::vector<std::string>{"{file}", "a\nb"}}) {
        value.arguments = arguments;
        EXPECT_FALSE(validateCompletionScanOptions(value).empty());
    }
    value = options();
    value.timeoutSeconds = 0;
    EXPECT_FALSE(validateCompletionScanOptions(value).empty());
    value = options();
    value.program = "relative.exe";
    EXPECT_FALSE(validateCompletionScanOptions(value).empty());
    Task task;
    task.scanState = ScanState::Passed;
    EXPECT_FALSE(validateCompletionScanRecord(task).empty());
    task.scanRequired = true;
    EXPECT_TRUE(validateCompletionScanRecord(task).empty());
    task.state = TaskState::Completed;
    task.scanState = ScanState::Pending;
    EXPECT_FALSE(validateCompletionScanRecord(task).empty());
    task.scanRequired = false;
    task.scanState = ScanState::Waived;
    EXPECT_TRUE(validateCompletionScanRecord(task).empty());
}

TEST(CompletionScanner, ActualExecutablePassRejectionMutationAndFloodHaveDistinctResults) {
    int argc = 1;
    char name[] = "CompletionScanner";
    char* argv[]{name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = std::filesystem::path(temporary.path().toStdWString()) / L"verified file.bin";
    for (const auto& [mode, expected] : {std::pair{"pass", ScanState::Passed},
                                         {"reject", ScanState::Rejected},
                                         {"mutate", ScanState::Error},
                                         {"flood", ScanState::Passed},
                                         {"crash", ScanState::Error}}) {
        SCOPED_TRACE(mode);
        write(path, "owned verified bytes");
        TransferControl control;
        const auto result = makeCompletionScanner(options(mode))->scan(path, control);
        EXPECT_EQ(result.state, expected) << result.error;
        EXPECT_EQ(result.outcome,
                  expected == ScanState::Passed ? TransferOutcome::Complete : TransferOutcome::Attention);
        EXPECT_TRUE(std::filesystem::exists(path));
    }
    auto invalid = options();
    invalid.program = path;
    EXPECT_THROW(makeCompletionScanner(invalid), std::invalid_argument);
}

TEST(CompletionScanner, TimeoutAndPauseStopOwnedProcessWithoutApprovingBytes) {
    int argc = 1;
    char name[] = "CompletionScannerStop";
    char* argv[]{name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = std::filesystem::path(temporary.path().toStdWString()) / L"stage.bin";
    write(path, "owned bytes");
    auto value = options("sleep");
    value.timeoutSeconds = 1;
    TransferControl timeout;
    const auto first = makeCompletionScanner(value)->scan(path, timeout);
    EXPECT_EQ(first.state, ScanState::Error);
    EXPECT_NE(first.error.find("timed out"), std::string::npos);
    value.timeoutSeconds = 5;
    TransferControl pause;
    std::jthread trigger([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        pause.pause = true;
    });
    const auto second = makeCompletionScanner(value)->scan(path, pause);
    EXPECT_EQ(second.outcome, TransferOutcome::Paused);
    EXPECT_EQ(second.state, ScanState::Pending);
}

TEST(CompletionScanStorage, RequiredAndWaivedRecordsSurviveWhileInvalidApprovalCannotBeSaved) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    auto store = makeSqliteRepository(root / L"tasks.db");
    Task task;
    task.id = newTaskId();
    task.spec.url = "https://example.test/file";
    task.spec.destination = root / L"final.bin";
    task.state = TaskState::Paused;
    task.scanRequired = true;
    task.scanState = ScanState::Pending;
    store->save(task);
    EXPECT_TRUE(store->load()[0].scanRequired);
    task.state = TaskState::Completed;
    EXPECT_THROW(store->save(task), std::invalid_argument);
    task.scanState = ScanState::Passed;
    store->save(task);
    EXPECT_EQ(store->load()[0].scanState, ScanState::Passed);
    task.scanRequired = false;
    task.scanState = ScanState::Waived;
    store->save(task);
    EXPECT_EQ(store->load()[0].scanState, ScanState::Waived);
}

TEST(CompletionScanService, ActualHttpPauseMissingPolicyAndExplicitWaiverPreservePublicationGate) {
    int argc = 1;
    char name[] = "CompletionScannerHttp";
    char* argv[]{name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    LocalHttpServer origin;
    DownloadService service(makeSqliteRepository(root / L"tasks.db"), [] { return makeCurlTransfer(); },
                            root / L"parts", {}, {}, {options("sleep"), makeCompletionScanner});
    DownloadSpec spec;
    spec.url = origin.url("/scan").toStdString();
    spec.destination = root / L"final.bin";
    const auto id = service.add(spec).get();
    ASSERT_TRUE(pump([&] {
        const auto all = service.snapshot();
        return std::any_of(all.begin(), all.end(), [&](const auto& item) { return item.id == id; });
    }));
    auto task = [&] {
        const auto all = service.snapshot();
        return *std::find_if(all.begin(), all.end(), [&](const auto& item) { return item.id == id; });
    };
    ASSERT_TRUE(pump([&] { return task().scanState == ScanState::Scanning; }));
    EXPECT_FALSE(std::filesystem::exists(spec.destination));
    CompletionScanOptions disabled;
    EXPECT_THROW(service.configureCompletionScan(disabled).get(), std::invalid_argument);
    service.pause(id);
    ASSERT_TRUE(pump([&] { return task().state == TaskState::Paused; }));
    service.configureCompletionScan(disabled).get();
    const auto count = origin.requests("/scan");
    service.resume(id);
    ASSERT_TRUE(pump([&] { return task().state == TaskState::NeedsAttention; }));
    EXPECT_EQ(origin.requests("/scan"), count);
    EXPECT_TRUE(task().scanRequired);
    service.setCompletionScanRequired(id, false).get();
    EXPECT_EQ(task().scanState, ScanState::Waived);
    service.resume(id);
    ASSERT_TRUE(pump([&] { return task().state == TaskState::Completed; }));
    QFile final(QString::fromStdWString(spec.destination.wstring()));
    ASSERT_TRUE(final.open(QIODevice::ReadOnly));
    EXPECT_EQ(final.readAll(), origin.data());
    EXPECT_EQ(task().scanState, ScanState::Waived);
}
TEST(CompletionScanService, WorkerScannerFactoryFailureRetainsItsErrorObserverAndNeverRunsTransport) {
    int argc = 1;
    char name[] = "CompletionScannerFactoryFault";
    char* argv[]{name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    struct Scanner final : ICompletionScanner {
        CompletionScanResult scan(const std::filesystem::path&, const TransferControl&) override {
            return {};
        }
    };
    struct Transport final : ITransferTransport {
        std::atomic_int& runs;
        explicit Transport(std::atomic_int& count) : runs(count) {}
        void run(const Task&, const std::filesystem::path&, TransferControl&,
                 const std::function<void(TransferEvent)>&) override {
            ++runs;
        }
    };
    std::atomic_int factories = 0, runs = 0;
    CompletionScanDependencies dependencies{options(),
                                            [&](const auto&) -> std::shared_ptr<ICompletionScanner> {
                                                if (++factories > 1)
                                                    throw std::runtime_error("Owned worker factory failure");
                                                return std::make_shared<Scanner>();
                                            }};
    DownloadService service(
        makeSqliteRepository(root / L"tasks.db"), [&] { return std::make_unique<Transport>(runs); },
        root / L"parts", {}, {}, dependencies);
    DownloadSpec spec;
    spec.url = "https://example.test/not-contacted";
    spec.destination = root / L"not-created.bin";
    const auto id = service.add(spec).get();
    ASSERT_TRUE(pump([&] {
        const auto all = service.snapshot();
        return std::any_of(all.begin(), all.end(), [&](const auto& task) {
            return task.id == id && task.state == TaskState::NeedsAttention &&
                   task.scanState == ScanState::Error;
        });
    }));
    EXPECT_EQ(runs.load(), 0);
    EXPECT_FALSE(std::filesystem::exists(spec.destination));
}

TEST(CompletionScanService, UnavailableStartupScannerBlocksOriginButAllowsSettingsRepair) {
    int argc = 1;
    char name[] = "CompletionScannerStartupFault";
    char* argv[]{name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    LocalHttpServer origin;
    CompletionScanDependencies dependencies{options(),
                                            [](const auto&) -> std::shared_ptr<ICompletionScanner> {
                                                throw std::runtime_error("Missing executable");
                                            }};
    DownloadService service(
        makeSqliteRepository(root / L"tasks.db"), [] { return makeCurlTransfer(); }, root / L"parts", {}, {},
        dependencies);
    EXPECT_FALSE(service.completionScanIssue().get().empty());
    DownloadSpec spec;
    spec.url = origin.url("/unavailable").toStdString();
    spec.destination = root / L"final.bin";
    const auto id = service.add(spec).get();
    auto task = [&] {
        const auto all = service.snapshot();
        return *std::find_if(all.begin(), all.end(), [&](const auto& item) { return item.id == id; });
    };
    ASSERT_TRUE(pump([&] { return task().state == TaskState::NeedsAttention; }));
    EXPECT_TRUE(task().scanRequired);
    EXPECT_EQ(task().scanState, ScanState::Pending);
    EXPECT_EQ(origin.requests("/unavailable"), 0);
    service.configureCompletionScan({}).get();
    EXPECT_TRUE(service.completionScanIssue().get().empty());
    service.resume(id);
    ASSERT_TRUE(pump([&] { return task().state == TaskState::NeedsAttention; }));
    EXPECT_EQ(origin.requests("/unavailable"), 0);
    service.setCompletionScanRequired(id, false).get();
    EXPECT_EQ(task().state, TaskState::Paused);
    EXPECT_EQ(task().scanState, ScanState::Waived);
    service.resume(id);
    ASSERT_TRUE(pump([&] { return task().state == TaskState::Completed; }));
    QFile final(QString::fromStdWString(spec.destination.wstring()));
    ASSERT_TRUE(final.open(QIODevice::ReadOnly));
    EXPECT_EQ(final.readAll(), origin.data());
}
