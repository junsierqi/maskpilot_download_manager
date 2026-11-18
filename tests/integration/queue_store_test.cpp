#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <sqlite3.h>

namespace {
struct QueueTemp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-queue-store-" + maskpilot::newTaskId());
    QueueTemp() { std::filesystem::create_directories(path); }
    ~QueueTemp() {
        std::error_code error;
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().string().starts_with("maskpilot-queue-store-"))
            std::filesystem::remove_all(path, error);
    }
};
maskpilot::Task task(const std::filesystem::path& root) {
    maskpilot::Task value;
    value.id = maskpilot::newTaskId();
    value.requestId = maskpilot::newTaskId();
    value.spec.url = "https://example.test/file";
    value.spec.destination = root / value.id;
    value.state = maskpilot::TaskState::Paused;
    value.runIntent = maskpilot::RunIntent::Paused;
    value.updatedAt = 13;
    return value;
}
maskpilot::DownloadQueue queue(const std::string& name) {
    maskpilot::DownloadQueue value;
    value.id = maskpilot::newTaskId();
    value.name = name;
    return value;
}
} // namespace

TEST(QueueStorage, MigratesRealVersionSixWithBackupAndPreservesScheduledIntentAndExistingFiles) {
    QueueTemp temporary;
    const auto path = temporary.path / "tasks.db";
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
    for (const auto* name : {"0001_initial.sql", "0002_media.sql", "0003_ftp.sql", "0004_torrents.sql",
                             "0005_proxy.sql", "0006_contexts.sql"}) {
        std::ifstream input(root / "database" / "migrations" / name);
        ASSERT_TRUE(input.good()) << name;
        const std::string sql((std::istreambuf_iterator<char>(input)), {});
        ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << name;
    }
    ASSERT_EQ(
        sqlite3_exec(
            raw,
            "INSERT INTO store_metadata VALUES('installation_id','dddddddddddddddddddddddddddddddd');"
            "INSERT INTO "
            "tasks(id,url,destination,category,referrer,user_agent,proxy,expected_sha256,connections,"
            "priority,max_retries,"
            "speed_limit,scheduled_at,stop_at,state,downloaded,total,created_at,updated_at,retries,resumable,"
            "error,owner_id,request_id,context_revision,proxy_credential_ref,origin_auth_required) "
            "VALUES('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','https://example.test/"
            "file','unchanged.bin','General','','MaskPilot','',"
            "'',4,0,3,0,2000000000,0,1,100,1000,1,1,0,1,'','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
            "'cccccccccccccccccccccccccccccccc',3,'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',1);"
            "INSERT INTO tasks SELECT "
            "'ffffffffffffffffffffffffffffffff',url,'paused.bin',category,referrer,user_agent,proxy,expected_"
            "sha256,connections,priority,max_retries,"
            "speed_limit,scheduled_at,stop_at,5,downloaded,total,created_at,updated_at,retries,resumable,"
            "error,media,audio_only,media_format,media_downloader,media_muxer,"
            "ftp_tls,ftp_active,ftp_ca,kind,bt_all,bt_selection,bt_seed,bt_upload_limit,bt_metadata,bt_"
            "published,bt_uploaded,proxy_auth_required,owner_id,"
            "'11111111111111111111111111111111',context_revision,proxy_credential_ref,origin_auth_required,"
            "request_name FROM tasks;",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    connection.reset();
    const auto part = temporary.path / "part-0";
    {
        std::ofstream output(part);
        output << "preserve durable bytes";
    }
    {
        auto store = maskpilot::makeSqliteStore(path);
        EXPECT_EQ(store.contexts->installationId(), "dddddddddddddddddddddddddddddddd");
        const auto queues = store.queues->loadQueues();
        ASSERT_EQ(queues.size(), 1);
        EXPECT_EQ(queues[0].id, maskpilot::defaultQueueId);
        EXPECT_EQ(queues[0].maxActive, 8);
        EXPECT_TRUE(queues[0].enabled);
        const auto tasks = store.tasks->load();
        ASSERT_EQ(tasks.size(), 2);
        for (const auto& item : tasks) {
            EXPECT_EQ(item.queueId, maskpilot::defaultQueueId);
            EXPECT_EQ(item.runIntent, item.state == maskpilot::TaskState::Scheduled
                                          ? maskpilot::RunIntent::Run
                                          : maskpilot::RunIntent::Paused);
            EXPECT_EQ(item.ownerId, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
            EXPECT_EQ(item.contextRevision, 3);
            EXPECT_EQ(item.proxyCredentialRef, "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
            EXPECT_TRUE(item.originAuthenticationRequired);
            EXPECT_EQ(item.downloaded, 100);
        }
        EXPECT_EQ(tasks[0].spec.destination, "unchanged.bin");
    }
    std::ifstream input(part);
    EXPECT_EQ(std::string((std::istreambuf_iterator<char>(input)), {}), "preserve durable bytes");
    int backups = 0;
    for (const auto& file : std::filesystem::directory_iterator(temporary.path)) {
        if (file.path().filename().string().find(".before-v7-") == std::string::npos)
            continue;
        ++backups;
        sqlite3* backup = nullptr;
        ASSERT_EQ(sqlite3_open_v2(file.path().string().c_str(), &backup, SQLITE_OPEN_READONLY, nullptr),
                  SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> close(backup, sqlite3_close);
        sqlite3_stmt* q = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(backup, "PRAGMA user_version", -1, &q, nullptr), SQLITE_OK);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(q, sqlite3_finalize);
        ASSERT_EQ(sqlite3_step(q), SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(q, 0), 6);
    }
    EXPECT_EQ(backups, 1);
    auto reopened = maskpilot::makeSqliteStore(path);
    EXPECT_EQ(reopened.tasks->load().size(), 2);
}

TEST(QueueStorage, PersistsQueueCASAndRejectsDuplicateNamesStaleWritesAndDefaultRemoval) {
    QueueTemp temporary;
    const auto path = temporary.path / "tasks.db";
    auto store = maskpilot::makeSqliteStore(path);
    auto first = queue("Work");
    first.maxActive = 2;
    first.downloadLimit = 4096;
    first.uploadLimit = 2048;
    store.queues->saveQueue(first, 0);
    EXPECT_THROW(store.queues->saveQueue(first, 0), std::exception);
    auto duplicate = queue("Work");
    EXPECT_THROW(store.queues->saveQueue(duplicate, 0), std::exception);
    auto stale = first;
    stale.revision = 3;
    EXPECT_THROW(store.queues->saveQueue(stale, 1), std::exception);
    ++first.revision;
    first.enabled = false;
    store.queues->saveQueue(first, 1);
    stale = first;
    stale.name = "Stale";
    EXPECT_THROW(store.queues->saveQueue(stale, 1), std::exception);
    EXPECT_THROW(store.queues->eraseQueue(maskpilot::defaultQueueId, 1), std::exception);
    store.tasks.reset();
    store.contexts.reset();
    EXPECT_EQ(store.queues->loadQueues().size(), 2);
    store.queues.reset();
    auto reopened = maskpilot::makeSqliteStore(path);
    const auto loaded = reopened.queues->loadQueues();
    const auto found =
        std::find_if(loaded.begin(), loaded.end(), [&](const auto& item) { return item.id == first.id; });
    ASSERT_NE(found, loaded.end());
    EXPECT_EQ(found->revision, 2);
    EXPECT_FALSE(found->enabled);
    EXPECT_EQ(found->downloadLimit, 4096);
    EXPECT_EQ(found->uploadLimit, 2048);
    EXPECT_THROW(reopened.queues->eraseQueue(first.id, 1), std::exception);
    reopened.queues->eraseQueue(first.id, 2);
    EXPECT_EQ(reopened.queues->loadQueues().size(), 1);
}

TEST(QueueStorage, MixedOwnerMoveCommitsAtomicallyAndFailedBatchRollsBackEveryMembershipAndTimestamp) {
    QueueTemp temporary;
    auto store = maskpilot::makeSqliteStore(temporary.path / "tasks.db");
    auto target = queue("Downloads");
    store.queues->saveQueue(target, 0);
    auto first = task(temporary.path), second = task(temporary.path);
    second.ownerId = maskpilot::newTaskId();
    second.contextRevision = 4;
    second.proxyCredentialRef = maskpilot::newTaskId();
    second.spec.proxy = "http://127.0.0.1:8080";
    second.originAuthenticationRequired = true;
    second.runIntent = maskpilot::RunIntent::Run;
    store.tasks->save(first);
    store.tasks->save(second);
    const std::vector<maskpilot::QueueMove> moves{{first.id, maskpilot::defaultQueueId},
                                                  {second.id, maskpilot::defaultQueueId}};
    auto mismatch = moves;
    mismatch[1].expectedQueueId = target.id;
    EXPECT_THROW(store.queues->moveTasks(mismatch, target.id, 99), std::exception);
    for (const auto& item : store.tasks->load()) {
        EXPECT_EQ(item.queueId, maskpilot::defaultQueueId);
        EXPECT_EQ(item.updatedAt, 13);
    }
    auto absent = moves;
    absent[1].taskId = maskpilot::newTaskId();
    EXPECT_THROW(store.queues->moveTasks(absent, target.id, 99), std::exception);
    EXPECT_THROW(store.queues->moveTasks(moves, maskpilot::newTaskId(), 99), std::exception);
    EXPECT_THROW(store.queues->moveTasks({moves[0], moves[0]}, target.id, 99), std::exception);
    second.state = maskpilot::TaskState::Downloading;
    store.tasks->save(second);
    EXPECT_THROW(store.queues->moveTasks(moves, target.id, 99), std::exception);
    EXPECT_EQ(store.tasks->load()[0].queueId, maskpilot::defaultQueueId);
    second.state = maskpilot::TaskState::Paused;
    store.tasks->save(second);
    store.queues->moveTasks(moves, target.id, 1234);
    const auto moved = store.tasks->load();
    ASSERT_EQ(moved.size(), 2);
    for (const auto& item : moved) {
        EXPECT_EQ(item.queueId, target.id);
        EXPECT_EQ(item.updatedAt, 1234);
    }
    const auto found =
        std::find_if(moved.begin(), moved.end(), [&](const auto& item) { return item.id == second.id; });
    ASSERT_NE(found, moved.end());
    EXPECT_EQ(found->ownerId, second.ownerId);
    EXPECT_EQ(found->requestId, second.requestId);
    EXPECT_EQ(found->contextRevision, second.contextRevision);
    EXPECT_EQ(found->proxyCredentialRef, second.proxyCredentialRef);
    EXPECT_EQ(found->spec.destination, second.spec.destination);
    EXPECT_EQ(found->spec.proxy, second.spec.proxy);
    EXPECT_EQ(found->runIntent, maskpilot::RunIntent::Run);
    EXPECT_TRUE(found->originAuthenticationRequired);
    EXPECT_THROW(store.queues->eraseQueue(target.id, 1), std::exception);
    first.queueId = maskpilot::newTaskId();
    EXPECT_THROW(store.tasks->save(first), std::exception);
    first.queueId = target.id;
    first.runIntent = static_cast<maskpilot::RunIntent>(99);
    EXPECT_THROW(store.tasks->save(first), std::exception);
    store.tasks->erase(first.id);
    store.tasks->erase(second.id);
    store.queues->eraseQueue(target.id, 1);
    EXPECT_EQ(store.queues->loadQueues().size(), 1);
}
