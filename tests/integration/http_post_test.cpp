#include "../../src/application/src/browser_recovery_registry.h"
#include "../../src/ui/src/models/task_list_codec.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/ui/add_download_dialog.h>
#include <sqlite3.h>
#include <thread>

using namespace maskpilot;
namespace {
DownloadSpec postSpec(const std::filesystem::path& destination) {
    DownloadSpec spec;
    spec.url = "http://127.0.0.1:9/owned";
    spec.destination = destination;
    spec.httpRequest.method = HttpMethod::Post;
    spec.httpRequest.contentType = "application/json";
    spec.httpRequest.body = "owned-private-post-body";
    spec.httpRequest.bodyProvided = true;
    return spec;
}
class RetryableFailure final : public ITransferTransport {
    std::shared_ptr<std::atomic_int> calls_;

  public:
    explicit RetryableFailure(std::shared_ptr<std::atomic_int> calls) : calls_(std::move(calls)) {}
    void run(const Task& task, const std::filesystem::path&, TransferControl&,
             const std::function<void(TransferEvent)>& callback) override {
        EXPECT_TRUE(task.spec.httpRequest.bodyProvided);
        EXPECT_EQ(task.spec.httpRequest.body, "owned-private-post-body");
        ++*calls_;
        TransferEvent event;
        event.outcome = TransferOutcome::Failed;
        event.retryable = true;
        event.error = "Owned interrupted response.";
        callback(event);
    }
};
bool waitAttention(DownloadService& service) {
    for (int n = 0; n < 150; ++n) {
        const auto tasks = service.snapshot();
        if (tasks.size() == 1 && tasks[0].state == TaskState::NeedsAttention)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
} // namespace
TEST(HttpPost, ValidatesBodyMethodMimeAndProtocolBoundaries) {
    auto spec = postSpec("owned.bin");
    EXPECT_TRUE(validateSpec(spec).empty());
    spec.httpRequest.body.clear();
    EXPECT_TRUE(validateHttpRequest(spec.httpRequest, true).empty());
    spec.httpRequest.bodyProvided = false;
    EXPECT_FALSE(validateHttpRequest(spec.httpRequest, true).empty());
    spec.httpRequest.body = std::string(1024 * 1024 + 1, 'a');
    EXPECT_FALSE(validateSpec(spec).empty());
    spec.httpRequest.body.clear();
    for (const auto* type : {"text/plain\r\nX: bad", "text", "/plain", "text/", "text/pl/ain"}) {
        spec.httpRequest.contentType = type;
        EXPECT_FALSE(validateSpec(spec).empty());
    }
    spec.httpRequest.contentType = "application/json; charset=utf-8";
    spec.headers = {{"Content-Type", "text/plain"}};
    EXPECT_FALSE(validateSpec(spec).empty());
    spec.headers.clear();
    spec.url = "ftp://example.test/file";
    EXPECT_FALSE(validateSpec(spec).empty());
    spec.url = "https://example.test/file";
    spec.kind = TransferKind::Media;
    EXPECT_FALSE(validateSpec(spec).empty());
    spec.kind = TransferKind::Http;
    spec.httpRequest.method = static_cast<HttpMethod>(2);
    EXPECT_FALSE(validateSpec(spec).empty());
}
TEST(HttpPost, PersistenceAndExportPreserveMethodButNeverSubmissionAuthority) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto root = std::filesystem::path(temp.path().toStdWString());
    auto repository = makeSqliteRepository(root / "tasks.db");
    Task task;
    task.id = newTaskId();
    task.spec = postSpec(root / "result.bin");
    repository->save(task);
    const auto loaded = repository->load();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded[0].spec.httpRequest.method, HttpMethod::Post);
    EXPECT_EQ(loaded[0].spec.httpRequest.contentType, "application/json");
    EXPECT_FALSE(loaded[0].spec.httpRequest.bodyProvided);
    EXPECT_TRUE(loaded[0].spec.httpRequest.body.empty());
    const auto encoded = ui::encodeTaskList({task});
    EXPECT_FALSE(encoded.contains("owned-private-post-body"));
    const auto defaults = [](const QString&) { return DownloadSpec{}; };
    const auto decoded = ui::decodeTaskList(encoded, defaults);
    ASSERT_EQ(decoded.downloads.size(), 1);
    EXPECT_EQ(decoded.downloads[0].httpRequest.method, HttpMethod::Post);
    EXPECT_TRUE(decoded.downloads[0].startPaused);
    EXPECT_FALSE(decoded.downloads[0].httpRequest.bodyProvided);
    EXPECT_TRUE(decoded.downloads[0].httpRequest.body.empty());
    auto document = QJsonDocument::fromJson(encoded).object();
    auto array = document["downloads"].toArray();
    auto value = array[0].toObject();
    value.insert("body", "injected");
    array[0] = value;
    document["downloads"] = array;
    EXPECT_THROW(ui::decodeTaskList(QJsonDocument(document).toJson(), defaults), std::exception);
    document = QJsonDocument::fromJson(encoded).object();
    document["version"] = 1;
    EXPECT_THROW(ui::decodeTaskList(QJsonDocument(document).toJson(), defaults), std::exception);
    task.projectId = newTaskId();
    task.projectRevision = 1;
    task.spec.maxStoredBytes = 1024;
    task.spec.sameOriginRedirectOnly = true;
    EXPECT_THROW(repository->save(task), std::exception);
    task.projectId.clear();
    task.projectRevision = 0;
    task.runIntent = RunIntent::Paused;
    task.state = TaskState::Paused;
    BrowserRecoveryRegistry recovery;
    EXPECT_THROW(recovery.begin(task, 1000), std::invalid_argument);
}
TEST(HttpPost, CoordinatorConsumesAuthorityAndNeverRetriesEvenRetryableTransportFailure) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto root = std::filesystem::path(temp.path().toStdWString());
    auto calls = std::make_shared<std::atomic_int>(0);
    DownloadService service(
        makeSqliteRepository(root / "tasks.db"),
        [calls] { return std::make_unique<RetryableFailure>(calls); }, root / "work");
    const auto id = service.add(postSpec(root / "result.bin")).get();
    ASSERT_TRUE(waitAttention(service));
    EXPECT_EQ(calls->load(), 1);
    auto task = service.snapshot().front();
    EXPECT_EQ(task.retries, 0);
    EXPECT_FALSE(task.spec.httpRequest.bodyProvided);
    EXPECT_TRUE(task.spec.httpRequest.body.empty());
    service.resume(id);
    ASSERT_TRUE(waitAttention(service));
    EXPECT_EQ(calls->load(), 1);
    auto spec = task.spec;
    spec.httpRequest.body = "owned-private-post-body";
    spec.httpRequest.bodyProvided = true;
    service.updateRequestWithCredentials(id, spec, false).get();
    service.resume(id);
    for (int n = 0; n < 150 && calls->load() < 2; ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(waitAttention(service));
    EXPECT_EQ(calls->load(), 2);
}
TEST(HttpPostUi, RequiresExplicitConfirmationAndInvalidatesEditedRequest) {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char name[] = "HttpPostWidgets";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QTemporaryDir temp;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(name);
    AddDownloadDialog dialog(false);
    dialog.setSpecification(postSpec(std::filesystem::path(temp.path().toStdWString()) / "result.bin"));
    auto* confirmation = dialog.findChild<QCheckBox*>("postConfirmed");
    ASSERT_NE(confirmation, nullptr);
    EXPECT_TRUE(dialog.specification().httpRequest.bodyProvided);
    dialog.findChild<QPlainTextEdit*>("postBody")->setPlainText("edited-body");
    EXPECT_FALSE(dialog.specification().httpRequest.bodyProvided);
    confirmation->setChecked(true);
    dialog.findChild<QLineEdit*>("downloadUrl")->setText("http://127.0.0.1:9/new-source");
    EXPECT_FALSE(dialog.specification().httpRequest.bodyProvided);
    confirmation->setChecked(true);
    dialog.findChild<QComboBox*>("postContentType")->setCurrentText("text/plain");
    EXPECT_FALSE(dialog.specification().httpRequest.bodyProvided);
    dialog.findChild<QLineEdit*>("downloadUrl")->setText("ftp://example.test/file");
    EXPECT_EQ(dialog.specification().httpRequest.method, HttpMethod::Get);
    EXPECT_TRUE(dialog.findChild<QPlainTextEdit*>("postBody")->toPlainText().isEmpty());
    dialog.findChild<QLineEdit*>("downloadUrl")->setText("http://127.0.0.1:9/owned");
    dialog.findChild<QComboBox*>("httpMethod")->setCurrentIndex(1);
    confirmation->setChecked(true);
    EXPECT_TRUE(dialog.specification().httpRequest.body.empty());
    EXPECT_TRUE(dialog.specification().httpRequest.bodyProvided);
    auto* tabs = dialog.findChild<QTabWidget*>("downloadOptionsTabs");
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i) == "Request")
            tabs->setCurrentIndex(i);
    dialog.show();
    QCoreApplication::processEvents();
    if (qEnvironmentVariableIsSet("MASKPILOT_TEST_POST_SCREENSHOT"))
        EXPECT_TRUE(dialog.grab().save(qEnvironmentVariable("MASKPILOT_TEST_POST_SCREENSHOT")));

    // Feed the actual dialog specification through the coordinator and native HTTP transport.
    QTcpServer origin;
    ASSERT_TRUE(origin.listen(QHostAddress::LocalHost, 0));
    int requests = 0;
    QByteArray received;
    QObject::connect(&origin, &QTcpServer::newConnection, &app, [&] {
        while (auto* socket = origin.nextPendingConnection()) {
            auto pending = std::make_shared<QByteArray>();
            auto answered = std::make_shared<bool>(false);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, pending, answered] {
                pending->append(socket->readAll());
                const auto end = pending->indexOf("\r\n\r\n");
                if (*answered || end < 0)
                    return;
                qsizetype length = 0;
                for (const auto& line : pending->left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:"))
                        length = line.mid(15).trimmed().toLongLong();
                if (pending->size() < end + 4 + length)
                    return;
                *answered = true;
                ++requests;
                received = *pending;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 9\r\nContent-Type: application/octet-stream"
                              "\r\nConnection: close\r\n\r\nqt-result");
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    auto spec = postSpec(std::filesystem::path(temp.path().toStdWString()) / "qt-result.bin");
    spec.url = "http://127.0.0.1:" + std::to_string(origin.serverPort()) + "/export";
    dialog.setSpecification(spec);
    const auto root = std::filesystem::path(temp.path().toStdWString());
    DownloadService service(makeSqliteRepository(root / "qt-tasks.db"), makeCurlTransfer, root / "work");
    const auto id = service.add(dialog.specification()).get();
    Task task;
    for (int n = 0; n < 300; ++n) {
        QCoreApplication::processEvents();
        task = service.snapshot().front();
        if (isTerminal(task.state) || task.state == TaskState::NeedsAttention)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(task.id, id);
    EXPECT_EQ(task.state, TaskState::Completed) << task.error;
    EXPECT_EQ(requests, 1);
    EXPECT_TRUE(received.startsWith("POST /export HTTP/1.1\r\n"));
    EXPECT_TRUE(received.endsWith("owned-private-post-body"));
    EXPECT_FALSE(received.toLower().contains("\r\nrange:"));
    QFile result(QString::fromStdWString(spec.destination.wstring()));
    ASSERT_TRUE(result.open(QIODevice::ReadOnly));
    EXPECT_EQ(result.readAll(), "qt-result");
}

TEST(HttpPost, MigratesSchema15WithBackupAndRejectsMalformedSavedMethod) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto root = std::filesystem::path(temp.path().toStdWString());
    const auto path = root / "tasks.db";
    auto repository = makeSqliteRepository(path);
    Task task;
    task.id = newTaskId();
    task.spec.url = "https://example.test/file";
    task.spec.destination = root / "result.bin";
    repository->save(task);
    repository.reset();
    sqlite3* database = nullptr;
    const auto bytes = path.u8string();
    ASSERT_EQ(sqlite3_open(reinterpret_cast<const char*>(bytes.c_str()), &database), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(database,
                           "ALTER TABLE tasks DROP COLUMN http_method;"
                           "ALTER TABLE tasks DROP COLUMN post_content_type;"
                           "ALTER TABLE tasks DROP COLUMN live_format;"
                           "ALTER TABLE tasks DROP COLUMN live_duration;"
                           "ALTER TABLE tasks DROP COLUMN live_max_bytes;"
                           "ALTER TABLE tasks DROP COLUMN sftp_authentication;"
                           "ALTER TABLE tasks DROP COLUMN sftp_host_key_sha256;"
                           "ALTER TABLE tasks DROP COLUMN sftp_private_key; PRAGMA user_version=15;",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(database);
    repository = makeSqliteRepository(path);
    ASSERT_EQ(repository->load().size(), 1);
    EXPECT_EQ(repository->load().front().spec.httpRequest.method, HttpMethod::Get);
    bool backup = false;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        backup |= entry.path().filename().string().find(".before-v16-") != std::string::npos;
    EXPECT_TRUE(backup);
    ASSERT_EQ(sqlite3_open(reinterpret_cast<const char*>(bytes.c_str()), &database), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(database,
                           "PRAGMA ignore_check_constraints=ON;"
                           "UPDATE tasks SET http_method='POST';",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(database);
    EXPECT_THROW(repository->load(), std::exception);
}
