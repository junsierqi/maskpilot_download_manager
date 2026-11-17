#include <algorithm>
#include <cctype>
#include <maskpilot/infrastructure/adapters.h>
#include <sqlite3.h>
#include <stdexcept>

namespace maskpilot {
namespace {
std::string utf8(const std::filesystem::path& path) {
    auto s = path.u8string();
    return {s.begin(), s.end()};
}
class Statement {
  public:
    sqlite3_stmt* value = nullptr;
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
            throw std::runtime_error("Database prepare failed.");
    }
    ~Statement() { sqlite3_finalize(value); }
    void text(int index, const std::string& data) {
        if (sqlite3_bind_text(value, index, data.c_str(), static_cast<int>(data.size()), SQLITE_TRANSIENT) !=
            SQLITE_OK)
            throw std::runtime_error("Database binding failed.");
    }
    void number(int index, std::int64_t data) {
        if (sqlite3_bind_int64(value, index, data) != SQLITE_OK)
            throw std::runtime_error("Database binding failed.");
    }
};
class Repository final : public ITaskRepository {
    sqlite3* db_ = nullptr;
    void execute(const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Database operation failed.");
    }

  public:
    explicit Repository(const std::filesystem::path& path) {
        std::filesystem::create_directories(path.parent_path());
        if (sqlite3_open_v2(utf8(path).c_str(), &db_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("Cannot open task database.");
        }
        try {
            sqlite3_busy_timeout(db_, 5000);
            int schemaVersion = 0;
            {
                Statement version(db_, "PRAGMA user_version");
                if (sqlite3_step(version.value) != SQLITE_ROW || sqlite3_column_int(version.value, 0) > 2)
                    throw std::runtime_error("Database was created by a newer version.");
                schemaVersion = sqlite3_column_int(version.value, 0);
            }
            if (schemaVersion == 1) {
                auto backupPath = path;
                backupPath += L".before-v2-" + std::filesystem::path(newTaskId()).wstring() + L".db";
                sqlite3* backupDb = nullptr;
                if (sqlite3_open(utf8(backupPath).c_str(), &backupDb) != SQLITE_OK) {
                    sqlite3_close(backupDb);
                    throw std::runtime_error("Cannot create migration backup.");
                }
                auto* backup = sqlite3_backup_init(backupDb, "main", db_, "main");
                const auto result = backup ? sqlite3_backup_step(backup, -1) : SQLITE_ERROR;
                if (backup)
                    sqlite3_backup_finish(backup);
                sqlite3_close(backupDb);
                if (result != SQLITE_DONE)
                    throw std::runtime_error("Cannot back up task database before migration.");
            }
            execute("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
            execute(
                "CREATE TABLE IF NOT EXISTS tasks(id TEXT PRIMARY KEY,url TEXT NOT NULL,destination TEXT NOT "
                "NULL,category TEXT NOT NULL,referrer TEXT NOT NULL,user_agent TEXT NOT NULL,proxy TEXT NOT "
                "NULL,expected_sha256 TEXT NOT NULL,connections INTEGER NOT NULL,priority INTEGER NOT "
                "NULL,max_retries INTEGER NOT NULL,speed_limit INTEGER NOT NULL,scheduled_at INTEGER NOT "
                "NULL,stop_at INTEGER NOT NULL,state INTEGER NOT NULL,downloaded INTEGER NOT NULL,total "
                "INTEGER NOT NULL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,retries INTEGER "
                "NOT NULL,resumable INTEGER NOT NULL,error TEXT NOT NULL);");
            if (schemaVersion < 2)
                execute(
                    "BEGIN IMMEDIATE; ALTER TABLE tasks ADD COLUMN media INTEGER NOT NULL DEFAULT 0; ALTER "
                    "TABLE tasks ADD COLUMN audio_only INTEGER NOT NULL DEFAULT 0; ALTER TABLE tasks ADD "
                    "COLUMN media_format TEXT NOT NULL DEFAULT 'bestvideo+bestaudio/best'; ALTER TABLE tasks "
                    "ADD COLUMN media_downloader TEXT NOT NULL DEFAULT ''; ALTER TABLE tasks ADD COLUMN "
                    "media_muxer TEXT NOT NULL DEFAULT ''; PRAGMA user_version=2; COMMIT;");
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }
    ~Repository() override {
        if (db_)
            sqlite3_close(db_);
    }
    std::vector<Task> load() override {
        Statement q(db_, "SELECT * FROM tasks ORDER BY created_at,id");
        std::vector<Task> result;
        auto text = [&](int col) {
            const auto* p = sqlite3_column_text(q.value, col);
            return p ? std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(q.value, col))
                     : std::string();
        };
        int status;
        while ((status = sqlite3_step(q.value)) == SQLITE_ROW) {
            Task t;
            t.id = text(0);
            t.spec.url = text(1);
            const auto destination = text(2);
            t.spec.destination = std::filesystem::path(std::u8string(destination.begin(), destination.end()));
            t.spec.category = text(3);
            t.spec.referrer = text(4);
            t.spec.userAgent = text(5);
            t.spec.proxy = text(6);
            t.spec.expectedSha256 = text(7);
            t.spec.connections = sqlite3_column_int(q.value, 8);
            t.spec.priority = sqlite3_column_int(q.value, 9);
            t.spec.maxRetries = sqlite3_column_int(q.value, 10);
            t.spec.speedLimit = sqlite3_column_int64(q.value, 11);
            t.spec.scheduledAt = sqlite3_column_int64(q.value, 12);
            t.spec.stopAt = sqlite3_column_int64(q.value, 13);
            const auto state = sqlite3_column_int(q.value, 14);
            t.state = state >= 0 && state < 14 ? static_cast<TaskState>(state) : TaskState::NeedsAttention;
            t.downloaded = sqlite3_column_int64(q.value, 15);
            t.total = sqlite3_column_int64(q.value, 16);
            t.createdAt = sqlite3_column_int64(q.value, 17);
            t.updatedAt = sqlite3_column_int64(q.value, 18);
            t.retries = sqlite3_column_int(q.value, 19);
            t.resumable = sqlite3_column_int(q.value, 20) != 0;
            t.error = text(21);
            t.spec.media = sqlite3_column_int(q.value, 22) != 0;
            t.spec.audioOnly = sqlite3_column_int(q.value, 23) != 0;
            t.spec.mediaFormat = text(24);
            const auto downloader = text(25), muxer = text(26);
            t.spec.mediaDownloader =
                std::filesystem::path(std::u8string(downloader.begin(), downloader.end()));
            t.spec.mediaMuxer = std::filesystem::path(std::u8string(muxer.begin(), muxer.end()));
            if (t.id.size() != 32 || !std::all_of(t.id.begin(), t.id.end(), [](unsigned char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                }))
                throw std::runtime_error("Invalid persistent task identity.");
            result.push_back(std::move(t));
        }
        if (status != SQLITE_DONE)
            throw std::runtime_error("Cannot read task database.");
        return result;
    }
    void save(const Task& t) override {
        Statement q(
            db_,
            "INSERT OR REPLACE INTO tasks VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        q.text(1, t.id);
        q.text(2, t.spec.url);
        q.text(3, utf8(t.spec.destination));
        q.text(4, t.spec.category);
        q.text(5, t.spec.referrer);
        q.text(6, t.spec.userAgent);
        q.text(7, t.spec.proxy);
        q.text(8, t.spec.expectedSha256);
        q.number(9, t.spec.connections);
        q.number(10, t.spec.priority);
        q.number(11, t.spec.maxRetries);
        q.number(12, t.spec.speedLimit);
        q.number(13, t.spec.scheduledAt);
        q.number(14, t.spec.stopAt);
        q.number(15, static_cast<int>(t.state));
        q.number(16, t.downloaded);
        q.number(17, t.total);
        q.number(18, t.createdAt);
        q.number(19, t.updatedAt);
        q.number(20, t.retries);
        q.number(21, t.resumable);
        q.text(22, t.error);
        q.number(23, t.spec.media);
        q.number(24, t.spec.audioOnly);
        q.text(25, t.spec.mediaFormat);
        q.text(26, utf8(t.spec.mediaDownloader));
        q.text(27, utf8(t.spec.mediaMuxer));
        if (sqlite3_step(q.value) != SQLITE_DONE)
            throw std::runtime_error("Cannot save task database.");
    }
    void erase(const std::string& id) override {
        Statement q(db_, "DELETE FROM tasks WHERE id=?");
        q.text(1, id);
        if (sqlite3_step(q.value) != SQLITE_DONE)
            throw std::runtime_error("Cannot remove task.");
    }
};
} // namespace
std::unique_ptr<ITaskRepository> makeSqliteRepository(const std::filesystem::path& path) {
    return std::make_unique<Repository>(path);
}
} // namespace maskpilot
