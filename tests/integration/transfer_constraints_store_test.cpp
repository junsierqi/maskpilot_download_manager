#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <sqlite3.h>

namespace {
struct ConstraintTemp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-constraints-store-" + maskpilot::newTaskId());
    ConstraintTemp() { std::filesystem::create_directories(path); }
    ~ConstraintTemp() {
        std::error_code error;
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().string().starts_with("maskpilot-constraints-store-"))
            std::filesystem::remove_all(path, error);
    }
};
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
Database open(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(path.string().c_str(), &raw) != SQLITE_OK) {
        sqlite3_close(raw);
        throw std::runtime_error("Cannot open owned fixture database.");
    }
    return Database(raw, sqlite3_close);
}
int version(sqlite3* database) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot read fixture schema version.");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (sqlite3_step(raw) != SQLITE_ROW)
        throw std::runtime_error("Missing fixture schema version.");
    return sqlite3_column_int(raw, 0);
}
maskpilot::Task task(const std::filesystem::path& root) {
    maskpilot::Task value;
    value.id = maskpilot::newTaskId();
    value.requestId = maskpilot::newTaskId();
    value.requestName = "bounded.bin";
    value.spec.url = "https://example.test/source";
    value.spec.destination = root / "bounded.bin";
    value.spec.maxStoredBytes = 9007199254740991LL;
    value.spec.sameOriginRedirectOnly = true;
    value.responseUrl = "https://example.test/final?version=2";
    return value;
}
} // namespace

TEST(TransferConstraintStorage, MigratesRealVersionTenWithDefaultPolicyAndOriginalBackup) {
    ConstraintTemp temporary;
    const auto path = temporary.path / "tasks.db";
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    {
        auto database = open(path);
        for (const auto* name :
             {"0001_initial.sql", "0002_media.sql", "0003_ftp.sql", "0004_torrents.sql", "0005_proxy.sql",
              "0006_contexts.sql", "0007_queues.sql", "0008_queue_windows.sql",
              "0009_native_payload_budgets.sql", "0010_media_selection.sql"}) {
            std::ifstream input(root / "database/migrations" / name);
            ASSERT_TRUE(input.good()) << name;
            const std::string sql((std::istreambuf_iterator<char>(input)), {});
            ASSERT_EQ(sqlite3_exec(database.get(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK)
                << name;
        }
        ASSERT_EQ(version(database.get()), 10);
        ASSERT_EQ(
            sqlite3_exec(database.get(),
                         "INSERT INTO tasks(id,url,destination,category,referrer,user_agent,proxy,"
                         "expected_sha256,connections,priority,max_retries,speed_limit,scheduled_at,"
                         "stop_at,state,downloaded,total,created_at,updated_at,retries,resumable,error) "
                         "VALUES('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','https://example.test/legacy',"
                         "'legacy.bin','General','','MaskPilot','','',4,0,3,0,0,0,5,0,-1,1,1,0,0,'');",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
    }
    {
        auto store = maskpilot::makeSqliteStore(path);
        const auto loaded = store.tasks->load();
        ASSERT_EQ(loaded.size(), 1);
        EXPECT_EQ(loaded[0].spec.maxStoredBytes, 0);
        EXPECT_FALSE(loaded[0].spec.sameOriginRedirectOnly);
        EXPECT_TRUE(loaded[0].responseUrl.empty());
        EXPECT_EQ(loaded[0].spec.url, "https://example.test/legacy");
    }
    EXPECT_EQ(version(open(path).get()), 18);
    int backups = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path)) {
        if (entry.path().filename().string().find(".before-v11-") != std::string::npos) {
            ++backups;
            EXPECT_EQ(version(open(entry.path()).get()), 10);
        }
    }
    EXPECT_EQ(backups, 1);
    auto reopened = maskpilot::makeSqliteStore(path);
    EXPECT_EQ(reopened.tasks->load().size(), 1);
}

TEST(TransferConstraintStorage, RoundTripsExactBoundAndObservedUrlAndRejectsInvalidMutationAtomically) {
    ConstraintTemp temporary;
    const auto path = temporary.path / "tasks.db";
    const auto original = task(temporary.path);
    {
        auto store = maskpilot::makeSqliteStore(path);
        store.tasks->save(original);
        for (const auto cap : {-1LL, 9007199254740992LL}) {
            auto invalid = original;
            invalid.spec.maxStoredBytes = cap;
            EXPECT_THROW(store.tasks->save(invalid), std::exception);
        }
        for (const auto* url : {"https://user:password@example.test/private", "https:///missing-host",
                                "ftp://example.test/file", "https://example.test/tab\tvalue"}) {
            auto invalid = original;
            invalid.responseUrl = url;
            EXPECT_THROW(store.tasks->save(invalid), std::exception);
        }
        auto invalid = original;
        invalid.responseUrl = "https://example.test/" + std::string(32768, 'x');
        EXPECT_THROW(store.tasks->save(invalid), std::exception);
        invalid = original;
        invalid.spec.url = "ftp://example.test/file";
        EXPECT_THROW(store.tasks->save(invalid), std::exception);
        invalid = original;
        invalid.spec.kind = maskpilot::TransferKind::Media;
        invalid.spec.maxStoredBytes = 0;
        invalid.spec.sameOriginRedirectOnly = false;
        EXPECT_THROW(store.tasks->save(invalid), std::exception);
        invalid = original;
        invalid.ownerId = maskpilot::newTaskId();
        invalid.contextRevision = 1;
        EXPECT_THROW(store.tasks->save(invalid), std::exception);
    }
    auto store = maskpilot::makeSqliteStore(path);
    const auto loaded = store.tasks->load();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded[0].spec.maxStoredBytes, original.spec.maxStoredBytes);
    EXPECT_TRUE(loaded[0].spec.sameOriginRedirectOnly);
    EXPECT_EQ(loaded[0].responseUrl, original.responseUrl);
    EXPECT_EQ(loaded[0].ownerId, original.ownerId);
    EXPECT_EQ(loaded[0].requestId, original.requestId);
    auto cleared = loaded[0];
    cleared.responseUrl.clear();
    cleared.spec.maxStoredBytes = 0;
    cleared.spec.sameOriginRedirectOnly = false;
    store.tasks->save(cleared);
    EXPECT_TRUE(store.tasks->load()[0].responseUrl.empty());
    EXPECT_EQ(store.tasks->load()[0].spec.maxStoredBytes, 0);
}

TEST(TransferConstraintStorage, RejectsExternallyCorruptedSavedPolicyAndObservedAuthority) {
    ConstraintTemp temporary;
    const auto path = temporary.path / "tasks.db";
    const auto original = task(temporary.path);
    for (const auto* mutation :
         {"max_stored_bytes=-1", "same_origin_redirect_only=2", "max_stored_bytes='invalid'",
          "response_url='https://user@example.test/file'",
          "response_url=CAST('https://example.test/file' AS BLOB)"}) {
        {
            auto store = maskpilot::makeSqliteStore(path);
            store.tasks->save(original);
        }
        {
            auto database = open(path);
            const auto sql = std::string("PRAGMA ignore_check_constraints=ON; UPDATE tasks SET ") + mutation;
            ASSERT_EQ(sqlite3_exec(database.get(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        }
        auto store = maskpilot::makeSqliteStore(path);
        EXPECT_THROW(store.tasks->load(), std::exception) << mutation;
    }
}
