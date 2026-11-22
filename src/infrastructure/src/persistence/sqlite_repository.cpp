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
void validateTransferConstraints(const Task& task) {
    const auto& spec = task.spec;
    if (!validateMediaOrigins(spec).empty() || (!spec.mediaOrigins.empty() && task.ownerId != "standalone"))
        throw std::invalid_argument("Invalid persistent media origin policy.");
    if (!validateSftpOptions(spec.sftp, isSftpUrl(spec.url)).empty() ||
        (isSftpUrl(spec.url) && (task.ownerId != "standalone" || spec.kind == TransferKind::Media ||
                                 spec.kind == TransferKind::BitTorrent)) ||
        (spec.kind == TransferKind::Sftp && !isSftpUrl(spec.url)) || static_cast<int>(spec.kind) < 0 ||
        static_cast<int>(spec.kind) > 5)
        throw std::invalid_argument("Invalid persistent SFTP configuration.");
    if (!validateLiveRecording(spec.live).empty() ||
        (spec.kind != TransferKind::Live && spec.live != LiveRecordingOptions{}) ||
        (spec.kind == TransferKind::Live &&
         (task.ownerId != "standalone" || !task.projectId.empty() || !validateSpec(spec).empty())))
        throw std::invalid_argument("Invalid persistent live recording configuration.");
    if (!validateHttpRequest(spec.httpRequest).empty() ||
        (spec.httpRequest.method == HttpMethod::Post &&
         (sourceKind(spec) != TransferKind::Http || task.ownerId != "standalone" || !task.projectId.empty())))
        throw std::invalid_argument("Invalid persistent HTTP request metadata.");
    if (task.projectId.empty()
            ? task.projectRevision != 0
            : (!isOpaqueId(task.projectId) || task.projectRevision < 1 || task.projectRevision > 1000000 ||
               task.ownerId != "standalone" || sourceKind(spec) != TransferKind::Http ||
               spec.maxStoredBytes <= 0 || !spec.sameOriginRedirectOnly ||
               !(spec.url.starts_with("http://") || spec.url.starts_with("https://"))))
        throw std::invalid_argument("Invalid persistent project child association.");
    if (!validHttpResponseContentType(task.responseContentType) ||
        (!task.responseContentType.empty() &&
         (sourceKind(spec) != TransferKind::Http || task.responseUrl.empty())))
        throw std::invalid_argument("Invalid persistent HTTP response content type.");
    if (spec.maxStoredBytes < 0 || spec.maxStoredBytes > 9007199254740991LL ||
        ((spec.maxStoredBytes || spec.sameOriginRedirectOnly) &&
         (sourceKind(spec) != TransferKind::Http ||
          !(spec.url.starts_with("http://") || spec.url.starts_with("https://")))))
        throw std::invalid_argument("Invalid persistent HTTP transfer constraints.");
    if (task.responseUrl.empty())
        return;
    if (sourceKind(spec) != TransferKind::Http || task.responseUrl.size() > 32768 ||
        !(task.responseUrl.starts_with("http://") || task.responseUrl.starts_with("https://")) ||
        std::any_of(task.responseUrl.begin(), task.responseUrl.end(),
                    [](unsigned char c) { return c <= 32 || c == 127; }))
        throw std::invalid_argument("Invalid persistent HTTP response URL.");
    DownloadSpec observed;
    observed.url = task.responseUrl;
    observed.destination = "observed.bin";
    if (!validateSpec(observed).empty())
        throw std::invalid_argument("Invalid persistent HTTP response URL.");
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
    template <typename Operation> auto trafficTransaction(Operation operation) {
        execute("BEGIN IMMEDIATE");
        try {
            auto result = operation();
            execute("COMMIT");
            return result;
        } catch (...) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }
    std::int64_t trafficPeriod(std::int64_t utc) {
        if (utc < 0)
            throw std::invalid_argument("Invalid traffic accounting time.");
        Statement q(db_, "SELECT period FROM traffic_clock WHERE singleton=1");
        if (sqlite3_step(q.value) != SQLITE_ROW)
            throw std::runtime_error("Traffic accounting clock is unavailable.");
        const auto floor = sqlite3_column_int64(q.value, 0);
        if (floor < 0 || floor % 86400 != 0)
            throw std::runtime_error("Traffic accounting clock is invalid.");
        const auto period = std::max<std::int64_t>(floor, utc / 86400 * 86400);
        if (period > floor) {
            Statement update(db_, "UPDATE traffic_clock SET period=? WHERE singleton=1");
            update.number(1, period);
            if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
                throw std::runtime_error("Cannot advance traffic accounting clock.");
        }
        return period;
    }
    std::int64_t trafficInstant(std::int64_t utc) {
        if (utc < 0 || utc > 253402300799LL)
            throw std::invalid_argument("Invalid traffic accounting instant.");
        Statement query(db_, "SELECT utc_highwater FROM traffic_clock WHERE singleton=1");
        if (sqlite3_step(query.value) != SQLITE_ROW || sqlite3_column_type(query.value, 0) != SQLITE_INTEGER)
            throw std::runtime_error("Traffic accounting highwater is unavailable.");
        const auto saved = sqlite3_column_int64(query.value, 0);
        if (saved < 0 || saved > 253402300799LL)
            throw std::runtime_error("Traffic accounting highwater is invalid.");
        const auto now = std::max<std::int64_t>(saved, utc);
        if (now != saved) {
            Statement update(db_, "UPDATE traffic_clock SET utc_highwater=? WHERE singleton=1");
            update.number(1, now);
            if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
                throw std::runtime_error("Cannot advance traffic accounting highwater.");
        }
        return now;
    }
    int payloadWindow(const std::string& queue) {
        Statement policy(db_, "SELECT payload_window_hours FROM download_queues WHERE id=?");
        policy.text(1, queue);
        const auto status = sqlite3_step(policy.value);
        if (status == SQLITE_DONE)
            return 0; // Removed queues retain their historical UTC-day ledger, as before.
        if (status != SQLITE_ROW)
            throw std::runtime_error("Traffic queue is unavailable.");
        const auto hours = sqlite3_column_int64(policy.value, 0);
        if (sqlite3_column_type(policy.value, 0) != SQLITE_INTEGER || hours < 0 || hours > 168)
            throw std::runtime_error("Traffic window is invalid.");
        return static_cast<int>(hours);
    }
    TrafficUsage rollingUsage(const std::string& queue, std::int64_t now, int hours) {
        Statement query(db_, "SELECT COALESCE(SUM(charge),0),COALESCE(SUM(received),0) FROM ("
                             "SELECT reserved AS charge,observed AS received FROM queue_traffic_runs "
                             "WHERE queue_id=? AND status=0 UNION ALL "
                             "SELECT observed,observed FROM queue_traffic_runs WHERE queue_id=? AND status=1 "
                             "AND closed_utc>? UNION ALL "
                             "SELECT reserved,observed FROM queue_traffic_runs WHERE queue_id=? AND status=2 "
                             "AND closed_utc>?)");
        query.text(1, queue);
        query.text(2, queue);
        query.number(3, now - hours * 3600LL);
        query.text(4, queue);
        query.number(5, now - hours * 3600LL);
        if (sqlite3_step(query.value) != SQLITE_ROW ||
            sqlite3_column_type(query.value, 0) != SQLITE_INTEGER ||
            sqlite3_column_type(query.value, 1) != SQLITE_INTEGER)
            throw std::runtime_error("Cannot inspect rolling traffic usage.");
        TrafficUsage usage{now, sqlite3_column_int64(query.value, 0), sqlite3_column_int64(query.value, 1)};
        if (usage.charged < 0 || usage.observed < 0 || usage.observed > usage.charged)
            throw std::runtime_error("Rolling traffic usage is invalid.");
        return usage;
    }
    struct RunRow {
        std::string queue;
        std::int64_t period, reserved, observed;
        int status;
    };
    RunRow trafficRun(const std::string& id) {
        if (!isOpaqueId(id))
            throw std::invalid_argument("Invalid traffic run identity.");
        Statement q(db_,
                    "SELECT queue_id,period,reserved,observed,status FROM queue_traffic_runs WHERE id=?");
        q.text(1, id);
        if (sqlite3_step(q.value) != SQLITE_ROW)
            throw std::invalid_argument("Traffic run is unavailable.");
        RunRow run{columnText(q.value, 0), sqlite3_column_int64(q.value, 1), sqlite3_column_int64(q.value, 2),
                   sqlite3_column_int64(q.value, 3), sqlite3_column_int(q.value, 4)};
        if (!isQueueId(run.queue) || run.period < 0 || run.period % 86400 || run.reserved < 0 ||
            run.reserved > maxDailyPayloadBudget || run.observed < 0 || run.observed > run.reserved ||
            run.status < 0 || run.status > 2)
            throw std::runtime_error("Traffic run data is invalid.");
        return run;
    }
    TrafficUsage trafficUsage(const std::string& queue, std::int64_t period) {
        Statement q(db_, "SELECT charged,observed FROM queue_traffic_usage WHERE queue_id=? AND period=?");
        q.text(1, queue);
        q.number(2, period);
        const auto status = sqlite3_step(q.value);
        if (status == SQLITE_DONE)
            return {period, 0, 0};
        if (status != SQLITE_ROW)
            throw std::runtime_error("Cannot read traffic usage.");
        TrafficUsage usage{period, sqlite3_column_int64(q.value, 0), sqlite3_column_int64(q.value, 1)};
        if (usage.charged < 0 || usage.observed < 0 || usage.observed > usage.charged)
            throw std::runtime_error("Traffic usage data is invalid.");
        return usage;
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
                if (sqlite3_step(version.value) != SQLITE_ROW || sqlite3_column_int(version.value, 0) > 19 ||
                    sqlite3_column_int(version.value, 0) < 0)
                    throw std::runtime_error("Database was created by a newer version.");
                schemaVersion = sqlite3_column_int(version.value, 0);
            }
            if (schemaVersion > 0 && schemaVersion < 19) {
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
            if (schemaVersion < 9)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE download_queues ADD COLUMN daily_payload_budget INTEGER NOT NULL DEFAULT 0 CHECK(daily_payload_budget BETWEEN 0 AND 1125899906842624);
CREATE TABLE traffic_clock(singleton INTEGER PRIMARY KEY CHECK(singleton=1),period INTEGER NOT NULL CHECK(period>=0 AND period%86400=0));
INSERT INTO traffic_clock VALUES(1,0);
CREATE TABLE queue_traffic_usage(queue_id TEXT NOT NULL,period INTEGER NOT NULL CHECK(period>=0 AND period%86400=0),
 charged INTEGER NOT NULL DEFAULT 0 CHECK(charged BETWEEN 0 AND 1125899906842624),
 observed INTEGER NOT NULL DEFAULT 0 CHECK(observed>=0 AND observed<=charged),PRIMARY KEY(queue_id,period));
CREATE TABLE queue_traffic_runs(id TEXT PRIMARY KEY,task_id TEXT NOT NULL,queue_id TEXT NOT NULL,
 period INTEGER NOT NULL CHECK(period>=0 AND period%86400=0),status INTEGER NOT NULL DEFAULT 0 CHECK(status IN(0,1,2)),
 reserved INTEGER NOT NULL DEFAULT 0 CHECK(reserved BETWEEN 0 AND 1125899906842624),
 observed INTEGER NOT NULL DEFAULT 0 CHECK(observed>=0 AND observed<=reserved));
CREATE INDEX traffic_runs_status ON queue_traffic_runs(status);
PRAGMA user_version=9;
COMMIT;)sql");
            if (schemaVersion < 10)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN media_selection_mode INTEGER NOT NULL DEFAULT 0 CHECK(media_selection_mode BETWEEN 0 AND 4);
ALTER TABLE tasks ADD COLUMN media_primary_format TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN media_audio_format TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN media_container INTEGER NOT NULL DEFAULT 0 CHECK(media_container BETWEEN 0 AND 7);
PRAGMA user_version=10;
COMMIT;)sql");
            if (schemaVersion < 11)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN max_stored_bytes INTEGER NOT NULL DEFAULT 0 CHECK(max_stored_bytes BETWEEN 0 AND 9007199254740991);
ALTER TABLE tasks ADD COLUMN same_origin_redirect_only INTEGER NOT NULL DEFAULT 0 CHECK(same_origin_redirect_only IN (0,1));
ALTER TABLE tasks ADD COLUMN response_url TEXT NOT NULL DEFAULT '';
PRAGMA user_version=11;
COMMIT;)sql");
            if (schemaVersion < 12)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN response_content_type TEXT NOT NULL DEFAULT '';
PRAGMA user_version=12;
COMMIT;)sql");
            if (schemaVersion < 13)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN project_id TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN project_revision INTEGER NOT NULL DEFAULT 0;
PRAGMA user_version=13;
COMMIT;)sql");
            if (schemaVersion < 14)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE download_queues ADD COLUMN payload_window_hours INTEGER NOT NULL DEFAULT 0 CHECK(payload_window_hours BETWEEN 0 AND 168);
ALTER TABLE queue_traffic_runs ADD COLUMN closed_utc INTEGER NOT NULL DEFAULT 0 CHECK(closed_utc>=0);
UPDATE queue_traffic_runs SET closed_utc=period+86400 WHERE status<>0;
ALTER TABLE traffic_clock ADD COLUMN utc_highwater INTEGER NOT NULL DEFAULT 0 CHECK(utc_highwater>=0);
UPDATE traffic_clock SET utc_highwater=period;
ALTER TABLE queue_traffic_usage RENAME TO queue_traffic_usage_v13;
CREATE TABLE queue_traffic_usage(queue_id TEXT NOT NULL,period INTEGER NOT NULL CHECK(period>=0 AND period%86400=0),
 charged INTEGER NOT NULL DEFAULT 0 CHECK(charged>=0),
 observed INTEGER NOT NULL DEFAULT 0 CHECK(observed>=0 AND observed<=charged),PRIMARY KEY(queue_id,period));
INSERT INTO queue_traffic_usage SELECT * FROM queue_traffic_usage_v13;
DROP TABLE queue_traffic_usage_v13;
CREATE INDEX traffic_runs_queue_close ON queue_traffic_runs(queue_id,status,closed_utc);
PRAGMA user_version=14;
COMMIT;)sql");
            if (schemaVersion < 15)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN scan_required INTEGER NOT NULL DEFAULT 0 CHECK(scan_required IN(0,1));
ALTER TABLE tasks ADD COLUMN scan_state INTEGER NOT NULL DEFAULT 0 CHECK(scan_state BETWEEN 0 AND 6);
ALTER TABLE tasks ADD COLUMN scan_at INTEGER NOT NULL DEFAULT 0 CHECK(scan_at BETWEEN 0 AND 253402300799);
PRAGMA user_version=15;
COMMIT;)sql");
            if (schemaVersion < 16)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN http_method INTEGER NOT NULL DEFAULT 0 CHECK(http_method IN(0,1));
ALTER TABLE tasks ADD COLUMN post_content_type TEXT NOT NULL DEFAULT 'application/x-www-form-urlencoded';
PRAGMA user_version=16;
COMMIT;)sql");
            if (schemaVersion < 17)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN sftp_authentication INTEGER NOT NULL DEFAULT 0 CHECK(sftp_authentication IN(0,1));
ALTER TABLE tasks ADD COLUMN sftp_host_key_sha256 TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN sftp_private_key TEXT NOT NULL DEFAULT '';
PRAGMA user_version=17;
COMMIT;)sql");
            if (schemaVersion < 18)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN live_format INTEGER NOT NULL DEFAULT 0 CHECK(live_format IN(0,1));
ALTER TABLE tasks ADD COLUMN live_duration INTEGER NOT NULL DEFAULT 3600;
ALTER TABLE tasks ADD COLUMN live_max_bytes INTEGER NOT NULL DEFAULT 1073741824;
PRAGMA user_version=18;
COMMIT;)sql");
            if (schemaVersion < 19)
                execute(R"sql(BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN media_origins TEXT NOT NULL DEFAULT '';
PRAGMA user_version=19;
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
            t.spec.mediaSelection.mode = static_cast<MediaSelectionMode>(sqlite3_column_int(q.value, 47));
            t.spec.mediaSelection.primaryFormat = text(48);
            t.spec.mediaSelection.audioFormat = text(49);
            t.spec.mediaSelection.container = static_cast<MediaContainer>(sqlite3_column_int(q.value, 50));
            if (sqlite3_column_type(q.value, 51) != SQLITE_INTEGER ||
                sqlite3_column_type(q.value, 52) != SQLITE_INTEGER || sqlite3_column_int64(q.value, 52) < 0 ||
                sqlite3_column_int64(q.value, 52) > 1 || sqlite3_column_type(q.value, 53) != SQLITE_TEXT)
                throw std::runtime_error("Saved HTTP transfer constraints have invalid types.");
            t.spec.maxStoredBytes = sqlite3_column_int64(q.value, 51);
            t.spec.sameOriginRedirectOnly = sqlite3_column_int(q.value, 52) != 0;
            t.responseUrl = text(53);
            if (sqlite3_column_type(q.value, 54) != SQLITE_TEXT)
                throw std::runtime_error("Saved HTTP content type has an invalid type.");
            t.responseContentType = text(54);
            if (sqlite3_column_type(q.value, 55) != SQLITE_TEXT ||
                sqlite3_column_type(q.value, 56) != SQLITE_INTEGER)
                throw std::runtime_error("Saved project association has invalid types.");
            t.projectId = text(55);
            t.projectRevision = sqlite3_column_int64(q.value, 56);
            if (sqlite3_column_type(q.value, 57) != SQLITE_INTEGER || sqlite3_column_int64(q.value, 57) < 0 ||
                sqlite3_column_int64(q.value, 57) > 1 || sqlite3_column_type(q.value, 58) != SQLITE_INTEGER ||
                sqlite3_column_int64(q.value, 58) < 0 ||
                sqlite3_column_int64(q.value, 58) >= static_cast<int>(ScanState::Count) ||
                sqlite3_column_type(q.value, 59) != SQLITE_INTEGER)
                throw std::runtime_error("Saved completion scan fields have invalid types.");
            t.scanRequired = sqlite3_column_int(q.value, 57) != 0;
            t.scanState = static_cast<ScanState>(sqlite3_column_int(q.value, 58));
            t.scanAt = sqlite3_column_int64(q.value, 59);
            if (sqlite3_column_type(q.value, 60) != SQLITE_INTEGER || sqlite3_column_int64(q.value, 60) < 0 ||
                sqlite3_column_int64(q.value, 60) > 1 || sqlite3_column_type(q.value, 61) != SQLITE_TEXT)
                throw std::runtime_error("Saved HTTP request fields have invalid types.");
            t.spec.httpRequest.method = static_cast<HttpMethod>(sqlite3_column_int(q.value, 60));
            t.spec.httpRequest.contentType = text(61);
            if (sqlite3_column_type(q.value, 62) != SQLITE_INTEGER || sqlite3_column_int64(q.value, 62) < 0 ||
                sqlite3_column_int64(q.value, 62) > 1 || sqlite3_column_type(q.value, 63) != SQLITE_TEXT ||
                sqlite3_column_type(q.value, 64) != SQLITE_TEXT)
                throw std::runtime_error("Saved SFTP fields have invalid types.");
            t.spec.sftp.authentication = static_cast<SftpAuthentication>(sqlite3_column_int(q.value, 62));
            t.spec.sftp.hostKeySha256 = text(63);
            const auto keyPath = text(64);
            t.spec.sftp.privateKeyFile = std::filesystem::path(std::u8string(keyPath.begin(), keyPath.end()));
            for (int column : {65, 66, 67})
                if (sqlite3_column_type(q.value, column) != SQLITE_INTEGER)
                    throw std::runtime_error("Saved live fields have invalid types.");
            const auto liveFormat = sqlite3_column_int64(q.value, 65);
            const auto liveDuration = sqlite3_column_int64(q.value, 66);
            if (liveFormat < 0 || liveFormat > 1 || liveDuration < 1 || liveDuration > 172800)
                throw std::runtime_error("Saved live fields are outside their bounds.");
            t.spec.live.format = static_cast<LiveFormat>(liveFormat);
            t.spec.live.durationSeconds = static_cast<int>(liveDuration);
            t.spec.live.maxBytes = sqlite3_column_int64(q.value, 67);
            if (sqlite3_column_type(q.value, 68) != SQLITE_TEXT)
                throw std::runtime_error("Saved media origins have an invalid type.");
            t.spec.mediaOrigins = decodeMediaOrigins(text(68));
            if (const auto message = validateCompletionScanRecord(t); !message.empty())
                throw std::runtime_error(message);
            validateTransferConstraints(t);
            if (!validateMediaSelection(t.spec.mediaSelection, t.spec.audioOnly).empty())
                throw std::runtime_error("Saved media selection is invalid.");
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
        validateTransferConstraints(t);
        if (const auto message = validateCompletionScanRecord(t); !message.empty())
            throw std::invalid_argument(message);
        if (!validateMediaSelection(t.spec.mediaSelection, t.spec.audioOnly).empty())
            throw std::invalid_argument("Invalid persistent media selection.");
        if (!queueExists(t.queueId))
            throw std::invalid_argument("Task download queue is unavailable.");
        Statement q(db_, R"sql(INSERT INTO tasks(
id,url,destination,category,referrer,user_agent,proxy,expected_sha256,connections,priority,max_retries,
speed_limit,scheduled_at,stop_at,state,downloaded,total,created_at,updated_at,retries,resumable,error,
media,audio_only,media_format,media_downloader,media_muxer,ftp_tls,ftp_active,ftp_ca,kind,bt_all,bt_selection,
bt_seed,bt_upload_limit,bt_metadata,bt_published,bt_uploaded,proxy_auth_required,owner_id,request_id,
context_revision,proxy_credential_ref,origin_auth_required,request_name,queue_id,run_intent,
media_selection_mode,media_primary_format,media_audio_format,media_container,max_stored_bytes,same_origin_redirect_only,response_url,response_content_type,project_id,project_revision,scan_required,scan_state,scan_at,http_method,post_content_type,sftp_authentication,sftp_host_key_sha256,sftp_private_key,live_format,live_duration,live_max_bytes,media_origins)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
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
queue_id=excluded.queue_id,run_intent=excluded.run_intent,media_selection_mode=excluded.media_selection_mode,
media_primary_format=excluded.media_primary_format,media_audio_format=excluded.media_audio_format,media_container=excluded.media_container,
max_stored_bytes=excluded.max_stored_bytes,same_origin_redirect_only=excluded.same_origin_redirect_only,response_url=excluded.response_url,
response_content_type=excluded.response_content_type,scan_required=excluded.scan_required,
scan_state=excluded.scan_state,scan_at=excluded.scan_at,
http_method=excluded.http_method,post_content_type=excluded.post_content_type,
sftp_authentication=excluded.sftp_authentication,sftp_host_key_sha256=excluded.sftp_host_key_sha256,
sftp_private_key=excluded.sftp_private_key,live_format=excluded.live_format,
live_duration=excluded.live_duration,live_max_bytes=excluded.live_max_bytes,media_origins=excluded.media_origins
WHERE tasks.owner_id=excluded.owner_id AND tasks.project_id=excluded.project_id AND tasks.project_revision=excluded.project_revision)sql");
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
        q.number(48, static_cast<int>(t.spec.mediaSelection.mode));
        q.text(49, t.spec.mediaSelection.primaryFormat);
        q.text(50, t.spec.mediaSelection.audioFormat);
        q.number(51, static_cast<int>(t.spec.mediaSelection.container));
        q.number(52, t.spec.maxStoredBytes);
        q.number(53, t.spec.sameOriginRedirectOnly);
        q.text(54, t.responseUrl);
        q.text(55, t.responseContentType);
        q.text(56, t.projectId);
        q.number(57, t.projectRevision);
        q.number(58, t.scanRequired);
        q.number(59, static_cast<int>(t.scanState));
        q.number(60, t.scanAt);
        q.number(61, static_cast<int>(t.spec.httpRequest.method));
        q.text(62, t.spec.httpRequest.contentType);
        q.number(63, static_cast<int>(t.spec.sftp.authentication));
        q.text(64, t.spec.sftp.hostKeySha256.empty() ? std::string{}
                                                     : canonicalSftpHostKey(t.spec.sftp.hostKeySha256));
        q.text(65, utf8(t.spec.sftp.privateKeyFile));
        q.number(66, static_cast<int>(t.spec.live.format));
        q.number(67, t.spec.live.durationSeconds);
        q.number(68, t.spec.live.maxBytes);
        q.text(69, encodeMediaOrigins(t.spec.mediaOrigins));
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
                         "schedule_enabled,schedule_weekdays,schedule_start,schedule_end,schedule_timezone,"
                         "daily_payload_budget,payload_window_hours "
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
            queue.dailyPayloadBudget = sqlite3_column_int64(q.value, 12);
            queue.payloadWindowHours = sqlite3_column_int(q.value, 13);
            if (sqlite3_column_type(q.value, 13) != SQLITE_INTEGER || sqlite3_column_int64(q.value, 13) < 0 ||
                sqlite3_column_int64(q.value, 13) > 168)
                throw std::runtime_error("Saved native payload window is invalid.");
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
        Statement q(
            db_,
            expectedRevision == 0
                ? "INSERT INTO "
                  "download_queues(id,name,revision,enabled,max_active,"
                  "download_limit,upload_limit,schedule_enabled,schedule_weekdays,"
                  "schedule_start,schedule_end,schedule_timezone,daily_payload_budget,payload_window_hours) "
                  "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
                : "UPDATE download_queues SET "
                  "name=?,revision=?,enabled=?,max_active=?,download_limit=?,"
                  "upload_limit=?,schedule_enabled=?,schedule_weekdays=?,schedule_start=?,"
                  "schedule_end=?,schedule_timezone=?,daily_payload_budget=?,payload_window_hours=? WHERE "
                  "id=? AND revision=?");
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
            q.number(13, queue.dailyPayloadBudget);
            q.number(14, queue.payloadWindowHours);
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
            q.number(12, queue.dailyPayloadBudget);
            q.number(13, queue.payloadWindowHours);
            q.text(14, queue.id);
            q.number(15, expectedRevision);
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
    TrafficRun openRun(const std::string& taskId, const std::string& queue, std::int64_t utc) {
        std::lock_guard lock(mutex_);
        if (!isOpaqueId(taskId) || !isQueueId(queue))
            throw std::invalid_argument("Invalid traffic run binding.");
        return trafficTransaction([&] {
            Statement policy(db_, "SELECT q.daily_payload_budget FROM download_queues q JOIN tasks t ON "
                                  "t.queue_id=q.id WHERE t.id=? AND q.id=?");
            policy.text(1, taskId);
            policy.text(2, queue);
            if (sqlite3_step(policy.value) != SQLITE_ROW || sqlite3_column_int64(policy.value, 0) <= 0 ||
                sqlite3_column_int64(policy.value, 0) > maxDailyPayloadBudget)
                throw std::invalid_argument("Task queue has no available native payload budget.");
            const auto now = trafficInstant(utc);
            TrafficRun run{newTaskId(), trafficPeriod(now), payloadWindow(queue) != 0};
            Statement usage(db_, "INSERT OR IGNORE INTO queue_traffic_usage(queue_id,period) VALUES(?,?)");
            usage.text(1, queue);
            usage.number(2, run.period);
            if (sqlite3_step(usage.value) != SQLITE_DONE)
                throw std::runtime_error("Cannot initialize traffic usage.");
            Statement insert(db_,
                             "INSERT INTO queue_traffic_runs(id,task_id,queue_id,period) VALUES(?,?,?,?)");
            insert.text(1, run.id);
            insert.text(2, taskId);
            insert.text(3, queue);
            insert.number(4, run.period);
            if (sqlite3_step(insert.value) != SQLITE_DONE)
                throw std::runtime_error("Cannot create traffic run.");
            return run;
        });
    }
    TrafficGrant reserve(const std::string& id, std::int64_t observed, std::int64_t desired,
                         std::int64_t utc) {
        std::lock_guard lock(mutex_);
        if (observed < 0 || desired < 0 || desired > maxDailyPayloadBudget)
            throw std::invalid_argument("Invalid traffic reservation counters.");
        return trafficTransaction([&] {
            const auto run = trafficRun(id);
            if (run.status != 0 || observed > run.reserved)
                throw std::invalid_argument("Traffic run is closed or observation exceeds issued credits.");
            const auto now = trafficInstant(utc);
            const auto period = trafficPeriod(now);
            const auto hours = payloadWindow(run.queue);
            const auto usage =
                hours ? rollingUsage(run.queue, now, hours) : trafficUsage(run.queue, run.period);
            const auto highwater = std::max(run.observed, observed);
            auto additional = std::int64_t{0};
            if ((hours || period == run.period) && desired > run.reserved) {
                Statement policy(db_, "SELECT daily_payload_budget FROM download_queues WHERE id=?");
                policy.text(1, run.queue);
                if (sqlite3_step(policy.value) != SQLITE_ROW)
                    throw std::invalid_argument("Traffic queue policy is unavailable.");
                const auto budget = sqlite3_column_int64(policy.value, 0);
                if (budget <= 0 || budget > maxDailyPayloadBudget)
                    throw std::invalid_argument("Traffic queue budget is disabled or invalid.");
                additional =
                    std::min(desired - run.reserved, std::max<std::int64_t>(0, budget - usage.charged));
            }
            Statement updateUsage(db_, "UPDATE queue_traffic_usage SET charged=charged+?,observed=observed+? "
                                       "WHERE queue_id=? AND period=?");
            updateUsage.number(1, additional);
            updateUsage.number(2, highwater - run.observed);
            updateUsage.text(3, run.queue);
            updateUsage.number(4, run.period);
            if (sqlite3_step(updateUsage.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
                throw std::runtime_error("Cannot commit traffic usage.");
            Statement updateRun(
                db_, "UPDATE queue_traffic_runs SET reserved=?,observed=? WHERE id=? AND status=0");
            updateRun.number(1, run.reserved + additional);
            updateRun.number(2, highwater);
            updateRun.text(3, id);
            if (sqlite3_step(updateRun.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
                throw std::runtime_error("Cannot commit traffic reservation.");
            return TrafficGrant{run.reserved + additional, period};
        });
    }
    void closeRun(const std::string& id, std::int64_t finalObserved, std::int64_t utc = 0) {
        std::lock_guard lock(mutex_);
        if (finalObserved < 0)
            throw std::invalid_argument("Invalid final traffic observation.");
        trafficTransaction([&] {
            const auto now = trafficInstant(utc);
            const auto run = trafficRun(id);
            if (finalObserved > run.reserved || run.status == 2)
                throw std::invalid_argument("Traffic run cannot be reconciled.");
            if (run.status == 1) {
                if (finalObserved > run.observed)
                    throw std::invalid_argument("Closed traffic observation cannot be advanced.");
                return 0;
            }
            const auto highwater = std::max(run.observed, finalObserved);
            Statement usage(db_, "UPDATE queue_traffic_usage SET charged=charged-?,observed=observed+? WHERE "
                                 "queue_id=? AND period=?");
            usage.number(1, run.reserved - highwater);
            usage.number(2, highwater - run.observed);
            usage.text(3, run.queue);
            usage.number(4, run.period);
            if (sqlite3_step(usage.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
                throw std::runtime_error("Cannot reconcile traffic usage.");
            Statement close(
                db_,
                "UPDATE queue_traffic_runs SET observed=?,status=1,closed_utc=? WHERE id=? AND status=0");
            close.number(1, highwater);
            close.number(2, now);
            close.text(3, id);
            if (sqlite3_step(close.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
                throw std::runtime_error("Cannot close traffic run.");
            return 0;
        });
    }
    TrafficUsage usage(const std::string& queue, std::int64_t utc) {
        std::lock_guard lock(mutex_);
        if (!isQueueId(queue))
            throw std::invalid_argument("Invalid traffic queue identity.");
        return trafficTransaction([&] {
            const auto now = trafficInstant(utc);
            const auto period = trafficPeriod(now);
            const auto hours = payloadWindow(queue);
            return hours ? rollingUsage(queue, now, hours) : trafficUsage(queue, period);
        });
    }
    std::int64_t currentPeriod(std::int64_t utc) {
        std::lock_guard lock(mutex_);
        return trafficTransaction([&] { return trafficPeriod(trafficInstant(utc)); });
    }
    void abandonOpenRuns(std::int64_t utc = 0) {
        std::lock_guard lock(mutex_);
        trafficTransaction([&] {
            const auto now = trafficInstant(utc);
            Statement abandon(db_, "UPDATE queue_traffic_runs SET status=2,closed_utc=? WHERE status=0");
            abandon.number(1, now);
            if (sqlite3_step(abandon.value) != SQLITE_DONE)
                throw std::runtime_error("Cannot preserve abandoned traffic reservations.");
            return 0;
        });
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
class TrafficRepository final : public IQueueTrafficRepository {
    std::shared_ptr<Backend> backend_;

  public:
    explicit TrafficRepository(std::shared_ptr<Backend> backend) : backend_(std::move(backend)) {}
    TrafficRun openRun(const std::string& task, const std::string& queue, std::int64_t utc) override {
        return backend_->openRun(task, queue, utc);
    }
    TrafficGrant reserve(const std::string& run, std::int64_t observed, std::int64_t desired,
                         std::int64_t utc) override {
        return backend_->reserve(run, observed, desired, utc);
    }
    void closeRun(const std::string& run, std::int64_t observed) override {
        backend_->closeRun(run, observed);
    }
    void closeRunAt(const std::string& run, std::int64_t observed, std::int64_t utc) override {
        backend_->closeRun(run, observed, utc);
    }
    TrafficUsage usage(const std::string& queue, std::int64_t utc) override {
        return backend_->usage(queue, utc);
    }
    std::int64_t currentPeriod(std::int64_t utc) override { return backend_->currentPeriod(utc); }
    void abandonOpenRuns() override { backend_->abandonOpenRuns(); }
    void abandonOpenRunsAt(std::int64_t utc) override { backend_->abandonOpenRuns(utc); }
};
} // namespace
SqliteStore makeSqliteStore(const std::filesystem::path& path) {
    auto backend = std::make_shared<Backend>(path);
    return {std::make_unique<TaskRepository>(backend), std::make_unique<ContextRepository>(backend),
            std::make_unique<QueueRepository>(backend),
            std::make_shared<TrafficRepository>(std::move(backend))};
}
std::unique_ptr<ITaskRepository> makeSqliteRepository(const std::filesystem::path& path) {
    return makeSqliteStore(path).tasks;
}
} // namespace maskpilot
