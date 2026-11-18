#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <maskpilot/domain/proxy.h>
#include <maskpilot/infrastructure/adapters.h>
#include <mutex>
#include <set>
#include <sqlite3.h>
#include <sstream>
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
    void blob(int index, const TorrentMetadata& data) {
        if (data && data->size() > maxTorrentMetadataBytes)
            throw std::invalid_argument("Torrent metadata exceeds the supported limit.");
        const auto result = data ? sqlite3_bind_blob(value, index, data->data(),
                                                     static_cast<int>(data->size()), SQLITE_TRANSIENT)
                                 : sqlite3_bind_zeroblob(value, index, 0);
        if (result != SQLITE_OK)
            throw std::runtime_error("Database metadata binding failed.");
    }
};
std::string columnText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? std::string(reinterpret_cast<const char*>(value), sqlite3_column_bytes(statement, column))
                 : std::string();
}
void validateTaskContext(const Task& task) {
    if ((task.ownerId != "standalone" && !isOpaqueId(task.ownerId)) ||
        (!task.requestId.empty() && !isOpaqueId(task.requestId)) || task.contextRevision < 0 ||
        (task.ownerId != "standalone" && task.contextRevision == 0) ||
        (!task.proxyCredentialRef.empty() && !isOpaqueId(task.proxyCredentialRef)) ||
        task.requestName.size() > 1024 || task.requestName.find('\0') != std::string::npos ||
        !isQueueId(task.queueId) || (task.runIntent != RunIntent::Run && task.runIntent != RunIntent::Paused))
        throw std::invalid_argument("Invalid persistent task ownership metadata.");
}
void validateRecord(const CredentialRecord& record) {
    if (!isOpaqueId(record.reference) || (record.ownerId != "standalone" && !isOpaqueId(record.ownerId)) ||
        (!record.ownershipNonce.empty() && !isOpaqueId(record.ownershipNonce)) ||
        parseProxyEndpoint(record.proxyEndpoint).type == ProxyType::Direct)
        throw std::invalid_argument("Invalid credential ownership record.");
}
class Backend {
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
    void execute(const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Database operation failed.");
    }
    bool queueExists(const std::string& id) {
        Statement q(db_, "SELECT 1 FROM download_queues WHERE id=?");
        q.text(1, id);
        const auto status = sqlite3_step(q.value);
        if (status != SQLITE_ROW && status != SQLITE_DONE)
            throw std::runtime_error("Cannot inspect download queue membership.");
        return status == SQLITE_ROW;
    }

  public:
    explicit Backend(const std::filesystem::path& path) {
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
                if (sqlite3_step(version.value) != SQLITE_ROW || sqlite3_column_int(version.value, 0) > 8 ||
                    sqlite3_column_int(version.value, 0) < 0)
                    throw std::runtime_error("Database was created by a newer version.");
                schemaVersion = sqlite3_column_int(version.value, 0);
            }
            if (schemaVersion > 0 && schemaVersion < 8) {
                auto backupPath = path;
                backupPath += L".before-v" + std::to_wstring(schemaVersion + 1) + L"-" +
                              std::filesystem::path(newTaskId()).wstring() + L".db";
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
            if (schemaVersion < 3)
                execute("BEGIN IMMEDIATE; ALTER TABLE tasks ADD COLUMN ftp_tls INTEGER NOT NULL DEFAULT 0; "
                        "ALTER TABLE tasks ADD COLUMN ftp_active INTEGER NOT NULL DEFAULT 0; "
                        "ALTER TABLE tasks ADD COLUMN ftp_ca TEXT NOT NULL DEFAULT ''; "
                        "PRAGMA user_version=3; COMMIT;");
            if (schemaVersion < 4)
                execute("BEGIN IMMEDIATE; "
                        "ALTER TABLE tasks ADD COLUMN kind INTEGER NOT NULL DEFAULT 0; "
                        "ALTER TABLE tasks ADD COLUMN bt_all INTEGER NOT NULL DEFAULT 1; "
                        "ALTER TABLE tasks ADD COLUMN bt_selection TEXT NOT NULL DEFAULT ''; "
                        "ALTER TABLE tasks ADD COLUMN bt_seed INTEGER NOT NULL DEFAULT 0; "
                        "ALTER TABLE tasks ADD COLUMN bt_upload_limit INTEGER NOT NULL DEFAULT 0; "
                        "ALTER TABLE tasks ADD COLUMN bt_metadata BLOB NOT NULL DEFAULT X''; "
                        "ALTER TABLE tasks ADD COLUMN bt_published INTEGER NOT NULL DEFAULT 0; "
                        "ALTER TABLE tasks ADD COLUMN bt_uploaded INTEGER NOT NULL DEFAULT 0; "
                        "UPDATE tasks SET kind=CASE WHEN media<>0 THEN 2 "
                        "WHEN url LIKE 'ftp://%' OR url LIKE 'ftps://%' THEN 1 ELSE 0 END; "
                        "PRAGMA user_version=4; COMMIT;");
            if (schemaVersion < 5)
                execute("BEGIN IMMEDIATE; ALTER TABLE tasks ADD COLUMN proxy_auth_required INTEGER "
                        "NOT NULL DEFAULT 0; PRAGMA user_version=5; COMMIT;");
            if (schemaVersion < 6)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN owner_id TEXT NOT NULL DEFAULT 'standalone';
ALTER TABLE tasks ADD COLUMN request_id TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN context_revision INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN proxy_credential_ref TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN origin_auth_required INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN request_name TEXT NOT NULL DEFAULT '';
UPDATE tasks SET request_id=id WHERE owner_id='standalone';
CREATE UNIQUE INDEX tasks_owner_request ON tasks(owner_id,request_id) WHERE request_id<>'';
CREATE TABLE download_contexts(
 id TEXT PRIMARY KEY, external_key TEXT NOT NULL UNIQUE, label TEXT NOT NULL,
 download_root TEXT NOT NULL, proxy_endpoint TEXT NOT NULL,
 proxy_auth_required INTEGER NOT NULL CHECK(proxy_auth_required IN(0,1)),
 proxy_credential_ref TEXT NOT NULL, origin_auth_required INTEGER NOT NULL CHECK(origin_auth_required IN(0,1)),
 revision INTEGER NOT NULL CHECK(revision>0), enabled INTEGER NOT NULL CHECK(enabled IN(0,1)));
CREATE TABLE credential_records(reference TEXT PRIMARY KEY,owner_id TEXT NOT NULL,proxy_endpoint TEXT NOT NULL,
 ownership_nonce TEXT NOT NULL DEFAULT '');
CREATE TABLE store_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
PRAGMA user_version=6;
COMMIT;)sql");
            if (schemaVersion < 7)
                execute(R"sql(BEGIN IMMEDIATE;
CREATE TABLE download_queues(id TEXT PRIMARY KEY,name TEXT NOT NULL UNIQUE,revision INTEGER NOT NULL CHECK(revision>=1),
 enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),max_active INTEGER NOT NULL CHECK(max_active BETWEEN 1 AND 8),
 download_limit INTEGER NOT NULL CHECK(download_limit=0 OR download_limit>=1024),
 upload_limit INTEGER NOT NULL CHECK(upload_limit=0 OR upload_limit>=1024));
INSERT INTO download_queues VALUES('default','Default',1,1,8,0,0);
ALTER TABLE tasks ADD COLUMN queue_id TEXT NOT NULL DEFAULT 'default';
ALTER TABLE tasks ADD COLUMN run_intent INTEGER NOT NULL DEFAULT 1 CHECK(run_intent IN(0,1));
UPDATE tasks SET run_intent=0 WHERE state=1;
CREATE INDEX tasks_queue_state ON tasks(queue_id,state,priority,created_at,id);
PRAGMA user_version=7;
COMMIT;)sql");
            if (schemaVersion < 8)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE download_queues ADD COLUMN schedule_enabled INTEGER NOT NULL DEFAULT 0 CHECK(schedule_enabled IN(0,1));
ALTER TABLE download_queues ADD COLUMN schedule_weekdays INTEGER NOT NULL DEFAULT 127 CHECK(schedule_weekdays BETWEEN 1 AND 127);
ALTER TABLE download_queues ADD COLUMN schedule_start INTEGER NOT NULL DEFAULT 0 CHECK(schedule_start BETWEEN 0 AND 1439);
ALTER TABLE download_queues ADD COLUMN schedule_end INTEGER NOT NULL DEFAULT 0 CHECK(schedule_end BETWEEN 0 AND 1439);
ALTER TABLE download_queues ADD COLUMN schedule_timezone TEXT NOT NULL DEFAULT 'UTC';
PRAGMA user_version=8;
COMMIT;)sql");
            Statement installation(
                db_, "INSERT OR IGNORE INTO store_metadata(key,value) VALUES('installation_id',?)");
            installation.text(1, newTaskId());
            if (sqlite3_step(installation.value) != SQLITE_DONE)
                throw std::runtime_error("Cannot initialize store identity.");
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }
    ~Backend() {
        if (db_)
            sqlite3_close(db_);
    }
    std::vector<Task> load() {
        std::lock_guard lock(mutex_);
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
            t.state = state >= 0 && state < static_cast<int>(TaskState::Count) ? static_cast<TaskState>(state)
                                                                               : TaskState::NeedsAttention;
            t.downloaded = sqlite3_column_int64(q.value, 15);
            t.total = sqlite3_column_int64(q.value, 16);
            t.createdAt = sqlite3_column_int64(q.value, 17);
            t.updatedAt = sqlite3_column_int64(q.value, 18);
            t.retries = sqlite3_column_int(q.value, 19);
            t.resumable = sqlite3_column_int(q.value, 20) != 0;
            t.error = text(21);
            t.spec.kind = static_cast<TransferKind>(sqlite3_column_int(q.value, 30));
            t.spec.audioOnly = sqlite3_column_int(q.value, 23) != 0;
            t.spec.mediaFormat = text(24);
            const auto downloader = text(25), muxer = text(26);
            t.spec.mediaDownloader =
                std::filesystem::path(std::u8string(downloader.begin(), downloader.end()));
            t.spec.mediaMuxer = std::filesystem::path(std::u8string(muxer.begin(), muxer.end()));
            t.spec.ftpExplicitTls = sqlite3_column_int(q.value, 27) != 0;
            t.spec.ftpActiveMode = sqlite3_column_int(q.value, 28) != 0;
            const auto ca = text(29);
            t.spec.ftpCaBundle = std::filesystem::path(std::u8string(ca.begin(), ca.end()));
            t.spec.torrent.allFiles = sqlite3_column_int(q.value, 31) != 0;
            const auto selection = text(32);
            std::size_t begin = 0;
            while (begin < selection.size()) {
                const auto end = selection.find(',', begin);
                const auto* first = selection.data() + begin;
                const auto* last = selection.data() + (end == std::string::npos ? selection.size() : end);
                int index = -1;
                const auto parsed = std::from_chars(first, last, index);
                if (parsed.ec != std::errc() || parsed.ptr != last ||
                    t.spec.torrent.selectedFiles.size() >= maxTorrentFiles)
                    throw std::runtime_error("Invalid saved torrent file selection.");
                t.spec.torrent.selectedFiles.push_back(index);
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
            t.spec.torrent.seedAfterDownload = sqlite3_column_int(q.value, 33) != 0;
            t.spec.torrent.uploadLimit = sqlite3_column_int64(q.value, 34);
            const auto metadataSize = sqlite3_column_bytes(q.value, 35);
            if (metadataSize < 0 || static_cast<std::size_t>(metadataSize) > maxTorrentMetadataBytes)
                throw std::runtime_error("Saved torrent metadata exceeds the supported limit.");
            if (metadataSize > 0) {
                const auto* metadata = static_cast<const char*>(sqlite3_column_blob(q.value, 35));
                if (!metadata)
                    throw std::runtime_error("Cannot read saved torrent metadata.");
                t.spec.torrent.metadata =
                    std::make_shared<const std::vector<char>>(metadata, metadata + metadataSize);
            }
            t.torrentPublished = sqlite3_column_int(q.value, 36) != 0;
            t.uploaded = sqlite3_column_int64(q.value, 37);
            t.spec.proxyAuthenticationRequired = sqlite3_column_int(q.value, 38) != 0;
            t.ownerId = text(39);
            t.requestId = text(40);
            t.contextRevision = sqlite3_column_int64(q.value, 41);
            t.proxyCredentialRef = text(42);
            t.originAuthenticationRequired = sqlite3_column_int(q.value, 43) != 0;
            t.spec.originAuthenticationRequired = t.originAuthenticationRequired;
            t.requestName = text(44);
            t.queueId = text(45);
            t.runIntent = static_cast<RunIntent>(sqlite3_column_int(q.value, 46));
            validateTaskContext(t);
            if (!queueExists(t.queueId))
                throw std::runtime_error("Saved task refers to an unavailable download queue.");
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
    void save(const Task& t) {
        std::lock_guard lock(mutex_);
        validateTaskContext(t);
        if (!queueExists(t.queueId))
            throw std::invalid_argument("Task download queue is unavailable.");
        Statement q(db_, R"sql(INSERT INTO tasks(
id,url,destination,category,referrer,user_agent,proxy,expected_sha256,connections,priority,max_retries,
speed_limit,scheduled_at,stop_at,state,downloaded,total,created_at,updated_at,retries,resumable,error,
media,audio_only,media_format,media_downloader,media_muxer,ftp_tls,ftp_active,ftp_ca,kind,bt_all,bt_selection,
bt_seed,bt_upload_limit,bt_metadata,bt_published,bt_uploaded,proxy_auth_required,owner_id,request_id,
context_revision,proxy_credential_ref,origin_auth_required,request_name,queue_id,run_intent)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(id) DO UPDATE SET
url=excluded.url,destination=excluded.destination,category=excluded.category,referrer=excluded.referrer,
user_agent=excluded.user_agent,proxy=excluded.proxy,expected_sha256=excluded.expected_sha256,
connections=excluded.connections,priority=excluded.priority,max_retries=excluded.max_retries,
speed_limit=excluded.speed_limit,scheduled_at=excluded.scheduled_at,stop_at=excluded.stop_at,
state=excluded.state,downloaded=excluded.downloaded,total=excluded.total,created_at=excluded.created_at,
updated_at=excluded.updated_at,retries=excluded.retries,resumable=excluded.resumable,error=excluded.error,
media=excluded.media,audio_only=excluded.audio_only,media_format=excluded.media_format,
media_downloader=excluded.media_downloader,media_muxer=excluded.media_muxer,ftp_tls=excluded.ftp_tls,
ftp_active=excluded.ftp_active,ftp_ca=excluded.ftp_ca,kind=excluded.kind,bt_all=excluded.bt_all,
bt_selection=excluded.bt_selection,bt_seed=excluded.bt_seed,bt_upload_limit=excluded.bt_upload_limit,
bt_metadata=excluded.bt_metadata,bt_published=excluded.bt_published,bt_uploaded=excluded.bt_uploaded,
proxy_auth_required=excluded.proxy_auth_required,request_id=excluded.request_id,
context_revision=excluded.context_revision,proxy_credential_ref=excluded.proxy_credential_ref,
origin_auth_required=excluded.origin_auth_required,request_name=excluded.request_name,
queue_id=excluded.queue_id,run_intent=excluded.run_intent
WHERE tasks.owner_id=excluded.owner_id)sql");
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
        q.number(23, (t.spec.kind == maskpilot::TransferKind::Media));
        q.number(24, t.spec.audioOnly);
        q.text(25, t.spec.mediaFormat);
        q.text(26, utf8(t.spec.mediaDownloader));
        q.text(27, utf8(t.spec.mediaMuxer));
        q.number(28, t.spec.ftpExplicitTls);
        q.number(29, t.spec.ftpActiveMode);
        q.text(30, utf8(t.spec.ftpCaBundle));
        q.number(31, static_cast<int>(sourceKind(t.spec)));
        q.number(32, t.spec.torrent.allFiles);
        std::ostringstream selection;
        for (std::size_t n = 0; n < t.spec.torrent.selectedFiles.size(); ++n) {
            if (n)
                selection << ',';
            selection << t.spec.torrent.selectedFiles[n];
        }
        q.text(33, selection.str());
        q.number(34, t.spec.torrent.seedAfterDownload);
        q.number(35, t.spec.torrent.uploadLimit);
        q.blob(36, t.spec.torrent.metadata);
        q.number(37, t.torrentPublished);
        q.number(38, t.uploaded);
        q.number(39, t.spec.proxyAuthenticationRequired || !t.spec.proxyUsername.empty() ||
                         !t.spec.proxyPassword.empty());
        q.text(40, t.ownerId);
        q.text(41, t.requestId);
        q.number(42, t.contextRevision);
        q.text(43, t.proxyCredentialRef);
        q.number(44, t.originAuthenticationRequired || requiresOriginAuthentication(t.spec));
        q.text(45, t.requestName);
        q.text(46, t.queueId);
        q.number(47, static_cast<int>(t.runIntent));
        if (sqlite3_step(q.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
            throw std::runtime_error("Cannot save task ownership or request identity.");
    }
    void erase(const std::string& id) {
        std::lock_guard lock(mutex_);
        Statement q(db_, "DELETE FROM tasks WHERE id=?");
        q.text(1, id);
        if (sqlite3_step(q.value) != SQLITE_DONE)
            throw std::runtime_error("Cannot remove task.");
    }
    std::string installationId() {
        std::lock_guard lock(mutex_);
        Statement q(db_, "SELECT value FROM store_metadata WHERE key='installation_id'");
        if (sqlite3_step(q.value) != SQLITE_ROW)
            throw std::runtime_error("Store identity is unavailable.");
        const auto id = columnText(q.value, 0);
        if (!isOpaqueId(id))
            throw std::runtime_error("Store identity is invalid.");
        return id;
    }
    std::vector<DownloadContext> loadContexts() {
        std::lock_guard lock(mutex_);
        Statement q(
            db_,
            "SELECT id,external_key,label,download_root,proxy_endpoint,proxy_auth_required,"
            "proxy_credential_ref,origin_auth_required,revision,enabled FROM download_contexts ORDER BY id");
        std::vector<DownloadContext> contexts;
        int status;
        while ((status = sqlite3_step(q.value)) == SQLITE_ROW) {
            DownloadContext context;
            context.id = columnText(q.value, 0);
            context.externalKey = columnText(q.value, 1);
            context.label = columnText(q.value, 2);
            const auto root = columnText(q.value, 3);
            context.downloadRoot = std::filesystem::path(std::u8string(root.begin(), root.end()));
            context.proxyEndpoint = columnText(q.value, 4);
            context.proxyAuthenticationRequired = sqlite3_column_int(q.value, 5) != 0;
            context.proxyCredentialRef = columnText(q.value, 6);
            context.originAuthenticationRequired = sqlite3_column_int(q.value, 7) != 0;
            context.revision = sqlite3_column_int64(q.value, 8);
            context.enabled = sqlite3_column_int(q.value, 9) != 0;
            if (!validateDownloadContext(context).empty())
                throw std::runtime_error("Saved download context is invalid.");
            contexts.push_back(std::move(context));
        }
        if (status != SQLITE_DONE)
            throw std::runtime_error("Cannot read download contexts.");
        return contexts;
    }
    void saveContext(const DownloadContext& context) {
        std::lock_guard lock(mutex_);
        if (!validateDownloadContext(context).empty())
            throw std::invalid_argument("Invalid download context.");
        {
            Statement exists(db_, "SELECT revision FROM download_contexts WHERE id=?");
            exists.text(1, context.id);
            const auto status = sqlite3_step(exists.value);
            if (status != SQLITE_ROW && status != SQLITE_DONE)
                throw std::runtime_error("Cannot inspect context revision.");
            if (status == SQLITE_DONE && context.revision != 1)
                throw std::invalid_argument("New download contexts must start at revision one.");
        }
        Statement q(db_, R"sql(INSERT INTO download_contexts(
id,external_key,label,download_root,proxy_endpoint,proxy_auth_required,proxy_credential_ref,
origin_auth_required,revision,enabled) VALUES(?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(id) DO UPDATE SET external_key=excluded.external_key,label=excluded.label,
download_root=excluded.download_root,proxy_endpoint=excluded.proxy_endpoint,
proxy_auth_required=excluded.proxy_auth_required,proxy_credential_ref=excluded.proxy_credential_ref,
origin_auth_required=excluded.origin_auth_required,revision=excluded.revision,enabled=excluded.enabled
WHERE excluded.revision=download_contexts.revision+1 OR
(excluded.revision=download_contexts.revision AND excluded.external_key=download_contexts.external_key
AND excluded.label=download_contexts.label AND excluded.download_root=download_contexts.download_root
AND excluded.proxy_endpoint=download_contexts.proxy_endpoint
AND excluded.proxy_auth_required=download_contexts.proxy_auth_required
AND excluded.proxy_credential_ref=download_contexts.proxy_credential_ref
AND excluded.origin_auth_required=download_contexts.origin_auth_required
AND excluded.enabled=download_contexts.enabled))sql");
        q.text(1, context.id);
        q.text(2, context.externalKey);
        q.text(3, context.label);
        q.text(4, utf8(context.downloadRoot));
        q.text(5, context.proxyEndpoint);
        q.number(6, context.proxyAuthenticationRequired);
        q.text(7, context.proxyCredentialRef);
        q.number(8, context.originAuthenticationRequired);
        q.number(9, context.revision);
        q.number(10, context.enabled);
        if (sqlite3_step(q.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
            throw std::runtime_error("Cannot save context revision or external identity.");
    }
    std::vector<CredentialRecord> loadCredentialRecords() {
        std::lock_guard lock(mutex_);
        Statement q(db_, "SELECT reference,owner_id,proxy_endpoint,ownership_nonce FROM credential_records "
                         "ORDER BY reference");
        std::vector<CredentialRecord> records;
        int status;
        while ((status = sqlite3_step(q.value)) == SQLITE_ROW) {
            CredentialRecord record{columnText(q.value, 0), columnText(q.value, 1), columnText(q.value, 2),
                                    columnText(q.value, 3)};
            validateRecord(record);
            records.push_back(std::move(record));
        }
        if (status != SQLITE_DONE)
            throw std::runtime_error("Cannot read credential ownership records.");
        return records;
    }
    void saveCredentialRecord(const CredentialRecord& record) {
        std::lock_guard lock(mutex_);
        validateRecord(record);
        Statement q(
            db_,
            R"sql(INSERT INTO credential_records(reference,owner_id,proxy_endpoint,ownership_nonce) VALUES(?,?,?,?)
ON CONFLICT(reference) DO UPDATE SET reference=excluded.reference
WHERE credential_records.owner_id=excluded.owner_id AND credential_records.proxy_endpoint=excluded.proxy_endpoint
AND credential_records.ownership_nonce=excluded.ownership_nonce)sql");
        q.text(1, record.reference);
        q.text(2, record.ownerId);
        q.text(3, record.proxyEndpoint);
        q.text(4, record.ownershipNonce);
        if (sqlite3_step(q.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
            throw std::runtime_error("Cannot save credential ownership record.");
    }
    void eraseCredentialRecord(const std::string& reference) {
        std::lock_guard lock(mutex_);
        if (!isOpaqueId(reference))
            throw std::invalid_argument("Invalid credential ownership reference.");
        Statement q(db_, "DELETE FROM credential_records WHERE reference=?");
        q.text(1, reference);
        if (sqlite3_step(q.value) != SQLITE_DONE)
            throw std::runtime_error("Cannot erase credential ownership record.");
    }
    std::vector<DownloadQueue> loadQueues() {
        std::lock_guard lock(mutex_);
        Statement q(db_, "SELECT id,name,revision,enabled,max_active,download_limit,upload_limit,"
                         "schedule_enabled,schedule_weekdays,schedule_start,schedule_end,schedule_timezone "
                         "FROM download_queues ORDER BY id");
        std::vector<DownloadQueue> queues;
        int status;
        while ((status = sqlite3_step(q.value)) == SQLITE_ROW) {
            DownloadQueue queue;
            queue.id = columnText(q.value, 0);
            queue.name = columnText(q.value, 1);
            queue.revision = sqlite3_column_int64(q.value, 2);
            queue.enabled = sqlite3_column_int(q.value, 3) != 0;
            queue.maxActive = sqlite3_column_int(q.value, 4);
            queue.downloadLimit = sqlite3_column_int64(q.value, 5);
            queue.uploadLimit = sqlite3_column_int64(q.value, 6);
            queue.schedule.enabled = sqlite3_column_int(q.value, 7) != 0;
            queue.schedule.weekdays = sqlite3_column_int(q.value, 8);
            queue.schedule.startMinute = sqlite3_column_int(q.value, 9);
            queue.schedule.endMinute = sqlite3_column_int(q.value, 10);
            queue.schedule.timeZone = columnText(q.value, 11);
            if (!validateDownloadQueue(queue).empty() || queues.size() >= maxDownloadQueues)
                throw std::runtime_error("Saved download queue policy is invalid.");
            queues.push_back(std::move(queue));
        }
        if (status != SQLITE_DONE)
            throw std::runtime_error("Cannot read download queue policies.");
        return queues;
    }
    void saveQueue(const DownloadQueue& queue, std::int64_t expectedRevision) {
        std::lock_guard lock(mutex_);
        if (!validateDownloadQueue(queue).empty() || expectedRevision < 0 ||
            expectedRevision == std::numeric_limits<std::int64_t>::max() ||
            queue.revision != expectedRevision + 1)
            throw std::invalid_argument("Invalid queue policy revision or limits.");
        if (expectedRevision == 0) {
            Statement count(db_, "SELECT COUNT(*) FROM download_queues");
            if (sqlite3_step(count.value) != SQLITE_ROW ||
                sqlite3_column_int64(count.value, 0) >= static_cast<std::int64_t>(maxDownloadQueues))
                throw std::runtime_error("Download queue policy limit reached.");
        }
        Statement q(db_,
                    expectedRevision == 0
                        ? "INSERT INTO "
                          "download_queues(id,name,revision,enabled,max_active,"
                          "download_limit,upload_limit,schedule_enabled,schedule_weekdays,"
                          "schedule_start,schedule_end,schedule_timezone) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"
                        : "UPDATE download_queues SET "
                          "name=?,revision=?,enabled=?,max_active=?,download_limit=?,"
                          "upload_limit=?,schedule_enabled=?,schedule_weekdays=?,schedule_start=?,"
                          "schedule_end=?,schedule_timezone=? WHERE id=? AND revision=?");
        if (expectedRevision == 0) {
            q.text(1, queue.id);
            q.text(2, queue.name);
            q.number(3, queue.revision);
            q.number(4, queue.enabled);
            q.number(5, queue.maxActive);
            q.number(6, queue.downloadLimit);
            q.number(7, queue.uploadLimit);
            q.number(8, queue.schedule.enabled);
            q.number(9, queue.schedule.weekdays);
            q.number(10, queue.schedule.startMinute);
            q.number(11, queue.schedule.endMinute);
            q.text(12, queue.schedule.timeZone);
        } else {
            q.text(1, queue.name);
            q.number(2, queue.revision);
            q.number(3, queue.enabled);
            q.number(4, queue.maxActive);
            q.number(5, queue.downloadLimit);
            q.number(6, queue.uploadLimit);
            q.number(7, queue.schedule.enabled);
            q.number(8, queue.schedule.weekdays);
            q.number(9, queue.schedule.startMinute);
            q.number(10, queue.schedule.endMinute);
            q.text(11, queue.schedule.timeZone);
            q.text(12, queue.id);
            q.number(13, expectedRevision);
        }
        if (sqlite3_step(q.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
            throw std::runtime_error("Queue changed or its name is already used. Refresh before retrying.");
    }
    void eraseQueue(const std::string& id, std::int64_t expectedRevision) {
        std::lock_guard lock(mutex_);
        if (!isQueueId(id) || id == defaultQueueId || expectedRevision < 1)
            throw std::invalid_argument("The default queue cannot be removed or queue identity is invalid.");
        Statement q(db_, "DELETE FROM download_queues WHERE id=? AND revision=? AND NOT EXISTS(SELECT 1 FROM "
                         "tasks WHERE queue_id=download_queues.id)");
        q.text(1, id);
        q.number(2, expectedRevision);
        if (sqlite3_step(q.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
            throw std::runtime_error("Queue changed, is unavailable or still contains tasks.");
    }
    void moveTasks(const std::vector<QueueMove>& moves, const std::string& target, std::int64_t updatedAt) {
        std::lock_guard lock(mutex_);
        if (!isQueueId(target) || moves.size() > 10000 || updatedAt < 0)
            throw std::invalid_argument("Invalid download queue move.");
        std::set<std::string> ids;
        for (const auto& move : moves)
            if (!isOpaqueId(move.taskId) || !isQueueId(move.expectedQueueId) ||
                !ids.insert(move.taskId).second)
                throw std::invalid_argument("Invalid or duplicate task in queue move.");
        execute("BEGIN IMMEDIATE");
        try {
            if (!queueExists(target))
                throw std::invalid_argument("Target queue is unavailable.");
            for (const auto& move : moves) {
                int state;
                {
                    Statement current(db_, "SELECT queue_id,state FROM tasks WHERE id=?");
                    current.text(1, move.taskId);
                    if (sqlite3_step(current.value) != SQLITE_ROW ||
                        columnText(current.value, 0) != move.expectedQueueId)
                        throw std::runtime_error("Task queue membership changed. Refresh before retrying.");
                    state = sqlite3_column_int(current.value, 1);
                    if (state < 0 || state >= static_cast<int>(TaskState::Count) ||
                        isActive(static_cast<TaskState>(state)))
                        throw std::invalid_argument(
                            "Pause active tasks before moving them to another queue.");
                }
                Statement update(
                    db_, "UPDATE tasks SET queue_id=?,updated_at=? WHERE id=? AND queue_id=? AND state=?");
                update.text(1, target);
                update.number(2, updatedAt);
                update.text(3, move.taskId);
                update.text(4, move.expectedQueueId);
                update.number(5, state);
                if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
                    throw std::runtime_error("Task queue membership changed. Refresh before retrying.");
            }
            execute("COMMIT");
        } catch (...) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }
};
class TaskRepository final : public ITaskRepository {
    std::shared_ptr<Backend> backend_;

  public:
    explicit TaskRepository(std::shared_ptr<Backend> backend) : backend_(std::move(backend)) {}
    std::vector<Task> load() override { return backend_->load(); }
    void save(const Task& task) override { backend_->save(task); }
    void erase(const std::string& id) override { backend_->erase(id); }
};
class ContextRepository final : public IContextRepository {
    std::shared_ptr<Backend> backend_;

  public:
    explicit ContextRepository(std::shared_ptr<Backend> backend) : backend_(std::move(backend)) {}
    std::string installationId() override { return backend_->installationId(); }
    std::vector<DownloadContext> loadContexts() override { return backend_->loadContexts(); }
    void saveContext(const DownloadContext& context) override { backend_->saveContext(context); }
    std::vector<CredentialRecord> loadCredentialRecords() override {
        return backend_->loadCredentialRecords();
    }
    void saveCredentialRecord(const CredentialRecord& record) override {
        backend_->saveCredentialRecord(record);
    }
    void eraseCredentialRecord(const std::string& reference) override {
        backend_->eraseCredentialRecord(reference);
    }
};
class QueueRepository final : public IQueueRepository {
    std::shared_ptr<Backend> backend_;

  public:
    explicit QueueRepository(std::shared_ptr<Backend> backend) : backend_(std::move(backend)) {}
    std::vector<DownloadQueue> loadQueues() override { return backend_->loadQueues(); }
    void saveQueue(const DownloadQueue& queue, std::int64_t expected) override {
        backend_->saveQueue(queue, expected);
    }
    void eraseQueue(const std::string& id, std::int64_t expected) override {
        backend_->eraseQueue(id, expected);
    }
    void moveTasks(const std::vector<QueueMove>& moves, const std::string& target,
                   std::int64_t updatedAt) override {
        backend_->moveTasks(moves, target, updatedAt);
    }
};
} // namespace
SqliteStore makeSqliteStore(const std::filesystem::path& path) {
    auto backend = std::make_shared<Backend>(path);
    return {std::make_unique<TaskRepository>(backend), std::make_unique<ContextRepository>(backend),
            std::make_unique<QueueRepository>(std::move(backend))};
}
std::unique_ptr<ITaskRepository> makeSqliteRepository(const std::filesystem::path& path) {
    return makeSqliteStore(path).tasks;
}
} // namespace maskpilot
