#include "../../src/infrastructure/src/network/curl_runtime.h"
#include "../../src/ui/src/models/task_list_codec.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <curl/curl.h>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/ui/add_download_dialog.h>
#include <sqlite3.h>
#include <thread>

using namespace maskpilot;
namespace {
DownloadSpec sshSpec(const std::filesystem::path& destination) {
    DownloadSpec spec;
    spec.url = "sftp://example.test/file.bin";
    spec.kind = TransferKind::Sftp;
    spec.destination = destination;
    spec.sftp.hostKeySha256 = "SHA256:" + std::string(43, 'A');
    spec.sftp.username = "owned-sftp-user";
    spec.sftp.password = "owned-sftp-password-private";
    return spec;
}
class OwnedServer {
  public:
    QProcess process;
    explicit OwnedServer(const QString& metadata) {
        process.start(QString::fromUtf8(MASKPILOT_SFTP_TEST_PYTHON),
                      {"-u", QString::fromUtf8(MASKPILOT_SFTP_TEST_FIXTURE), "--metadata", metadata});
    }
    ~OwnedServer() {
        process.write("stop\n");
        process.closeWriteChannel();
        if (!process.waitForFinished(10000)) {
            process.kill(); // This exact test-owned server has threads, no child processes.
            process.waitForFinished(5000);
        }
    }
};
} // namespace
TEST(Sftp, RequiresCanonicalServerIdentityAndExplicitIsolatedAuthentication) {
    auto spec = sshSpec("result.bin");
    EXPECT_TRUE(validateSpec(spec).empty());
    EXPECT_EQ(sourceKind(spec), TransferKind::Sftp);
    EXPECT_TRUE(requiresOriginAuthentication(spec));
    EXPECT_EQ(canonicalSftpHostKey(spec.sftp.hostKeySha256), std::string(43, 'A') + '=');
    for (const auto* value : {"", "MD5:00:00", "SHA256:bad", "bad\r\nX: value"}) {
        spec.sftp.hostKeySha256 = value;
        EXPECT_FALSE(validateSpec(spec).empty());
    }
    spec = sshSpec("result.bin");
    spec.sftp.username.clear();
    EXPECT_FALSE(validateOriginAuthentication(spec, true).empty());
    spec = sshSpec("result.bin");
    spec.cookie = "must-not-cross-protocol";
    EXPECT_FALSE(validateSpec(spec).empty());
    spec.cookie.clear();
    spec.httpRequest.method = HttpMethod::Post;
    EXPECT_FALSE(validateSpec(spec).empty());
    spec.httpRequest = {};
    spec.sftp.authentication = SftpAuthentication::PrivateKey;
    EXPECT_FALSE(validateSpec(spec).empty());
    spec.sftp.password.clear();
    spec.sftp.privateKeyFile = "relative.pem";
    EXPECT_FALSE(validateSpec(spec).empty());
    spec.sftp.privateKeyFile = std::filesystem::temp_directory_path() / "owned-key.pem";
    EXPECT_TRUE(validateSpec(spec).empty());
    spec.sftp.authentication = static_cast<SftpAuthentication>(2);
    EXPECT_FALSE(validateSpec(spec).empty());
    spec = sshSpec("result.bin");
    spec.url = "https://example.test/file";
    EXPECT_FALSE(validateSpec(spec).empty());
}
TEST(Sftp, PersistsOnlyPublicConfigurationAndRejectsCorruptSavedMode) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    const auto path = root / "tasks.db";
    auto repository = makeSqliteRepository(path);
    Task task;
    task.id = newTaskId();
    task.spec = sshSpec(root / "result.bin");
    task.spec.sftp.authentication = SftpAuthentication::PrivateKey;
    task.spec.sftp.password.clear();
    task.spec.sftp.privateKeyFile = root / "owned-private-key.pem";
    task.spec.sftp.keyPassphrase = "owned-sftp-key-passphrase-private";
    repository->save(task);
    auto saved = repository->load();
    ASSERT_EQ(saved.size(), 1);
    EXPECT_EQ(saved[0].spec.kind, TransferKind::Sftp);
    EXPECT_TRUE(saved[0].originAuthenticationRequired);
    EXPECT_TRUE(saved[0].spec.sftp.username.empty());
    EXPECT_TRUE(saved[0].spec.sftp.password.empty());
    EXPECT_TRUE(saved[0].spec.sftp.keyPassphrase.empty());
    EXPECT_EQ(saved[0].spec.sftp.privateKeyFile, task.spec.sftp.privateKeyFile);
    const auto encoded = ui::encodeTaskList({task});
    for (const auto& secret : {task.spec.sftp.username, task.spec.sftp.keyPassphrase})
        EXPECT_FALSE(encoded.contains(QByteArray::fromStdString(secret)));
    const auto decoded = ui::decodeTaskList(encoded, [](const QString&) { return DownloadSpec{}; });
    ASSERT_EQ(decoded.downloads.size(), 1);
    EXPECT_EQ(decoded.downloads[0].kind, TransferKind::Sftp);
    EXPECT_TRUE(decoded.downloads[0].startPaused);
    EXPECT_TRUE(decoded.downloads[0].sftp.username.empty());
    auto document = QJsonDocument::fromJson(encoded).object();
    auto downloads = document["downloads"].toArray();
    auto entry = downloads[0].toObject();
    auto ssh = entry["sftp"].toObject();
    ssh["password"] = "injected";
    entry["sftp"] = ssh;
    downloads[0] = entry;
    document["downloads"] = downloads;
    EXPECT_THROW(
        ui::decodeTaskList(QJsonDocument(document).toJson(), [](const QString&) { return DownloadSpec{}; }),
        std::exception);
    task.ownerId = newTaskId();
    task.contextRevision = 1;
    EXPECT_THROW(repository->save(task), std::exception);
    sqlite3* database = nullptr;
    const auto bytes = path.u8string();
    ASSERT_EQ(sqlite3_open(reinterpret_cast<const char*>(bytes.c_str()), &database), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(database,
                           "PRAGMA ignore_check_constraints=ON;"
                           "UPDATE tasks SET sftp_authentication='key';",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(database);
    EXPECT_THROW(repository->load(), std::exception);
}
TEST(Sftp, NativeRuntimeRetainsWindowsTlsBackendAndProvidesSftp) {
    network::initializeCurl();
    const auto* version = curl_version_info(CURLVERSION_NOW);
#ifdef _WIN32
    ASSERT_NE(version->ssl_version, nullptr);
    // MultiSSL reports inactive backends in parentheses, before or after the selected backend.
    const auto active =
        QString::fromLatin1(version->ssl_version).remove(QRegularExpression("\\([^)]*\\)")).trimmed();
    EXPECT_EQ(active, "Schannel") << version->ssl_version;
#endif
    bool supported = false;
    for (const auto* protocol = version->protocols; protocol && *protocol; ++protocol)
        supported |= std::string_view(*protocol) == "sftp";
    EXPECT_TRUE(supported);
}
TEST(Sftp, MigratesActualSchema16WithOriginalBackupAndOrdinaryDefaults) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
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
                           "ALTER TABLE tasks DROP COLUMN media_origins;"
                           "ALTER TABLE tasks DROP COLUMN live_format;"
                           "ALTER TABLE tasks DROP COLUMN live_duration;"
                           "ALTER TABLE tasks DROP COLUMN live_max_bytes;"
                           "ALTER TABLE tasks DROP COLUMN sftp_authentication;"
                           "ALTER TABLE tasks DROP COLUMN sftp_host_key_sha256;"
                           "ALTER TABLE tasks DROP COLUMN sftp_private_key; PRAGMA user_version=16;",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(database);
    repository = makeSqliteRepository(path);
    const auto saved = repository->load();
    ASSERT_EQ(saved.size(), 1);
    EXPECT_EQ(saved[0].id, task.id);
    EXPECT_EQ(saved[0].spec.sftp.authentication, SftpAuthentication::Password);
    EXPECT_TRUE(saved[0].spec.sftp.hostKeySha256.empty());
    EXPECT_TRUE(saved[0].spec.sftp.privateKeyFile.empty());
    bool backup = false;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        backup |= entry.path().filename().string().find(".before-v17-") != std::string::npos;
    EXPECT_TRUE(backup);
}
TEST(SftpUi, ActualDialogDownloadAndNativePauseResumePreserveVerifiedOutput) {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char name[] = "SftpWidgets";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(name);
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    const auto metadata = temporary.filePath("fixture.json");
    OwnedServer fixture(metadata);
    ASSERT_TRUE(fixture.process.waitForStarted(5000));
    for (int n = 0; n < 500 && !QFile::exists(metadata); ++n) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    QFile source(metadata);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly)) << fixture.process.readAllStandardError().toStdString();
    const auto config = QJsonDocument::fromJson(source.readAll()).object();
    source.close();
    auto spec = sshSpec(root / L"result.bin");
    spec.url = config["url"].toString().toStdString();
    spec.sftp.hostKeySha256 = config["fingerprint"].toString().toStdString();
    spec.expectedSha256 = config["sha256"].toString().toStdString();
    AddDownloadDialog dialog(false);
    dialog.setSpecification(spec);
    ASSERT_EQ(dialog.specification().kind, TransferKind::Sftp);
    EXPECT_EQ(dialog.findChild<QLineEdit*>("sftpPassword")->echoMode(), QLineEdit::Password);
    auto* tabs = dialog.findChild<QTabWidget*>("downloadOptionsTabs");
    ASSERT_NE(tabs, nullptr);
    for (int n = 0; n < tabs->count(); ++n)
        if (tabs->tabText(n) == "SFTP")
            tabs->setCurrentIndex(n);
    dialog.show();
    QCoreApplication::processEvents();
    if (qEnvironmentVariableIsSet("MASKPILOT_TEST_SFTP_SCREENSHOT"))
        EXPECT_TRUE(dialog.grab().save(qEnvironmentVariable("MASKPILOT_TEST_SFTP_SCREENSHOT")));
    {
        DownloadService service(makeSqliteRepository(root / "tasks.db"), makeUnifiedTransfer, root / "work");
        const auto id = service.add(dialog.specification()).get();
        Task task;
        for (int n = 0; n < 1500; ++n) {
            QCoreApplication::processEvents();
            task = service.snapshot().front();
            if (isTerminal(task.state) || task.state == TaskState::NeedsAttention)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_EQ(task.id, id);
        ASSERT_EQ(task.state, TaskState::Completed) << task.error;
        EXPECT_EQ(sha256File(spec.destination), spec.expectedSha256);
    }
    Task paused;
    paused.id = newTaskId();
    paused.spec = spec;
    paused.spec.destination = root / "resumed.bin";
    TransferControl control;
    TransferEvent result;
    makeSftpTransfer()->run(paused, root / "resume-work", control, [&](TransferEvent event) {
        result = event;
        if (event.outcome == TransferOutcome::Progress && event.downloaded >= 1024 * 1024)
            control.pause = true;
    });
    ASSERT_EQ(result.outcome, TransferOutcome::Paused) << result.error;
    const auto prefix = std::filesystem::file_size(root / "resume-work/part-0");
    ASSERT_GT(prefix, 0);
    ASSERT_LT(prefix, static_cast<std::uint64_t>(result.total));
    TransferControl resumed;
    bool reusedPrefix = false;
    makeSftpTransfer()->run(paused, root / "resume-work", resumed, [&](TransferEvent event) {
        reusedPrefix |= event.outcome == TransferOutcome::Progress && event.phase == TaskState::Downloading &&
                        event.downloaded == static_cast<std::int64_t>(prefix);
        result = event;
    });
    EXPECT_TRUE(reusedPrefix);
    EXPECT_EQ(result.outcome, TransferOutcome::Complete) << result.error;
    EXPECT_EQ(sha256File(paused.spec.destination), spec.expectedSha256);
    // The same native finalization path must preserve a file that appears during the transfer.
    paused.id = newTaskId();
    paused.spec.destination = root / "preserved.bin";
    TransferControl clobber;
    makeSftpTransfer()->run(paused, root / "clobber-work", clobber, [&](TransferEvent event) {
        result = event;
        if (event.phase == TaskState::Downloading && !std::filesystem::exists(paused.spec.destination))
            std::ofstream(paused.spec.destination, std::ios::binary) << "preserve";
    });
    EXPECT_EQ(result.outcome, TransferOutcome::Attention);
    QFile final(QString::fromStdWString(paused.spec.destination.wstring()));
    ASSERT_TRUE(final.open(QIODevice::ReadOnly));
    EXPECT_EQ(final.readAll(), "preserve");
    dialog.findChild<QLineEdit*>("downloadUrl")->setText("https://example.test/file");
    EXPECT_TRUE(dialog.findChild<QLineEdit*>("sftpPassword")->text().isEmpty());
    EXPECT_TRUE(dialog.specification().sftp.username.empty());
}
