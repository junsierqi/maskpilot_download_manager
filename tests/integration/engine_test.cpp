#include <QProcess>
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/media_tools.h>
#include <memory>
#include <sqlite3.h>
#include <thread>

namespace {
struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 std::filesystem::path("maskpilot-test-" + maskpilot::newTaskId());
    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace
TEST(Storage, PersistsTaskButNeverAuthenticationSecrets) {
    TempDirectory temp;
    auto repo = maskpilot::makeSqliteRepository(temp.path / "tasks.db");
    maskpilot::Task t;
    t.id = maskpilot::newTaskId();
    t.spec.url = "https://example.test/file";
    t.spec.destination = temp.path / "中文.bin";
    t.spec.cookie = "private-cookie";
    t.spec.authorization = "secret-token";
    t.spec.proxy = "http://proxy.example.test:8080";
    t.spec.proxyUsername = "private-proxy-user";
    t.spec.proxyPassword = "private-proxy-password";
    t.createdAt = maskpilot::unixTime();
    repo->save(t);
    auto loaded = repo->load();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded[0].spec.destination, t.spec.destination);
    EXPECT_TRUE(loaded[0].spec.cookie.empty());
    EXPECT_TRUE(loaded[0].spec.authorization.empty());
    EXPECT_TRUE(loaded[0].originAuthenticationRequired);
    EXPECT_TRUE(loaded[0].spec.originAuthenticationRequired);
    EXPECT_TRUE(loaded[0].spec.proxyUsername.empty());
    EXPECT_TRUE(loaded[0].spec.proxyPassword.empty());
    EXPECT_TRUE(loaded[0].spec.proxyAuthenticationRequired);
    EXPECT_EQ(loaded[0].spec.proxy, t.spec.proxy);
    for (const auto& file : std::filesystem::directory_iterator(temp.path)) {
        if (!file.is_regular_file())
            continue;
        std::ifstream input(file.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        EXPECT_EQ(bytes.find(t.spec.proxyUsername), std::string::npos);
        EXPECT_EQ(bytes.find(t.spec.proxyPassword), std::string::npos);
    }
    repo->erase(t.id);
    EXPECT_TRUE(repo->load().empty());
}
TEST(Checksums, UsesKnownSha256Vector) {
    TempDirectory temp;
    auto path = temp.path / "abc";
    {
        std::ofstream f(path);
        f << "abc";
    }
    EXPECT_EQ(maskpilot::sha256File(path),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
TEST(Storage, SourceRequirementMirrorsTaskSpecAndProcessMaterialWithoutPersistingSecrets) {
    TempDirectory temporary;
    const auto path = temporary.path / "tasks.db";
    std::vector<maskpilot::Task> originals;
    {
        auto repository = maskpilot::makeSqliteRepository(path);
        for (int source = 0; source < 6; ++source) {
            maskpilot::Task task;
            task.id = maskpilot::newTaskId();
            task.spec.url = source == 3 ? "ftp://example.test/file" : "https://example.test/file";
            task.spec.destination = temporary.path / task.id;
            if (source == 0)
                task.spec.cookie = "storage-source-cookie-private";
            if (source == 1)
                task.spec.authorization = "storage-source-authorization-private";
            if (source == 2)
                task.spec.headers = {{"X-Api-Key", "storage-source-header-private"}};
            if (source == 3) {
                task.spec.ftpUsername = "storage-source-ftp-user-private";
                task.spec.ftpPassword = "storage-source-ftp-password-private";
            }
            if (source == 4)
                task.spec.originAuthenticationRequired = true;
            if (source == 5)
                task.originAuthenticationRequired = true;
            repository->save(task);
            originals.push_back(task);
        }
    }
    auto repository = maskpilot::makeSqliteRepository(path);
    const auto loaded = repository->load();
    ASSERT_EQ(loaded.size(), originals.size());
    for (const auto& task : loaded) {
        EXPECT_TRUE(task.originAuthenticationRequired);
        EXPECT_TRUE(task.spec.originAuthenticationRequired);
        EXPECT_FALSE(maskpilot::hasOriginMaterial(task.spec));
        EXPECT_TRUE(maskpilot::validateSpec(task.spec).empty());
        EXPECT_FALSE(maskpilot::validateOriginAuthentication(task.spec, true).empty());
    }
    for (const auto& file : std::filesystem::directory_iterator(temporary.path)) {
        if (!file.is_regular_file())
            continue;
        std::ifstream input(file.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        for (const auto* secret : {"storage-source-cookie-private", "storage-source-authorization-private",
                                   "storage-source-header-private", "storage-source-ftp-user-private",
                                   "storage-source-ftp-password-private"})
            EXPECT_EQ(bytes.find(secret), std::string::npos);
    }
}
TEST(Storage, MigratesVersionFourBeforeAddingProxyCredentialRequirement) {
    TempDirectory temporary;
    const auto database = temporary.path / "tasks.db";
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
    for (const auto* name : {"0001_initial.sql", "0002_media.sql", "0003_ftp.sql", "0004_torrents.sql"}) {
        std::ifstream source(root / "database" / "migrations" / name, std::ios::binary);
        ASSERT_TRUE(source.good()) << name;
        const std::string sql((std::istreambuf_iterator<char>(source)), {});
        ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << name;
    }
    ASSERT_EQ(sqlite3_exec(raw,
                           "INSERT INTO tasks(id,url,destination,category,referrer,user_agent,proxy,"
                           "expected_sha256,connections,priority,max_retries,speed_limit,scheduled_at,"
                           "stop_at,state,downloaded,total,created_at,updated_at,retries,resumable,error) "
                           "VALUES('0123456789abcdef0123456789abcdef','https://example.test/file',"
                           "'file.bin','General','','MaskPilot','http://proxy.test:8080','',4,0,3,0,0,0,"
                           "5,321,4096,1,1,0,1,'');",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    connection.reset();
    {
        auto repository = maskpilot::makeSqliteRepository(database);
        auto tasks = repository->load();
        ASSERT_EQ(tasks.size(), 1);
        EXPECT_EQ(tasks[0].downloaded, 321);
        EXPECT_EQ(tasks[0].state, maskpilot::TaskState::Paused);
        EXPECT_EQ(tasks[0].spec.proxy, "http://proxy.test:8080");
        EXPECT_FALSE(tasks[0].spec.proxyAuthenticationRequired);
        tasks[0].spec.proxyUsername = "migration-private-user";
        tasks[0].spec.proxyPassword = "migration-private-password";
        repository->save(tasks[0]);
    }
    {
        auto repository = maskpilot::makeSqliteRepository(database);
        const auto tasks = repository->load();
        ASSERT_EQ(tasks.size(), 1);
        EXPECT_TRUE(tasks[0].spec.proxyAuthenticationRequired);
        EXPECT_TRUE(tasks[0].spec.proxyUsername.empty());
        EXPECT_TRUE(tasks[0].spec.proxyPassword.empty());
        EXPECT_TRUE(maskpilot::validateSpec(tasks[0].spec).empty());
    }
    int backups = 0;
    for (const auto& file : std::filesystem::directory_iterator(temporary.path)) {
        if (file.path().filename().string().find(".before-v5-") == std::string::npos)
            continue;
        ++backups;
        sqlite3* backup = nullptr;
        ASSERT_EQ(sqlite3_open_v2(file.path().string().c_str(), &backup, SQLITE_OPEN_READONLY, nullptr),
                  SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> closed(backup, sqlite3_close);
        sqlite3_stmt* statement = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(backup, "PRAGMA user_version", -1, &statement, nullptr), SQLITE_OK);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> finalized(statement, sqlite3_finalize);
        ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(statement, 0), 4);
    }
    EXPECT_EQ(backups, 1);
}
TEST(Storage, PreservesEmbeddedNullsForValidation) {
    TempDirectory temp;
    auto repo = maskpilot::makeSqliteRepository(temp.path / "tasks.db");
    maskpilot::Task task;
    task.id = maskpilot::newTaskId();
    task.spec.url = std::string("https://example.test/file\0suffix", 32);
    auto destination = (temp.path / "file.bin").u8string();
    destination += std::u8string(u8"\0suffix", 7);
    task.spec.destination = std::filesystem::path(destination);
    repo->save(task);
    const auto loaded = repo->load();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded[0].spec.url, task.spec.url);
    EXPECT_EQ(loaded[0].spec.destination, task.spec.destination);
    EXPECT_FALSE(maskpilot::validateSpec(loaded[0].spec).empty());
    auto validUrl = loaded[0].spec;
    validUrl.url = "https://example.test/file";
    EXPECT_FALSE(maskpilot::validateSpec(validUrl).empty());
}
TEST(Storage, RejectsNoncanonicalTaskIdentities) {
    TempDirectory temp;
    auto repo = maskpilot::makeSqliteRepository(temp.path / "tasks.db");
    maskpilot::Task task;
    task.id = "ABCDEF0123456789ABCDEF0123456789";
    task.spec.url = "https://example.test/file";
    task.spec.destination = temp.path / "file.bin";
    repo->save(task);
    EXPECT_THROW(repo->load(), std::runtime_error);
}
TEST(Service, RejectsCaseVariantCacheOwners) {
    TempDirectory temp;
    maskpilot::DownloadService service(maskpilot::makeSqliteRepository(temp.path / "tasks.db"),
                                       maskpilot::makeCurlTransfer, temp.path / "parts");
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/file";
    spec.destination = temp.path / "file.bin";
    spec.startPaused = true;
    const std::string id = "abcdef0123456789abcdef0123456789";
    ASSERT_EQ(service.add(spec, id).get(), id);
    EXPECT_THROW(service.add(spec, "ABCDEF0123456789ABCDEF0123456789").get(), std::invalid_argument);
    EXPECT_EQ(service.snapshot().size(), 1);
}
TEST(Checksums, HonorsCancellation) {
    TempDirectory temp;
    auto path = temp.path / "abc";
    {
        std::ofstream f(path);
        f << "abc";
    }
    maskpilot::TransferControl control;
    control.cancel = true;
    EXPECT_THROW(maskpilot::sha256File(path, &control), std::runtime_error);
}
TEST(Service, DurableAcknowledgementAndIdempotentRequestIdentity) {
    TempDirectory temp;
    maskpilot::DownloadService service(maskpilot::makeSqliteRepository(temp.path / "tasks.db"),
                                       maskpilot::makeCurlTransfer, temp.path / "parts");
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/file";
    spec.destination = temp.path / "file";
    spec.startPaused = true;
    const auto request = maskpilot::newTaskId();
    EXPECT_EQ(service.add(spec, request).get(), request);
    EXPECT_EQ(service.add(spec, request).get(), request);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(service.snapshot().size(), 1);
}

TEST(Storage, PersistsFtpPolicyButNeverCredentials) {
    TempDirectory temp;
    auto repo = maskpilot::makeSqliteRepository(temp.path / "tasks.db");
    maskpilot::Task task;
    task.id = maskpilot::newTaskId();
    task.spec.url = "ftp://example.test/file.bin";
    task.spec.destination = temp.path / "file.bin";
    task.spec.ftpUsername = "fixture-private-ftp-user";
    task.spec.ftpPassword = "fixture-private-ftp-password";
    task.spec.ftpExplicitTls = true;
    task.spec.ftpActiveMode = true;
    task.spec.ftpCaBundle = temp.path / "ca.pem";
    repo->save(task);
    const auto loaded = repo->load();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_TRUE(loaded[0].spec.ftpUsername.empty());
    EXPECT_TRUE(loaded[0].spec.ftpPassword.empty());
    EXPECT_TRUE(loaded[0].spec.ftpExplicitTls);
    EXPECT_TRUE(loaded[0].spec.ftpActiveMode);
    EXPECT_EQ(loaded[0].spec.ftpCaBundle, task.spec.ftpCaBundle);
    for (const auto& file : std::filesystem::directory_iterator(temp.path)) {
        if (!file.is_regular_file())
            continue;
        std::ifstream input(file.path(), std::ios::binary);
        std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        EXPECT_EQ(bytes.find("fixture-private-ftp-user"), std::string::npos);
        EXPECT_EQ(bytes.find("fixture-private-ftp-password"), std::string::npos);
    }
}
TEST(FtpTransfer, PauseCancelAndPublicationRespectActualControlCallbacks) {
    QProcess fixture;
    const auto script =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "support" / "ftp_fixture.py";
    fixture.start("python", {QString::fromStdWString(script.wstring()), "--serve"});
    struct Cleanup {
        QProcess& process;
        ~Cleanup() {
            process.closeWriteChannel();
            if (!process.waitForFinished(3000)) {
                process.kill();
                process.waitForFinished(3000);
            }
        }
    } cleanup{fixture};
    ASSERT_TRUE(fixture.waitForStarted(10000));
    for (int n = 0; n < 100 && !fixture.canReadLine(); ++n)
        fixture.waitForReadyRead(100);
    const auto url = fixture.readLine().trimmed().toStdString();
    ASSERT_TRUE(url.starts_with("ftp://127.0.0.1:")) << fixture.readAllStandardError().toStdString();
    const std::string expected = "3b5013c594c83f64be0f38fb5a63c21f4e59141933c56b26d4dd2e480be6fddb";
    for (const auto phase : {maskpilot::TaskState::Downloading, maskpilot::TaskState::Finalizing}) {
        for (const bool cancel : {false, true}) {
            TempDirectory temporary;
            maskpilot::Task task;
            task.id = maskpilot::newTaskId();
            task.spec.url = url;
            task.spec.destination = temporary.path / "output.bin";
            task.spec.expectedSha256 = expected;
            maskpilot::TransferControl control;
            bool exercised = false;
            int terminalEvents = 0;
            auto outcome = maskpilot::TransferOutcome::Progress;
            maskpilot::makeFtpTransfer()->run(task, temporary.path / task.id, control, [&](auto event) {
                if (event.outcome == maskpilot::TransferOutcome::Progress && event.phase == phase &&
                    (phase != maskpilot::TaskState::Downloading || event.downloaded > 0)) {
                    exercised = true;
                    if (cancel)
                        control.cancel = true;
                    else
                        control.pause = true;
                }
                if (event.outcome != maskpilot::TransferOutcome::Progress) {
                    ++terminalEvents;
                    outcome = event.outcome;
                }
            });
            EXPECT_TRUE(exercised);
            EXPECT_EQ(terminalEvents, 1);
            EXPECT_EQ(outcome,
                      cancel ? maskpilot::TransferOutcome::Cancelled : maskpilot::TransferOutcome::Paused);
            EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
            if (!cancel) {
                control.pause = false;
                maskpilot::makeFtpTransfer()->run(task, temporary.path / task.id, control, [&](auto event) {
                    if (event.outcome != maskpilot::TransferOutcome::Progress)
                        outcome = event.outcome;
                });
                ASSERT_EQ(outcome, maskpilot::TransferOutcome::Complete);
                EXPECT_EQ(maskpilot::sha256File(task.spec.destination), expected);
            }
        }
    }
}

TEST(TaskControl, UpdatingProtocolClearsOriginCredentialsBeforeValidation) {
    TempDirectory temporary;
    maskpilot::DownloadService service(maskpilot::makeSqliteRepository(temporary.path / "tasks.db"),
                                       maskpilot::makeCurlTransfer, temporary.path / "parts");
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/file.bin";
    spec.destination = temporary.path / "file.bin";
    spec.cookie = "private-cookie";
    spec.authorization = "private-auth";
    spec.referrer = "https://example.test/page";
    spec.startPaused = true;
    const auto id = service.add(spec).get();
    service.updateUrl(id, "ftp://example.test/file.bin");
    for (int n = 0; n < 100 && service.snapshot()[0].spec.url != "ftp://example.test/file.bin"; ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto tasks = service.snapshot();
    ASSERT_EQ(tasks.size(), 1);
    EXPECT_EQ(tasks[0].spec.url, "ftp://example.test/file.bin");
    EXPECT_TRUE(tasks[0].spec.cookie.empty());
    EXPECT_TRUE(tasks[0].spec.authorization.empty());
    EXPECT_TRUE(tasks[0].spec.referrer.empty());
    EXPECT_TRUE(tasks[0].spec.ftpUsername.empty());
    EXPECT_TRUE(tasks[0].spec.ftpPassword.empty());
    auto ftp = tasks[0].spec;
    ftp.ftpUsername = "ftp-user";
    ftp.ftpPassword = "ftp-private-password";
    ftp.ftpExplicitTls = true;
    ftp.ftpActiveMode = true;
    ftp.ftpCaBundle = temporary.path / "ca.pem";
    service.updateRequest(id, ftp);
    service.updateUrl(id, "https://other.test/file.bin");
    for (int n = 0; n < 100 && service.snapshot()[0].spec.url != "https://other.test/file.bin"; ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto updated = service.snapshot()[0].spec;
    EXPECT_EQ(updated.url, "https://other.test/file.bin");
    EXPECT_TRUE(updated.ftpUsername.empty());
    EXPECT_TRUE(updated.ftpPassword.empty());
    EXPECT_FALSE(updated.ftpExplicitTls);
    EXPECT_FALSE(updated.ftpActiveMode);
    EXPECT_TRUE(updated.ftpCaBundle.empty());
}

TEST(Storage, PreservesBinaryTorrentMetadataSelectionAndPublishedState) {
    TempDirectory temporary;
    auto repository = maskpilot::makeSqliteRepository(temporary.path / "tasks.db");
    maskpilot::Task task;
    task.id = maskpilot::newTaskId();
    task.spec.url = "torrent:C:/missing-original.torrent";
    task.spec.kind = maskpilot::TransferKind::BitTorrent;
    task.spec.destination = temporary.path / "payload";
    task.spec.torrent.metadata =
        std::make_shared<const std::vector<char>>(std::initializer_list<char>{'d', char{0}, 'e'});
    task.spec.torrent.allFiles = false;
    task.spec.torrent.selectedFiles = {0, 2};
    task.spec.torrent.seedAfterDownload = true;
    task.spec.torrent.uploadLimit = 65536;
    task.state = maskpilot::TaskState::Seeding;
    task.torrentPublished = true;
    task.uploaded = 12345;
    repository->save(task);
    const auto loaded = repository->load();
    ASSERT_EQ(loaded.size(), 1);
    ASSERT_NE(loaded[0].spec.torrent.metadata, nullptr);
    EXPECT_EQ(*loaded[0].spec.torrent.metadata, *task.spec.torrent.metadata);
    EXPECT_EQ(loaded[0].spec.kind, maskpilot::TransferKind::BitTorrent);
    EXPECT_EQ(loaded[0].spec.torrent.selectedFiles, task.spec.torrent.selectedFiles);
    EXPECT_FALSE(loaded[0].spec.torrent.allFiles);
    EXPECT_TRUE(loaded[0].spec.torrent.seedAfterDownload);
    EXPECT_EQ(loaded[0].spec.torrent.uploadLimit, 65536);
    EXPECT_EQ(loaded[0].state, maskpilot::TaskState::Seeding);
    EXPECT_TRUE(loaded[0].torrentPublished);
    EXPECT_EQ(loaded[0].uploaded, 12345);
}
TEST(Storage, MigratesPreviousSchemaWithoutChangingExistingProtocolsOrFiles) {
    TempDirectory temporary;
    const auto database = temporary.path / "tasks.db";
    std::vector<maskpilot::Task> originals;
    {
        auto repository = maskpilot::makeSqliteRepository(database);
        for (const auto kind :
             {maskpilot::TransferKind::Http, maskpilot::TransferKind::Ftp, maskpilot::TransferKind::Media}) {
            maskpilot::Task task;
            task.id = maskpilot::newTaskId();
            task.spec.kind = kind;
            task.spec.url = kind == maskpilot::TransferKind::Ftp ? "ftps://example.test/file.bin"
                                                                 : "https://example.test/file.bin";
            task.spec.destination = temporary.path / task.id;
            task.spec.ftpExplicitTls = kind == maskpilot::TransferKind::Ftp;
            task.state = maskpilot::TaskState::Paused;
            task.downloaded = 123;
            task.createdAt = static_cast<std::int64_t>(originals.size());
            repository->save(task);
            originals.push_back(task);
        }
    }
    // Reconstruct the previous on-disk shape, retaining its real task rows and legacy media field.
    sqlite3* connection = nullptr;
    ASSERT_EQ(sqlite3_open(database.string().c_str(), &connection), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> close(connection, sqlite3_close);
    ASSERT_EQ(
        sqlite3_exec(
            connection,
            "DROP INDEX tasks_owner_request; DROP INDEX tasks_queue_state; DROP TABLE download_contexts; "
            "DROP TABLE credential_records; DROP TABLE store_metadata; DROP TABLE download_queues; "
            "DROP TABLE queue_traffic_runs; DROP TABLE queue_traffic_usage; DROP TABLE traffic_clock;",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    for (const auto* column : {"kind",
                               "bt_all",
                               "bt_selection",
                               "bt_seed",
                               "bt_upload_limit",
                               "bt_metadata",
                               "bt_published",
                               "bt_uploaded",
                               "proxy_auth_required",
                               "owner_id",
                               "request_id",
                               "context_revision",
                               "proxy_credential_ref",
                               "origin_auth_required",
                               "request_name",
                               "queue_id",
                               "run_intent",
                               "media_selection_mode",
                               "media_primary_format",
                               "media_audio_format",
                               "media_container",
                               "max_stored_bytes",
                               "same_origin_redirect_only",
                               "response_url",
                               "response_content_type",
                               "project_id",
                               "project_revision"}) {
        const auto sql = std::string("ALTER TABLE tasks DROP COLUMN ") + column;
        ASSERT_EQ(sqlite3_exec(connection, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }
    ASSERT_EQ(sqlite3_exec(connection, "PRAGMA user_version=3", nullptr, nullptr, nullptr), SQLITE_OK);
    close.reset();
    auto repository = maskpilot::makeSqliteRepository(database);
    const auto migrated = repository->load();
    ASSERT_EQ(migrated.size(), originals.size());
    for (std::size_t i = 0; i < migrated.size(); ++i) {
        EXPECT_EQ(migrated[i].id, originals[i].id);
        EXPECT_EQ(migrated[i].spec.kind, originals[i].spec.kind);
        EXPECT_EQ(migrated[i].spec.url, originals[i].spec.url);
        EXPECT_EQ(migrated[i].spec.destination, originals[i].spec.destination);
        EXPECT_EQ(migrated[i].spec.ftpExplicitTls, originals[i].spec.ftpExplicitTls);
        EXPECT_EQ(migrated[i].state, maskpilot::TaskState::Paused);
        EXPECT_EQ(migrated[i].downloaded, 123);
        EXPECT_FALSE(migrated[i].torrentPublished);
        EXPECT_EQ(migrated[i].spec.torrent.metadata, nullptr);
    }
    EXPECT_TRUE(std::any_of(std::filesystem::directory_iterator(temporary.path),
                            std::filesystem::directory_iterator(), [](const auto& file) {
                                return file.path().filename().string().find(".before-v4-") !=
                                       std::string::npos;
                            }));
}
TEST(Storage, MigratesVersionFiveToContextsWithoutMovingTaskFiles) {
    TempDirectory temporary;
    const auto path = temporary.path / "tasks.db";
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
    for (const auto* name :
         {"0001_initial.sql", "0002_media.sql", "0003_ftp.sql", "0004_torrents.sql", "0005_proxy.sql"}) {
        std::ifstream source(root / "database" / "migrations" / name, std::ios::binary);
        ASSERT_TRUE(source.good()) << name;
        const std::string sql((std::istreambuf_iterator<char>(source)), {});
        ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << name;
    }
    ASSERT_EQ(sqlite3_exec(raw,
                           "INSERT INTO tasks(id,url,destination,category,referrer,user_agent,proxy,"
                           "expected_sha256,connections,priority,max_retries,speed_limit,scheduled_at,"
                           "stop_at,state,downloaded,total,created_at,updated_at,retries,resumable,error,"
                           "proxy_auth_required) "
                           "VALUES('0123456789abcdef0123456789abcdef','https://example.test/file',"
                           "'unchanged.bin','General','','MaskPilot','http://proxy.test:8080','',4,0,3,0,0,0,"
                           "5,765,4096,1,1,0,1,'',1);",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    connection.reset();
    const auto part = temporary.path / "parts" / "0123456789abcdef0123456789abcdef" / "part-0";
    std::filesystem::create_directories(part.parent_path());
    {
        std::ofstream output(part, std::ios::binary);
        output << "durable existing bytes";
    }
    std::string installation;
    {
        auto store = maskpilot::makeSqliteStore(path);
        installation = store.contexts->installationId();
        EXPECT_TRUE(maskpilot::isOpaqueId(installation));
        EXPECT_TRUE(store.contexts->loadContexts().empty());
        EXPECT_TRUE(store.contexts->loadCredentialRecords().empty());
        const auto tasks = store.tasks->load();
        ASSERT_EQ(tasks.size(), 1);
        EXPECT_EQ(tasks[0].ownerId, "standalone");
        EXPECT_EQ(tasks[0].requestId, tasks[0].id);
        EXPECT_TRUE(tasks[0].requestName.empty());
        EXPECT_EQ(tasks[0].contextRevision, 0);
        EXPECT_TRUE(tasks[0].proxyCredentialRef.empty());
        EXPECT_FALSE(tasks[0].originAuthenticationRequired);
        EXPECT_TRUE(tasks[0].originGrantId.empty());
        EXPECT_EQ(tasks[0].downloaded, 765);
        EXPECT_EQ(tasks[0].spec.destination, "unchanged.bin");
        EXPECT_TRUE(tasks[0].spec.proxyAuthenticationRequired);
        store.tasks->save(tasks[0]);
    }
    {
        auto store = maskpilot::makeSqliteStore(path);
        EXPECT_EQ(store.contexts->installationId(), installation);
    }
    std::ifstream input(part, std::ios::binary);
    EXPECT_EQ(std::string((std::istreambuf_iterator<char>(input)), {}), "durable existing bytes");
    int backups = 0;
    for (const auto& file : std::filesystem::directory_iterator(temporary.path)) {
        if (file.path().filename().string().find(".before-v6-") == std::string::npos)
            continue;
        ++backups;
        sqlite3* backup = nullptr;
        ASSERT_EQ(sqlite3_open_v2(file.path().string().c_str(), &backup, SQLITE_OPEN_READONLY, nullptr),
                  SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> closed(backup, sqlite3_close);
        sqlite3_stmt* statement = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(backup, "PRAGMA user_version", -1, &statement, nullptr), SQLITE_OK);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> finalized(statement, sqlite3_finalize);
        ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(statement, 0), 5);
    }
    EXPECT_EQ(backups, 1);
}

TEST(Storage, ContextTaskMetadataIsDurableAndOwnerAndScopedRequestCannotBeReassigned) {
    TempDirectory temporary;
    const auto path = temporary.path / "tasks.db";
    auto store = maskpilot::makeSqliteStore(path);
    maskpilot::Task task;
    task.id = maskpilot::newTaskId();
    task.ownerId = maskpilot::newTaskId();
    task.requestId = maskpilot::newTaskId();
    task.requestName = "requested.bin";
    task.contextRevision = 7;
    task.proxyCredentialRef = maskpilot::newTaskId();
    task.originAuthenticationRequired = true;
    task.originGrantId = "memory-only-origin-grant-private";
    task.spec.url = "https://example.test/file";
    task.spec.destination = temporary.path / "requested (1).bin";
    task.spec.cookie = "memory-only-context-cookie-private";
    task.spec.proxyPassword = "memory-only-context-proxy-private";
    store.tasks->save(task);
    auto reassigned = task;
    reassigned.ownerId = maskpilot::newTaskId();
    reassigned.spec.url = "https://different.test/file";
    EXPECT_THROW(store.tasks->save(reassigned), std::exception);
    auto duplicate = task;
    duplicate.id = maskpilot::newTaskId();
    EXPECT_THROW(store.tasks->save(duplicate), std::exception);
    duplicate.ownerId = reassigned.ownerId;
    EXPECT_NO_THROW(store.tasks->save(duplicate));
    // Empty request IDs intentionally do not become a shared uniqueness key.
    auto withoutRequest = task;
    withoutRequest.requestId.clear();
    withoutRequest.id = maskpilot::newTaskId();
    store.tasks->save(withoutRequest);
    withoutRequest.id = maskpilot::newTaskId();
    store.tasks->save(withoutRequest);
    store.tasks.reset();
    store.contexts.reset();
    auto reopened = maskpilot::makeSqliteStore(path);
    const auto tasks = reopened.tasks->load();
    ASSERT_EQ(tasks.size(), 4);
    const auto found =
        std::find_if(tasks.begin(), tasks.end(), [&](const auto& value) { return value.id == task.id; });
    ASSERT_NE(found, tasks.end());
    EXPECT_EQ(found->ownerId, task.ownerId);
    EXPECT_EQ(found->requestId, task.requestId);
    EXPECT_EQ(found->requestName, task.requestName);
    EXPECT_EQ(found->contextRevision, 7);
    EXPECT_EQ(found->proxyCredentialRef, task.proxyCredentialRef);
    EXPECT_TRUE(found->originAuthenticationRequired);
    EXPECT_EQ(found->spec.destination, task.spec.destination);
    EXPECT_EQ(found->spec.url, task.spec.url);
    EXPECT_TRUE(found->originGrantId.empty());
    EXPECT_TRUE(found->spec.cookie.empty());
    EXPECT_TRUE(found->spec.proxyPassword.empty());
    for (const auto& file : std::filesystem::directory_iterator(temporary.path)) {
        if (!file.is_regular_file())
            continue;
        std::ifstream input(file.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        for (const auto& secret : {task.originGrantId, task.spec.cookie, task.spec.proxyPassword})
            EXPECT_EQ(bytes.find(secret), std::string::npos);
    }
}

TEST(Storage, SharedContextFacadePersistsRevisionsAndCredentialOwnershipJournalBeforePolicy) {
    TempDirectory temporary;
    const auto path = temporary.path / "tasks.db";
    auto store = maskpilot::makeSqliteStore(path);
    const auto installation = store.contexts->installationId();
    maskpilot::DownloadContext context;
    context.id = maskpilot::newTaskId();
    context.externalKey = "trusted-browser/profile-a";
    context.label = "Context A";
    context.downloadRoot = temporary.path / "context-a";
    context.proxyEndpoint = "socks5://127.0.0.1:1080";
    context.proxyAuthenticationRequired = true;
    context.proxyCredentialRef = maskpilot::newTaskId();
    const maskpilot::CredentialRecord record{context.proxyCredentialRef, context.id, context.proxyEndpoint,
                                             maskpilot::newTaskId()};
    store.contexts->saveCredentialRecord(record);
    {
        // A separate connection must observe the journal before any native write or context reference.
        auto observer = maskpilot::makeSqliteStore(path);
        EXPECT_TRUE(observer.contexts->loadContexts().empty());
        const auto journal = observer.contexts->loadCredentialRecords();
        ASSERT_EQ(journal.size(), 1);
        EXPECT_EQ(journal[0].ownerId, context.id);
        EXPECT_EQ(journal[0].reference, record.reference);
        EXPECT_EQ(journal[0].ownershipNonce, record.ownershipNonce);
        EXPECT_EQ(observer.contexts->installationId(), installation);
    }
    store.contexts->saveCredentialRecord(record);
    auto foreignRecord = record;
    foreignRecord.ownerId = maskpilot::newTaskId();
    EXPECT_THROW(store.contexts->saveCredentialRecord(foreignRecord), std::exception);
    foreignRecord = record;
    foreignRecord.proxyEndpoint = "http://127.0.0.1:8080";
    EXPECT_THROW(store.contexts->saveCredentialRecord(foreignRecord), std::exception);
    foreignRecord = record;
    foreignRecord.ownershipNonce = maskpilot::newTaskId();
    EXPECT_THROW(store.contexts->saveCredentialRecord(foreignRecord), std::exception);
    foreignRecord.reference = maskpilot::newTaskId();
    foreignRecord.ownershipNonce = "invalid nonce";
    EXPECT_THROW(store.contexts->saveCredentialRecord(foreignRecord), std::exception);
    const maskpilot::CredentialRecord legacyRecord{maskpilot::newTaskId(), context.id, context.proxyEndpoint};
    store.contexts->saveCredentialRecord(legacyRecord);
    const auto journalWithLegacy = store.contexts->loadCredentialRecords();
    const auto legacy =
        std::find_if(journalWithLegacy.begin(), journalWithLegacy.end(),
                     [&](const auto& item) { return item.reference == legacyRecord.reference; });
    ASSERT_NE(legacy, journalWithLegacy.end());
    EXPECT_TRUE(legacy->ownershipNonce.empty());
    store.contexts->eraseCredentialRecord(legacyRecord.reference);
    store.contexts->saveContext(context);
    EXPECT_NO_THROW(store.contexts->saveContext(context));
    auto duplicate = context;
    duplicate.id = maskpilot::newTaskId();
    EXPECT_THROW(store.contexts->saveContext(duplicate), std::exception);
    auto invalidRevision = context;
    invalidRevision.label = "Unversioned edit";
    EXPECT_THROW(store.contexts->saveContext(invalidRevision), std::exception);
    invalidRevision.revision = 3;
    EXPECT_THROW(store.contexts->saveContext(invalidRevision), std::exception);
    ++context.revision;
    context.enabled = false;
    store.contexts->saveContext(context);
    invalidRevision = context;
    invalidRevision.revision = 1;
    EXPECT_THROW(store.contexts->saveContext(invalidRevision), std::exception);
    // Either facade owns the shared connection; destroying task facade must not close context storage.
    store.tasks.reset();
    EXPECT_EQ(store.contexts->installationId(), installation);
    store.contexts.reset();
    auto reopened = maskpilot::makeSqliteStore(path);
    const auto contexts = reopened.contexts->loadContexts();
    ASSERT_EQ(contexts.size(), 1);
    EXPECT_EQ(contexts[0].id, context.id);
    EXPECT_EQ(contexts[0].revision, 2);
    EXPECT_FALSE(contexts[0].enabled);
    EXPECT_EQ(contexts[0].proxyCredentialRef, record.reference);
    EXPECT_EQ(reopened.contexts->loadCredentialRecords().size(), 1);
    EXPECT_EQ(reopened.contexts->loadCredentialRecords()[0].ownershipNonce, record.ownershipNonce);
    reopened.contexts->eraseCredentialRecord(record.reference);
    EXPECT_TRUE(reopened.contexts->loadCredentialRecords().empty());
    EXPECT_NO_THROW(reopened.contexts->eraseCredentialRecord(record.reference));
    EXPECT_THROW(reopened.contexts->eraseCredentialRecord("../../foreign-target"), std::exception);
    reopened.contexts.reset();
    maskpilot::Task standalone;
    standalone.id = maskpilot::newTaskId();
    standalone.spec.url = "https://example.test/legacy-facade";
    standalone.spec.destination = temporary.path / "legacy.bin";
    reopened.tasks->save(standalone);
    EXPECT_EQ(reopened.tasks->load().size(), 1);
}

TEST(Service, PersistsTorrentPublicationBeforeSeedingStops) {
    struct PublishingTransport final : maskpilot::ITransferTransport {
        void run(const maskpilot::Task&, const std::filesystem::path&, maskpilot::TransferControl& control,
                 const std::function<void(maskpilot::TransferEvent)>& callback) override {
            maskpilot::TransferEvent event;
            event.phase = maskpilot::TaskState::Seeding;
            event.torrentPublished = true;
            callback(event);
            while (!control.pause && !control.cancel)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            event.outcome = maskpilot::TransferOutcome::Paused;
            callback(event);
        }
    };
    TempDirectory temporary;
    const auto database = temporary.path / "tasks.db";
    maskpilot::DownloadService service(
        maskpilot::makeSqliteRepository(database), [] { return std::make_unique<PublishingTransport>(); },
        temporary.path / "parts");
    maskpilot::DownloadSpec spec;
    spec.url = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";
    spec.destination = temporary.path / "payload";
    const auto id = service.add(spec).get();
    bool seeding = false;
    for (int n = 0; n < 200 && !seeding; ++n) {
        const auto tasks = service.snapshot();
        seeding = tasks.size() == 1 && tasks[0].state == maskpilot::TaskState::Seeding;
        if (!seeding)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(seeding);
    auto observer = maskpilot::makeSqliteRepository(database);
    const auto durable = observer->load();
    ASSERT_EQ(durable.size(), 1);
    EXPECT_TRUE(durable[0].torrentPublished);
    EXPECT_EQ(durable[0].state, maskpilot::TaskState::Seeding);
    service.pause(id);
}
TEST(Service, ImportPrevalidationRejectsBadMetadataBeforeCreatingAnyTask) {
    TempDirectory temporary;
    maskpilot::DownloadService service(maskpilot::makeSqliteRepository(temporary.path / "tasks.db"),
                                       maskpilot::makeUnifiedTransferFactory(), temporary.path / "parts");
    maskpilot::DownloadSpec file;
    file.url = "https://example.test/file.bin";
    file.destination = temporary.path / "file.bin";
    auto torrent = file;
    torrent.url = "torrent:C:/unused.torrent";
    torrent.kind = maskpilot::TransferKind::BitTorrent;
    torrent.torrent.metadata = std::make_shared<const std::vector<char>>(1, 'x');
    EXPECT_THROW(service.prepareImport({file, torrent}).get(), std::exception);
    EXPECT_TRUE(service.snapshot().empty());
}
