#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <maskpilot/infrastructure/adapters.h>
#include <sqlite3.h>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

using namespace maskpilot;
namespace {
struct ProjectTemp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-project-store-" + newTaskId());
    ProjectTemp() { std::filesystem::create_directories(path); }
    ~ProjectTemp() {
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().string().starts_with("maskpilot-project-store-")) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
    SiteProject project() const {
        SiteProject value;
        value.id = newTaskId();
        value.options.rootUrl = "https://example.test/root";
        value.options.outputDirectory = path / "exports";
        return value;
    }
};
void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(file.good());
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)), {}};
}
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
Database open(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(path.string().c_str(), &raw) != SQLITE_OK) {
        sqlite3_close(raw);
        throw std::runtime_error("Cannot open owned project fixture database.");
    }
    return Database(raw, sqlite3_close);
}
int version(sqlite3* database) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare version query.");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (sqlite3_step(raw) != SQLITE_ROW)
        throw std::runtime_error("Cannot read version.");
    return sqlite3_column_int(raw, 0);
}
std::vector<SiteProjectArtifact> artifacts() { return {{"index.html", "<!doctype html><p>Owned page</p>"}}; }
void junction(const std::filesystem::path& link, const std::filesystem::path& target) {
#ifdef _WIN32
    struct MountPoint {
        DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        WORD dataLength = 0, reserved = 0, substituteOffset = 0, substituteLength = 0, printOffset = 0,
             printLength = 0;
        wchar_t names[4096]{};
    } buffer;
    const auto substitute = L"\\??\\" + target.wstring();
    const auto printable = target.wstring();
    ASSERT_LT(substitute.size() + printable.size() + 2, std::size(buffer.names));
    std::copy(substitute.begin(), substitute.end(), buffer.names);
    std::copy(printable.begin(), printable.end(), buffer.names + substitute.size() + 1);
    buffer.substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    buffer.printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    buffer.printLength = static_cast<WORD>(printable.size() * sizeof(wchar_t));
    buffer.dataLength = static_cast<WORD>(8 + (substitute.size() + printable.size() + 2) * sizeof(wchar_t));
    ASSERT_TRUE(std::filesystem::create_directory(link));
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    const auto success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer, 8 + buffer.dataLength,
                                         nullptr, 0, &returned, nullptr);
    const auto error = GetLastError();
    CloseHandle(handle);
    ASSERT_TRUE(success) << error;
#else
    std::filesystem::create_directory_symlink(target, link);
#endif
}
} // namespace

TEST(SiteProjectRepository, LegacyManifestDefaultsToNoScheduleAndCorruptRecurrenceFailsClosed) {
    ProjectTemp temporary;
    auto project = temporary.project();
    const auto data = temporary.path / "projects";
    auto repository = makeSiteProjectRepository(data);
    repository->save(project);
    const auto manifest = data / (project.id + ".json");
    auto json = QJsonDocument::fromJson(QByteArray::fromStdString(read(manifest))).object();
    EXPECT_EQ(json.value("version").toInt(), 2);
    json.insert("version", 1);
    for (const auto* key :
         {"syncIntervalMinutes", "syncRemainingRuns", "syncNextAt", "syncLastAt", "automaticRevision"})
        json.remove(key);
    const auto legacy = QJsonDocument(json).toJson().toStdString();
    write(manifest, legacy);
    const auto loaded = repository->load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].syncIntervalMinutes, 0);
    EXPECT_FALSE(loaded[0].automaticRevision);
    EXPECT_EQ(read(manifest), legacy); // Reading alone does not rewrite the previous format.
    repository->save(loaded[0]);
    const auto current = read(manifest);
    json = QJsonDocument::fromJson(QByteArray::fromStdString(current)).object();
    for (const auto* key : {"syncIntervalMinutes", "syncRemainingRuns", "syncNextAt", "automaticRevision"}) {
        auto corrupt = json;
        corrupt.insert(key, key == std::string("automaticRevision") ? QJsonValue(true) : QJsonValue(1));
        write(manifest, QJsonDocument(corrupt).toJson().toStdString());
        EXPECT_THROW(repository->load(), std::exception) << key;
    }
    write(manifest, current);
    EXPECT_EQ(repository->load().size(), 1u);
}

TEST(SiteProjectStorage, MigratesActualVersionTwelveWithOrdinaryDefaultsAndOriginalBackup) {
    ProjectTemp temporary;
    const auto path = temporary.path / "tasks.db";
    const auto migrations =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "database/migrations";
    {
        auto database = open(path);
        for (const auto* name :
             {"0001_initial.sql", "0002_media.sql", "0003_ftp.sql", "0004_torrents.sql", "0005_proxy.sql",
              "0006_contexts.sql", "0007_queues.sql", "0008_queue_windows.sql",
              "0009_native_payload_budgets.sql", "0010_media_selection.sql", "0011_transfer_constraints.sql",
              "0012_response_content_type.sql"}) {
            const auto sql = read(migrations / name);
            ASSERT_FALSE(sql.empty()) << name;
            ASSERT_EQ(sqlite3_exec(database.get(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK)
                << name;
        }
        ASSERT_EQ(version(database.get()), 12);
        ASSERT_EQ(sqlite3_exec(database.get(),
                               "INSERT INTO "
                               "tasks(id,url,destination,category,referrer,user_agent,proxy,expected_sha256,"
                               "connections,priority,"
                               "max_retries,speed_limit,scheduled_at,stop_at,state,downloaded,total,created_"
                               "at,updated_at,retries,resumable,error)"
                               " VALUES('cccccccccccccccccccccccccccccccc','https://example.test/"
                               "file','file.bin','General','','MaskPilot','','',"
                               "4,0,3,0,0,0,5,0,-1,1,1,0,0,'');",
                               nullptr, nullptr, nullptr),
                  SQLITE_OK);
    }
    {
        auto store = makeSqliteStore(path);
        const auto loaded = store.tasks->load();
        ASSERT_EQ(loaded.size(), 1u);
        EXPECT_TRUE(loaded[0].projectId.empty());
        EXPECT_EQ(loaded[0].projectRevision, 0);
    }
    EXPECT_EQ(version(open(path).get()), 14);
    int backups = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path))
        if (entry.path().filename().string().find(".before-v13-") != std::string::npos) {
            ++backups;
            EXPECT_EQ(version(open(entry.path()).get()), 12);
        }
    EXPECT_EQ(backups, 1);
}

TEST(SiteProjectStorage, AssociationIsImmutableAndRequiresBoundedStandaloneHttp) {
    ProjectTemp temporary;
    const auto database = temporary.path / "tasks.db";
    Task child;
    child.id = newTaskId();
    child.projectId = newTaskId();
    child.projectRevision = 1;
    child.spec.url = "https://example.test/root";
    child.spec.destination = temporary.path / "root.body";
    child.spec.maxStoredBytes = 4096;
    child.spec.sameOriginRedirectOnly = true;
    {
        auto store = makeSqliteStore(database);
        store.tasks->save(child);
        for (int change = 0; change < 8; ++change) {
            auto invalid = child;
            if (change == 0) {
                invalid.projectId.clear();
                invalid.projectRevision = 0;
            }
            if (change == 1)
                invalid.projectId = newTaskId();
            if (change == 2)
                ++invalid.projectRevision;
            if (change == 3)
                invalid.spec.maxStoredBytes = 0;
            if (change == 4)
                invalid.spec.sameOriginRedirectOnly = false;
            if (change == 5)
                invalid.spec.url = "ftp://example.test/root";
            if (change == 6) {
                invalid.ownerId = newTaskId();
                invalid.contextRevision = 1;
            }
            if (change == 7)
                invalid.projectRevision = 1000001;
            EXPECT_THROW(store.tasks->save(invalid), std::exception) << change;
        }
        auto ordinary = child;
        ordinary.id = newTaskId();
        ordinary.projectId.clear();
        ordinary.projectRevision = 0;
        store.tasks->save(ordinary);
        ordinary.projectId = child.projectId;
        ordinary.projectRevision = 1;
        EXPECT_THROW(store.tasks->save(ordinary), std::exception);
    }
    const auto loaded = makeSqliteRepository(database)->load();
    ASSERT_EQ(loaded.size(), 2u);
    const auto found =
        std::find_if(loaded.begin(), loaded.end(), [&](const auto& item) { return item.id == child.id; });
    ASSERT_NE(found, loaded.end());
    EXPECT_EQ(found->projectId, child.projectId);
    EXPECT_EQ(found->projectRevision, 1);
    EXPECT_EQ(found->spec.maxStoredBytes, 4096);
    {
        auto raw = open(database);
        ASSERT_EQ(sqlite3_exec(raw.get(), "UPDATE tasks SET project_revision='invalid' WHERE project_id<>''",
                               nullptr, nullptr, nullptr),
                  SQLITE_OK);
    }
    EXPECT_THROW(makeSqliteRepository(database)->load(), std::exception);
}

TEST(SiteProjectRepository, AtomicManifestAndOwnedBodyRoundTripRejectPathHashAndSchemaChanges) {
    ProjectTemp temporary;
    const auto data = temporary.path / "projects";
    auto repository = makeSiteProjectRepository(data);
    auto project = temporary.project();
    const auto revision = repository->prepareRevision(project.id, project.revision);
    SiteProjectEntry entry;
    entry.id = newTaskId();
    entry.requestId = newTaskId();
    entry.url = project.options.rootUrl;
    entry.destination = revision / (entry.id + ".body");
    entry.maxStoredBytes = 64;
    project.entries.push_back(entry);
    repository->save(project);
    write(entry.destination, "owned body");
    const auto body = repository->readBody(project, project.entries[0]);
    EXPECT_EQ(body.body, "owned body");
    EXPECT_EQ(body.sha256.size(), 64u);
    project.entries[0].sha256 = body.sha256;
    repository->save(project);
    repository.reset();
    repository = makeSiteProjectRepository(data);
    const auto loaded = repository->load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].entries[0].destination, entry.destination);
    EXPECT_EQ(repository->readBody(loaded[0], loaded[0].entries[0]).sha256, body.sha256);
    write(entry.destination, "user changed body");
    EXPECT_THROW(repository->readBody(project, project.entries[0]), std::exception);
    write(entry.destination, std::string(65, 'x'));
    EXPECT_THROW(repository->readBody(project, project.entries[0]), std::exception);
    write(entry.destination, "owned body");
    auto wrong = project;
    wrong.entries[0].destination = temporary.path / (entry.id + ".body");
    EXPECT_THROW(repository->save(wrong), std::exception);
    EXPECT_THROW(repository->readBody(wrong, wrong.entries[0]), std::exception);
    const auto manifest = data / (project.id + ".json");
    const auto original = read(manifest);
    auto json = QJsonDocument::fromJson(QByteArray::fromStdString(original)).object();
    json.insert("credentials", "must-not-be-adopted");
    write(manifest, QJsonDocument(json).toJson().toStdString());
    EXPECT_THROW(repository->load(), std::exception);
    EXPECT_EQ(read(entry.destination), "owned body");
    write(manifest, original);
    repository->erase(project.id);
    EXPECT_TRUE(repository->load().empty());
    EXPECT_EQ(read(entry.destination), "owned body");
    EXPECT_THROW(repository->prepareRevision("../foreign", 1), std::exception);
}

TEST(SiteProjectRepository, RefusesForeignRawRevisionAndThirtyThirdManifestWithoutDeletingAnything) {
    ProjectTemp temporary;
    const auto data = temporary.path / "projects";
    auto repository = makeSiteProjectRepository(data);
    auto foreign = temporary.project();
    std::filesystem::create_directories(data / foreign.id / "r-1");
    write(data / foreign.id / "r-1/user.txt", "foreign");
    EXPECT_THROW(repository->prepareRevision(foreign.id, 1), std::exception);
    EXPECT_EQ(read(data / foreign.id / "r-1/user.txt"), "foreign");
    for (int i = 0; i < 32; ++i)
        repository->save(temporary.project());
    EXPECT_EQ(repository->load().size(), 32u);
    EXPECT_THROW(repository->save(temporary.project()), std::exception);
    EXPECT_EQ(repository->load().size(), 32u);
}

#ifdef _WIN32
TEST(SiteProjectRepository, ReopeningSamePhysicalRootWithDifferentCaseKeepsExactOwnedBodyIdentity) {
    ProjectTemp temporary;
    const auto data = temporary.path / "ProjectsMixedCase";
    auto project = temporary.project();
    auto repository = makeSiteProjectRepository(data);
    const auto revision = repository->prepareRevision(project.id, project.revision);
    SiteProjectEntry entry;
    entry.id = newTaskId();
    entry.requestId = newTaskId();
    entry.url = project.options.rootUrl;
    entry.destination = revision / (entry.id + ".body");
    entry.maxStoredBytes = 64;
    project.entries.push_back(entry);
    write(entry.destination, "stable physical body");
    const auto body = repository->readBody(project, project.entries[0]);
    project.entries[0].sha256 = body.sha256;
    repository->save(project);
    repository.reset();
    auto alternate = data.wstring();
    std::transform(alternate.begin(), alternate.end(), alternate.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towupper(value)); });
    ASSERT_NE(std::filesystem::path(alternate), data);
    repository = makeSiteProjectRepository(std::filesystem::path(alternate));
    const auto loaded = repository->load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(repository->prepareRevision(project.id, project.revision), revision);
    EXPECT_EQ(loaded[0].entries[0].destination, entry.destination);
    EXPECT_EQ(repository->readBody(loaded[0], loaded[0].entries[0]).body, "stable physical body");
    EXPECT_EQ(repository->readBody(loaded[0], loaded[0].entries[0]).sha256, body.sha256);
    repository->save(loaded[0]);
}
#endif

TEST(SiteProjectPublication, ImmutableCommitCanBeRecoveredAfterRenameAndNeverOverwritesOrTrustsEditedFiles) {
    ProjectTemp temporary;
    auto project = temporary.project();
    auto publisher = makeSiteProjectPublisher();
    const auto files = artifacts();
    const auto plan = publisher->plan(project, files);
    EXPECT_FALSE(std::filesystem::exists(project.options.outputDirectory));
    EXPECT_FALSE(publisher->resolve(project, plan).has_value());
    project.pendingPublication = plan;
    publisher->commit(project, plan, files);
    auto recovered = makeSiteProjectPublisher();
    const auto index = recovered->resolve(project, plan);
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(read(*index), files[0].body);
    EXPECT_THROW(recovered->commit(project, plan, files), std::exception);
    EXPECT_EQ(read(*index), files[0].body);
    write(index->parent_path() / "user.txt", "foreign");
    EXPECT_THROW(recovered->resolve(project, plan), std::exception);
    EXPECT_EQ(read(index->parent_path() / "user.txt"), "foreign");
    std::filesystem::remove(index->parent_path() / "user.txt");
    write(*index, "user edit");
    EXPECT_THROW(recovered->resolve(project, plan), std::exception);
    recovered->discardStage(project, plan);
    EXPECT_EQ(read(*index), "user edit");
}

TEST(SiteProjectPublication,
     StageDiscardChecksCompleteOwnershipBeforeDeletingAndPreservesUnknownOrEditedFiles) {
    ProjectTemp temporary;
    auto project = temporary.project();
    auto publisher = makeSiteProjectPublisher();
    const auto files = artifacts();
    const auto plan = publisher->plan(project, files);
    publisher->commit(project, plan, files);
    const auto root = project.options.outputDirectory / ("site-" + project.id);
    const auto stage = root / (".stage-1-" + plan.stageToken);
    std::filesystem::rename(root / "r-1", stage);
    write(stage / "user.txt", "foreign");
    EXPECT_THROW(publisher->discardStage(project, plan), std::exception);
    EXPECT_EQ(read(stage / "index.html"), files[0].body);
    EXPECT_EQ(read(stage / "user.txt"), "foreign");
    std::filesystem::remove(stage / "user.txt");
    write(stage / "index.html", "changed");
    EXPECT_THROW(publisher->discardStage(project, plan), std::exception);
    EXPECT_EQ(read(stage / "index.html"), "changed");
    write(stage / "index.html", files[0].body);
    publisher->discardStage(project, plan);
    EXPECT_FALSE(std::filesystem::exists(stage));
    EXPECT_TRUE(std::filesystem::is_regular_file(root / ".maskpilot-site-owner"));
    EXPECT_THROW(publisher->plan(project, {{"../outside.html", "unsafe"}}), std::exception);
    EXPECT_THROW(publisher->plan(project, {{"index.html", "safe"}, {"CON", "unsafe"}}), std::exception);
}

TEST(SiteProjectOwnership, RealDirectoryReparseCannotRedirectRawReadsOrPublicationCleanup) {
    ProjectTemp temporary;
    auto repository = makeSiteProjectRepository(temporary.path / "projects");
    auto project = temporary.project();
    const auto revision = repository->prepareRevision(project.id, project.revision);
    SiteProjectEntry entry;
    entry.id = newTaskId();
    entry.requestId = newTaskId();
    entry.url = project.options.rootUrl;
    entry.destination = revision / (entry.id + ".body");
    entry.maxStoredBytes = 64;
    project.entries.push_back(entry);
    write(entry.destination, "owned raw");
    const auto rawTarget = temporary.path / "raw-target";
    std::filesystem::rename(revision, rawTarget);
    junction(revision, rawTarget);
    ASSERT_TRUE(std::filesystem::exists(revision / (entry.id + ".body")));
    EXPECT_THROW(repository->readBody(project, entry), std::exception);
    EXPECT_EQ(read(rawTarget / (entry.id + ".body")), "owned raw");
    // The publication fixture is independent of the deliberately invalid raw plan.
    auto publicationProject = temporary.project();
    auto publisher = makeSiteProjectPublisher();
    const auto files = artifacts();
    const auto plan = publisher->plan(publicationProject, files);
    publisher->commit(publicationProject, plan, files);
    const auto root = publicationProject.options.outputDirectory / ("site-" + publicationProject.id);
    const auto exported = root / "r-1";
    const auto exportTarget = temporary.path / "export-target";
    std::filesystem::rename(exported, exportTarget);
    junction(exported, exportTarget);
    EXPECT_THROW(publisher->resolve(publicationProject, plan), std::exception);
    const auto stage = root / (".stage-1-" + plan.stageToken);
    junction(stage, exportTarget);
    EXPECT_THROW(publisher->discardStage(publicationProject, plan), std::exception);
    EXPECT_EQ(read(exportTarget / "index.html"), files[0].body);
}
