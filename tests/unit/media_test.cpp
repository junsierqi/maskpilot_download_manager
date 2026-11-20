#include "../../src/infrastructure/src/media/media_process.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

TEST(MediaBoundary, UnresolvedOriginRequirementNeverStartsAHelper) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    for (const bool taskMarker : {false, true}) {
        maskpilot::Task task;
        task.id = maskpilot::newTaskId();
        task.spec.kind = maskpilot::TransferKind::Media;
        task.spec.url = "https://example.invalid/private";
        task.spec.destination = std::filesystem::path(temporary.path().toStdWString()) / "never-created.mkv";
        task.originAuthenticationRequired = taskMarker;
        task.spec.originAuthenticationRequired = !taskMarker;
        maskpilot::TransferControl control;
        int terminals = 0;
        maskpilot::makeMediaTransfer()->run(
            task, task.spec.destination.parent_path() / task.id, control, [&](const auto& event) {
                if (event.outcome != maskpilot::TransferOutcome::Progress) {
                    ++terminals;
                    EXPECT_EQ(event.outcome, maskpilot::TransferOutcome::Attention);
                    EXPECT_NE(event.error.find("Origin authentication"), std::string::npos);
                }
            });
        EXPECT_EQ(terminals, 1);
        EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
        EXPECT_FALSE(std::filesystem::exists(task.spec.destination.parent_path() / task.id));
    }
}

TEST(MediaArguments, IsolatesSecretsAndPassesUrlPrivately) {
    maskpilot::Task task;
    task.spec.url = "https://example.test/video?x=1&y=2";
    task.spec.kind = maskpilot::TransferKind::Media;
    task.spec.cookie = "private-cookie";
    task.spec.authorization = "secret-auth";
    task.spec.mediaMuxer = L"C:/Program Files/processor.exe";
    auto args = maskpilot::mediaArguments(task, std::filesystem::temp_directory_path(), 12345);
    task.spec.destination = std::filesystem::temp_directory_path() / L"authenticated-media.mkv";
    EXPECT_TRUE(maskpilot::validateSpec(task.spec).empty());
    EXPECT_FALSE(args.contains(QString::fromStdString(task.spec.url)));
    EXPECT_TRUE(args.contains("--config-locations"));
    EXPECT_TRUE(args.contains("ffmpeg_i:-protocol_whitelist pipe"));
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
TEST(MediaProxy, RequiresPrivateCredentialsBeforeLaunchingAnyHelper) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    maskpilot::Task task;
    task.id = maskpilot::newTaskId();
    task.spec.kind = maskpilot::TransferKind::Media;
    task.spec.url = "https://example.test/video";
    task.spec.destination = std::filesystem::path(temporary.filePath("video.mkv").toStdWString());
    task.spec.proxy = "http://127.0.0.1:1080";
    task.spec.proxyAuthenticationRequired = true;
    const auto args = maskpilot::mediaArguments(task, std::filesystem::temp_directory_path(), 0);
    EXPECT_FALSE(args.join(' ').contains("media-private-user"));
    EXPECT_FALSE(args.join(' ').contains("media-private-password"));
    maskpilot::TransferControl control;
    int terminals = 0;
    maskpilot::makeMediaTransfer()->run(
        task, std::filesystem::path(temporary.path().toStdWString()) / "work", control,
        [&](const auto& event) {
            if (event.outcome == maskpilot::TransferOutcome::Progress)
                return;
            ++terminals;
            EXPECT_EQ(event.outcome, maskpilot::TransferOutcome::Attention);
            EXPECT_NE(event.error.find("Proxy authentication"), std::string::npos);
            if (!task.spec.proxyPassword.empty())
                EXPECT_EQ(event.error.find(task.spec.proxyPassword), std::string::npos);
        });
    EXPECT_EQ(terminals, 1);
    task.spec.proxyUsername = "nonempty-proxy-user";
    task.spec.proxyPassword = "nonempty-proxy-secret";
    maskpilot::makeMediaTransfer()->run(
        task, std::filesystem::path(temporary.path().toStdWString()) / "work", control,
        [&](const auto& event) {
            if (event.outcome == maskpilot::TransferOutcome::Progress)
                return;
            ++terminals;
            EXPECT_EQ(event.outcome, maskpilot::TransferOutcome::Attention);
            EXPECT_EQ(event.error.find(task.spec.proxyPassword), std::string::npos);
            EXPECT_EQ(event.error.find(task.spec.proxyUsername), std::string::npos);
        });
    EXPECT_EQ(terminals, 2);
    EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
    EXPECT_TRUE(QDir(temporary.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
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

TEST(MediaPrivateInput, EncodesProxyCredentialsAndQuotesUrlWithoutArgvOrPlaintextFiles) {
    maskpilot::Task task;
    task.spec.kind = maskpilot::TransferKind::Media;
    task.spec.url = "https://example.test/watch?token=a'b&next=%22x%22";
    task.spec.destination = std::filesystem::temp_directory_path() / L"media.mkv";
    task.spec.proxy = "socks5://127.0.0.1:1080";
    task.spec.proxyUsername = "user name@x";
    task.spec.proxyPassword = "pass:#%?";
    task.spec.proxyAuthenticationRequired = true;
    task.spec.userAgent = "Agent 'quoted'";
    auto input = maskpilot::media_process::privateInput(task.spec);
    EXPECT_TRUE(input.contains("socks5h://user%20name%40x:pass%3A%23%25%3F@127.0.0.1:1080"));
    EXPECT_TRUE(input.contains("'\\''"));
    EXPECT_FALSE(input.contains("\n--\n"));
    EXPECT_TRUE(input.endsWith("\n"));
    auto args = maskpilot::mediaArguments(task, std::filesystem::temp_directory_path(), 0).join(' ');
    EXPECT_FALSE(args.contains("user name@x"));
    EXPECT_FALSE(args.contains("pass:#%?"));
    EXPECT_FALSE(args.contains("token="));
    EXPECT_FALSE(args.contains("Agent"));
    task.spec.proxy.clear();
    task.spec.proxyUsername.clear();
    task.spec.proxyPassword.clear();
    task.spec.proxyAuthenticationRequired = false;
    EXPECT_TRUE(maskpilot::media_process::privateInput(task.spec).contains("--proxy ''"));
}
namespace {
QJsonObject inventoryFormat(QString id, QString video = "avc1.64001f", QString audio = "mp4a.40.2",
                            QString protocol = "https") {
    return {{"format_id", id},
            {"ext", "mp4"},
            {"vcodec", video},
            {"acodec", audio},
            {"protocol", protocol},
            {"url", "https://private.invalid/?token=never-return"},
            {"http_headers", QJsonObject{{"Cookie", "never-return"}}},
            {"width", 1920},
            {"height", 1080},
            {"fps", 30.0},
            {"filesize", 1234}};
}
QByteArray inventoryJson(const QJsonArray& formats) {
    return QJsonDocument(QJsonObject{{"title", "Sample title"},
                                     {"formats", formats},
                                     {"webpage_url", "https://private.invalid/?token=never-return"}})
        .toJson();
}
} // namespace
TEST(MediaInventory, KeepsOnlyTypedMetadataAndFiltersExternalProtocolsAndDrm) {
    auto valid = inventoryFormat("muxed"), external = inventoryFormat("rtmp", "avc1", "aac", "rtmp"),
         protectedFormat = inventoryFormat("encrypted");
    protectedFormat["has_drm"] = true;
    auto result =
        maskpilot::media_process::parseInventory(inventoryJson(QJsonArray{valid, external, protectedFormat}));
    ASSERT_EQ(result.formats.size(), 1);
    EXPECT_EQ(result.formats.front().id, "muxed");
    EXPECT_EQ(result.formats.front().bytes, 1234);
    EXPECT_EQ(result.title, "Sample title");
    EXPECT_TRUE(result.formats.front().hasVideo);
    EXPECT_TRUE(result.formats.front().hasAudio);
}
TEST(MediaInventory, RejectsAmbiguousIdsUnsafeSelectorsAndMalformedNumbers) {
    auto format = inventoryFormat("one");
    EXPECT_THROW(maskpilot::media_process::parseInventory(inventoryJson(QJsonArray{format, format})),
                 std::runtime_error);
    format["format_id"] = "best/other";
    EXPECT_THROW(maskpilot::media_process::parseInventory(inventoryJson(QJsonArray{format})),
                 std::runtime_error);
    format = inventoryFormat("one");
    format["filesize"] = 9007199254740992.0;
    EXPECT_THROW(maskpilot::media_process::parseInventory(inventoryJson(QJsonArray{format})),
                 std::runtime_error);
    format["filesize"] = 1.5;
    EXPECT_THROW(maskpilot::media_process::parseInventory(inventoryJson(QJsonArray{format})),
                 std::runtime_error);
    format["filesize"] = "123";
    EXPECT_THROW(maskpilot::media_process::parseInventory(inventoryJson(QJsonArray{format})),
                 std::runtime_error);
}
TEST(MediaInventory, RejectsLivePlaylistsAndBoundedOutputOverflow) {
    auto object = QJsonDocument::fromJson(inventoryJson(QJsonArray{inventoryFormat("one")})).object();
    object["is_live"] = true;
    EXPECT_THROW(maskpilot::media_process::parseInventory(QJsonDocument(object).toJson()),
                 std::runtime_error);
    object["is_live"] = false;
    object["entries"] = QJsonArray{};
    EXPECT_THROW(maskpilot::media_process::parseInventory(QJsonDocument(object).toJson()),
                 std::runtime_error);
    EXPECT_THROW(maskpilot::media_process::parseInventory(QByteArray(8 * 1024 * 1024 + 1, ' ')),
                 std::runtime_error);
    QJsonArray many;
    for (int i = 0; i < 2001; ++i)
        many.push_back(inventoryFormat(QString::number(i)));
    EXPECT_THROW(maskpilot::media_process::parseInventory(inventoryJson(many)), std::runtime_error);
}
TEST(MediaArguments, ExplicitModesUseOnlySelectedIdentifiersAndRequestedContainers) {
    maskpilot::Task task;
    task.spec.mediaSelection = {maskpilot::MediaSelectionMode::VideoAudio, "video-1080", "audio-aac",
                                maskpilot::MediaContainer::Mp4};
    auto args = maskpilot::mediaArguments(task, std::filesystem::temp_directory_path(), 0);
    EXPECT_EQ(args.value(args.indexOf("--format") + 1),
              "best*[format_id='video-1080']+best*[format_id='audio-aac']");
    EXPECT_EQ(args.value(args.indexOf("--remux-video") + 1), "mp4");
    task.spec.mediaSelection = {
        maskpilot::MediaSelectionMode::VideoOnly, "video-720", {}, maskpilot::MediaContainer::Webm};
    args = maskpilot::mediaArguments(task, std::filesystem::temp_directory_path(), 0);
    EXPECT_EQ(args.value(args.indexOf("--format") + 1), "best*[format_id='video-720']");
    EXPECT_FALSE(args.contains("--extract-audio"));
    task.spec.mediaSelection = {
        maskpilot::MediaSelectionMode::AudioOnly, "combined", {}, maskpilot::MediaContainer::Mp3};
    args = maskpilot::mediaArguments(task, std::filesystem::temp_directory_path(), 0);
    EXPECT_EQ(args.value(args.indexOf("--format") + 1), "best*[format_id='combined']");
    EXPECT_EQ(args.value(args.indexOf("--audio-format") + 1), "mp3");
}
