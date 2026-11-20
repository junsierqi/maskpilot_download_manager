#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <sqlite3.h>

namespace {
struct MetadataTemp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-response-store-" + maskpilot::newTaskId());
    MetadataTemp() { std::filesystem::create_directories(path); }
    ~MetadataTemp() {
        std::error_code error;
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().string().starts_with("maskpilot-response-store-"))
            std::filesystem::remove_all(path, error);
    }
};
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
Database openMetadata(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("Cannot open owned metadata fixture.");
    }
    return Database(database, sqlite3_close);
}
int metadataVersion(sqlite3* database) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare schema query.");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (sqlite3_step(raw) != SQLITE_ROW)
        throw std::runtime_error("Cannot read schema version.");
    return sqlite3_column_int(raw, 0);
}
maskpilot::Task metadataTask(const std::filesystem::path& root) {
    maskpilot::Task task;
    task.id = maskpilot::newTaskId();
    task.spec.url = "https://example.test/source";
    task.spec.destination = root / "page.html";
    task.spec.maxStoredBytes = 4096;
    task.spec.sameOriginRedirectOnly = true;
    task.responseUrl = "https://example.test/final";
    task.responseContentType = "text/html; charset=UTF-8";
    return task;
}
} // namespace

TEST(ResponseContentTypeStorage, MigratesActualVersionElevenAndKeepsOriginalBackupAndUnknownType) {
    MetadataTemp temporary;
    const auto path = temporary.path / "tasks.db";
    const auto migrations =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "database/migrations";
    {
        auto database = openMetadata(path);
        for (const auto* name : {"0001_initial.sql", "0002_media.sql", "0003_ftp.sql", "0004_torrents.sql",
                                 "0005_proxy.sql", "0006_contexts.sql", "0007_queues.sql",
                                 "0008_queue_windows.sql", "0009_native_payload_budgets.sql",
                                 "0010_media_selection.sql", "0011_transfer_constraints.sql"}) {
            std::ifstream input(migrations / name);
            ASSERT_TRUE(input.good()) << name;
            const std::string sql((std::istreambuf_iterator<char>(input)), {});
            ASSERT_EQ(sqlite3_exec(database.get(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK)
                << name;
        }
        ASSERT_EQ(metadataVersion(database.get()), 11);
        ASSERT_EQ(sqlite3_exec(database.get(),
                               "INSERT INTO tasks(id,url,destination,category,referrer,user_agent,proxy,"
                               "expected_sha256,connections,priority,max_retries,speed_limit,scheduled_at,"
                               "stop_at,state,downloaded,total,created_at,updated_at,retries,resumable,error,"
                               "max_stored_bytes,same_origin_redirect_only,response_url) "
                               "VALUES('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','https://example.test/source',"
                               "'page.html','General','','MaskPilot','','',4,0,3,0,0,0,9,10,10,1,1,0,0,'',"
                               "4096,1,'https://example.test/final');",
                               nullptr, nullptr, nullptr),
                  SQLITE_OK);
    }
    {
        auto store = maskpilot::makeSqliteStore(path);
        const auto loaded = store.tasks->load();
        ASSERT_EQ(loaded.size(), 1u);
        EXPECT_EQ(loaded[0].responseUrl, "https://example.test/final");
        EXPECT_TRUE(loaded[0].responseContentType.empty());
        EXPECT_EQ(loaded[0].spec.maxStoredBytes, 4096);
        EXPECT_TRUE(loaded[0].spec.sameOriginRedirectOnly);
    }
    EXPECT_EQ(metadataVersion(openMetadata(path).get()), 17);
    int backups = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path)) {
        if (entry.path().filename().string().find(".before-v12-") != std::string::npos) {
            ++backups;
            EXPECT_EQ(metadataVersion(openMetadata(entry.path()).get()), 11);
        }
    }
    EXPECT_EQ(backups, 1);
    auto reopened = maskpilot::makeSqliteStore(path);
    EXPECT_TRUE(reopened.tasks->load()[0].responseContentType.empty());
}

TEST(ResponseContentTypeStorage, RoundTripsAndInvalidWritesPreservePreviousObservation) {
    MetadataTemp temporary;
    const auto path = temporary.path / "tasks.db";
    const auto original = metadataTask(temporary.path);
    {
        auto store = maskpilot::makeSqliteStore(path);
        store.tasks->save(original);
        for (const auto& type :
             {std::string("text/html\r\nInjected: secret"), std::string("text/\thtml"),
              std::string("text/html\0hidden", 16), std::string(1025, 'x'), std::string("text/\xc3\xa9")}) {
            auto invalid = original;
            invalid.responseContentType = type;
            EXPECT_THROW(store.tasks->save(invalid), std::exception);
        }
        auto invalid = original;
        invalid.responseUrl.clear();
        EXPECT_THROW(store.tasks->save(invalid), std::exception);
        invalid = original;
        invalid.spec.kind = maskpilot::TransferKind::Media;
        invalid.spec.maxStoredBytes = 0;
        invalid.spec.sameOriginRedirectOnly = false;
        EXPECT_THROW(store.tasks->save(invalid), std::exception);
    }
    auto store = maskpilot::makeSqliteStore(path);
    auto loaded = store.tasks->load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].responseContentType, original.responseContentType);
    EXPECT_EQ(loaded[0].responseUrl, original.responseUrl);
    loaded[0].responseContentType.clear();
    store.tasks->save(loaded[0]);
    EXPECT_TRUE(store.tasks->load()[0].responseContentType.empty());
    EXPECT_EQ(store.tasks->load()[0].responseUrl, original.responseUrl);
}

TEST(ResponseContentTypeStorage, RejectsCorruptedDatabaseContentTypeAndMissingUrlBinding) {
    MetadataTemp temporary;
    const auto path = temporary.path / "tasks.db";
    const auto original = metadataTask(temporary.path);
    for (const auto* mutation :
         {"response_content_type=CAST('text/html' AS BLOB)", "response_content_type='text/html'||char(10)",
          "response_content_type='text/html',response_url=''"}) {
        {
            auto store = maskpilot::makeSqliteStore(path);
            store.tasks->save(original);
        }
        {
            auto database = openMetadata(path);
            const auto sql = std::string("UPDATE tasks SET ") + mutation;
            ASSERT_EQ(sqlite3_exec(database.get(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        }
        auto store = maskpilot::makeSqliteStore(path);
        EXPECT_THROW(store.tasks->load(), std::exception) << mutation;
    }
}
