#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <sqlite3.h>

namespace {
struct MediaTemp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-media-store-" + maskpilot::newTaskId());
    MediaTemp() { std::filesystem::create_directories(path); }
    ~MediaTemp() {
        std::error_code error;
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().string().starts_with("maskpilot-media-store-"))
            std::filesystem::remove_all(path, error);
    }
};
} // namespace
TEST(MediaStorage, MigratesRealVersionNineWithoutChangingLegacyAudioPresetAndMakesBackup) {
    MediaTemp temporary;
    const auto path = temporary.path / "tasks.db";
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> close(raw, sqlite3_close);
    for (const auto* name : {"0001_initial.sql", "0002_media.sql", "0003_ftp.sql", "0004_torrents.sql",
                             "0005_proxy.sql", "0006_contexts.sql", "0007_queues.sql",
                             "0008_queue_windows.sql", "0009_native_payload_budgets.sql"}) {
        std::ifstream input(root / "database" / "migrations" / name);
        ASSERT_TRUE(input.good()) << name;
        const std::string sql((std::istreambuf_iterator<char>(input)), {});
        ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << name;
    }
    ASSERT_EQ(sqlite3_exec(raw,
                           "INSERT INTO "
                           "tasks(id,url,destination,category,referrer,user_agent,proxy,expected_sha256,"
                           "connections,priority,max_retries,"
                           "speed_limit,scheduled_at,stop_at,state,downloaded,total,created_at,updated_at,"
                           "retries,resumable,error,media,audio_only,media_format,kind) "
                           "VALUES('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','https://example.test/"
                           "media','legacy.m4a','Music','','MaskPilot','','',4,0,3,0,0,0,5,0,-1,1,1,0,0,'',1,"
                           "1,'bestaudio/best',2);",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    close.reset();
    {
        auto store = maskpilot::makeSqliteStore(path);
        const auto tasks = store.tasks->load();
        ASSERT_EQ(tasks.size(), 1);
        EXPECT_TRUE(tasks[0].spec.audioOnly);
        EXPECT_EQ(tasks[0].spec.mediaFormat, "bestaudio/best");
        EXPECT_EQ(tasks[0].spec.mediaSelection.mode, maskpilot::MediaSelectionMode::Automatic);
        EXPECT_TRUE(tasks[0].spec.mediaSelection.primaryFormat.empty());
        EXPECT_EQ(tasks[0].spec.destination, "legacy.m4a");
        EXPECT_TRUE(maskpilot::mediaAudioOnly(tasks[0].spec.mediaSelection, tasks[0].spec.audioOnly));
    }
    int backups = 0;
    for (const auto& file : std::filesystem::directory_iterator(temporary.path)) {
        if (file.path().filename().string().find(".before-v10-") == std::string::npos)
            continue;
        ++backups;
        sqlite3* backup = nullptr;
        ASSERT_EQ(sqlite3_open_v2(file.path().string().c_str(), &backup, SQLITE_OPEN_READONLY, nullptr),
                  SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> closed(backup, sqlite3_close);
        sqlite3_stmt* q = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(backup, "PRAGMA user_version", -1, &q, nullptr), SQLITE_OK);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> finalized(q, sqlite3_finalize);
        ASSERT_EQ(sqlite3_step(q), SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(q, 0), 9);
    }
    EXPECT_EQ(backups, 1);
}
TEST(MediaStorage, ExplicitSelectionSurvivesReopenAndInvalidMutationPreservesPriorChoice) {
    MediaTemp temporary;
    const auto path = temporary.path / "tasks.db";
    maskpilot::Task task;
    task.id = maskpilot::newTaskId();
    task.spec.kind = maskpilot::TransferKind::Media;
    task.spec.url = "https://example.test/media";
    task.spec.destination = temporary.path / "selected.mp4";
    task.spec.audioOnly = true;
    task.spec.mediaSelection = {maskpilot::MediaSelectionMode::VideoAudio, "v-720", "a-en",
                                maskpilot::MediaContainer::Mp4};
    {
        auto store = maskpilot::makeSqliteStore(path);
        store.tasks->save(task);
        auto invalid = task;
        invalid.spec.mediaSelection.primaryFormat = "best/best";
        EXPECT_THROW(store.tasks->save(invalid), std::exception);
        invalid = task;
        invalid.spec.mediaSelection.container = static_cast<maskpilot::MediaContainer>(99);
        EXPECT_THROW(store.tasks->save(invalid), std::exception);
        invalid = task;
        invalid.spec.mediaSelection.mode = maskpilot::MediaSelectionMode::Automatic;
        EXPECT_THROW(store.tasks->save(invalid), std::exception);
    }
    auto store = maskpilot::makeSqliteStore(path);
    const auto loaded = store.tasks->load();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded[0].spec.mediaSelection.mode, task.spec.mediaSelection.mode);
    EXPECT_EQ(loaded[0].spec.mediaSelection.primaryFormat, "v-720");
    EXPECT_EQ(loaded[0].spec.mediaSelection.audioFormat, "a-en");
    EXPECT_EQ(loaded[0].spec.mediaSelection.container, maskpilot::MediaContainer::Mp4);
    EXPECT_FALSE(maskpilot::mediaAudioOnly(loaded[0].spec.mediaSelection, loaded[0].spec.audioOnly));
    EXPECT_EQ(loaded[0].spec.destination, task.spec.destination);
}
