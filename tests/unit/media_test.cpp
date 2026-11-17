#include <QDir>
#include <QTemporaryDir>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/media_tools.h>
#include <sqlite3.h>

TEST(MediaArguments, IsolatesSecretsAndPassesUrlAsData) {
    maskpilot::Task task;
    task.spec.url = "https://example.test/video?x=1&y=2";
    task.spec.kind = maskpilot::TransferKind::Media;
    task.spec.cookie = "private-cookie";
    task.spec.authorization = "secret-auth";
    task.spec.mediaMuxer = L"C:/Program Files/processor.exe";
    auto args = maskpilot::mediaArguments(task, std::filesystem::temp_directory_path(), 12345);
    EXPECT_FALSE(maskpilot::validateSpec(task.spec).empty());
    EXPECT_EQ(args.last().toStdString(), task.spec.url);
    EXPECT_EQ(args[args.size() - 2], "--");
    EXPECT_TRUE(args.contains("12345"));
    EXPECT_TRUE(args.contains("--ignore-config"));
    EXPECT_TRUE(args.contains("--no-continue"));
    EXPECT_TRUE(args.contains("--no-plugin-dirs"));
    EXPECT_FALSE(args.join(' ').contains("private-cookie"));
    EXPECT_FALSE(args.join(' ').contains("secret-auth"));
}
TEST(MediaArguments, AudioAndVideoHaveExplicitContainers) {
    maskpilot::Task task;
    task.spec.url = "https://example.test/video";
    task.spec.kind = maskpilot::TransferKind::Media;
    task.spec.audioOnly = true;
    auto args = maskpilot::mediaArguments(task, std::filesystem::temp_directory_path(), 0);
    EXPECT_TRUE(args.contains("--extract-audio"));
    EXPECT_TRUE(args.contains("m4a"));
    EXPECT_FALSE(args.contains("--remux-video"));
    task.spec.audioOnly = false;
    args = maskpilot::mediaArguments(task, std::filesystem::temp_directory_path(), 0);
    EXPECT_TRUE(args.contains("--remux-video"));
    EXPECT_TRUE(args.contains("mkv"));
}
TEST(Migration, PreservesVersionOneTasksAndCreatesBackup) {
    auto folder = std::filesystem::temp_directory_path() / ("maskpilot-migration-" + maskpilot::newTaskId());
    std::filesystem::create_directories(folder);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{folder};
    auto database = folder / "tasks.db";
    sqlite3* raw = nullptr;
    auto path = database.u8string();
    ASSERT_EQ(sqlite3_open(reinterpret_cast<const char*>(path.c_str()), &raw), SQLITE_OK);
    const char* schema =
        "CREATE TABLE tasks(id TEXT PRIMARY KEY,url TEXT,destination TEXT,category TEXT,referrer "
        "TEXT,user_agent TEXT,proxy TEXT,expected_sha256 TEXT,connections INTEGER,priority "
        "INTEGER,max_retries INTEGER,speed_limit INTEGER,scheduled_at INTEGER,stop_at INTEGER,state "
        "INTEGER,downloaded INTEGER,total INTEGER,created_at INTEGER,updated_at INTEGER,retries "
        "INTEGER,resumable INTEGER,error TEXT); PRAGMA user_version=1; INSERT INTO tasks "
        "VALUES('0123456789abcdef0123456789abcdef','https://example.test/"
        "file','file.bin','General','','MaskPilot','','',4,0,3,0,0,0,5,10,100,1,1,0,1,'');";
    ASSERT_EQ(sqlite3_exec(raw, schema, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    {
        auto repository = maskpilot::makeSqliteRepository(database);
        auto tasks = repository->load();
        ASSERT_EQ(tasks.size(), 1);
        EXPECT_EQ(tasks[0].downloaded, 10);
        EXPECT_FALSE(tasks[0].spec.kind == maskpilot::TransferKind::Media);
        tasks[0].spec.kind = maskpilot::TransferKind::Media;
        tasks[0].spec.mediaFormat = "best";
        repository->save(tasks[0]);
        EXPECT_TRUE(repository->load()[0].spec.kind == maskpilot::TransferKind::Media);
    }
    int backups = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        if (entry.path().filename().string().find("before-v2") != std::string::npos)
            ++backups;
    EXPECT_EQ(backups, 1);
}
#ifdef _WIN32
// The child runs this test alone as a deterministic media helper. No external tools or network.
TEST(MediaPublication, HelperProcess) {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_MEDIA_HELPER"))
        return;
    const auto output = std::filesystem::current_path() / "media.mkv";
    {
        std::ofstream file(output, std::ios::binary);
        file << "fixture media bytes";
    }
    const auto utf8 = output.u8string();
    std::cout << "MASKPILOT_FILE:";
    std::cout.write(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    std::cout << std::endl;
}
TEST(MediaPublication, HonorsStopAtFinalizingBeforePublishing) {
    QTemporaryDir temporary(QDir::tempPath() + QString::fromUtf8("/maskpilot-media-媒体-XXXXXX"));
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    wchar_t executable[32768];
    const auto length = GetModuleFileNameW(nullptr, executable, std::size(executable));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, std::size(executable));
    struct Environment {
        QByteArray filter = qgetenv("GTEST_FILTER"), helper = qgetenv("MASKPILOT_TEST_MEDIA_HELPER");
        ~Environment() {
            for (const auto& [name, value] :
                 {std::pair{"GTEST_FILTER", filter}, std::pair{"MASKPILOT_TEST_MEDIA_HELPER", helper}})
                if (value.isNull())
                    qunsetenv(name);
                else
                    qputenv(name, value);
        }
    } environment;
    qputenv("GTEST_FILTER", "MediaPublication.HelperProcess");
    qputenv("MASKPILOT_TEST_MEDIA_HELPER", "1");
    for (const bool cancel : {false, true}) {
        maskpilot::Task task;
        task.id = maskpilot::newTaskId();
        task.spec.url = "https://example.test/media";
        task.spec.destination = root / (cancel ? "cancel.mkv" : "pause.mkv");
        task.spec.kind = maskpilot::TransferKind::Media;
        task.spec.mediaDownloader = std::filesystem::path(executable, executable + length);
        task.spec.mediaMuxer = task.spec.mediaDownloader;
        maskpilot::TransferControl control;
        auto outcome = maskpilot::TransferOutcome::Progress;
        bool reachedFinalizing = false;
        maskpilot::makeUnifiedTransfer()->run(task, root / task.id, control, [&](auto event) {
            if (event.outcome == maskpilot::TransferOutcome::Progress &&
                event.phase == maskpilot::TaskState::Finalizing) {
                reachedFinalizing = true;
                if (cancel)
                    control.cancel = true;
                else
                    control.pause = true;
            }
            if (event.outcome != maskpilot::TransferOutcome::Progress)
                outcome = event.outcome;
        });
        EXPECT_TRUE(reachedFinalizing);
        EXPECT_EQ(outcome,
                  cancel ? maskpilot::TransferOutcome::Cancelled : maskpilot::TransferOutcome::Paused);
        EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
    }
}
#endif
