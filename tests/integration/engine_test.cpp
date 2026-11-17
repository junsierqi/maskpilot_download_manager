#include <QProcess>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
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
    t.createdAt = maskpilot::unixTime();
    repo->save(t);
    auto loaded = repo->load();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded[0].spec.destination, t.spec.destination);
    EXPECT_TRUE(loaded[0].spec.cookie.empty());
    EXPECT_TRUE(loaded[0].spec.authorization.empty());
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
