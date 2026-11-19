#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cctype>
#include <maskpilot/infrastructure/adapters.h>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace maskpilot {
namespace {
namespace fs = std::filesystem;
constexpr auto markerName = ".maskpilot-publication.json";
[[noreturn]] void reject() {
    throw std::runtime_error(
        "Project publication ownership or content changed. Existing files were preserved.");
}
QString text(const fs::path& path) { return QString::fromStdWString(path.wstring()); }
void safe(const fs::path& path) {
    if (!path.is_absolute() || path.lexically_normal() != path ||
        path.native().starts_with(fs::path("//").native()) ||
        path.native().starts_with(fs::path("\\\\").native()))
        reject();
    fs::path current;
    for (const auto& component : path) {
        current /= component;
        std::error_code error;
        const auto status = fs::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory)
            continue;
        if (error || fs::is_symlink(status))
            reject();
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            reject();
#endif
    }
}
bool flat(const std::string& name) {
    return !name.empty() && name.size() <= 180 && name.front() != '.' && name.back() != '.' &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_' || c == '.';
           });
}
QByteArray bytes(const std::string& body) {
    return QByteArray(body.data(), static_cast<qsizetype>(body.size()));
}
std::string digest(const std::string& body) {
    return QCryptographicHash::hash(bytes(body), QCryptographicHash::Sha256).toHex().toStdString();
}
QByteArray receipt(const SiteProject& project, const SiteProjectPublication& publication) {
    QJsonArray files;
    for (const auto& file : publication.files)
        files.append(QJsonObject{{"name", QString::fromStdString(file.name)},
                                 {"sha256", QString::fromStdString(file.sha256)},
                                 {"bytes", static_cast<double>(file.bytes)}});
    return QJsonDocument(QJsonObject{{"version", 1},
                                     {"project", QString::fromStdString(project.id)},
                                     {"revision", static_cast<double>(publication.revision)},
                                     {"token", QString::fromStdString(publication.stageToken)},
                                     {"index", QString::fromStdString(publication.indexName)},
                                     {"files", files}})
        .toJson(QJsonDocument::Compact);
}
void writeNew(const fs::path& path, const QByteArray& value) {
    safe(path);
    QFile file(text(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(value) != value.size() ||
        !file.flush())
        reject();
}
bool exactFile(const fs::path& path, const QByteArray& expected) {
    safe(path);
    if (!fs::is_regular_file(fs::symlink_status(path)))
        return false;
    QFile file(text(path));
    return file.open(QIODevice::ReadOnly) && file.size() == expected.size() && file.readAll() == expected;
}
fs::path rootFor(const SiteProject& project) {
    return project.options.outputDirectory / ("site-" + project.id);
}
fs::path stageFor(const SiteProject& project, const SiteProjectPublication& publication) {
    return rootFor(project) /
           (".stage-" + std::to_string(publication.revision) + "-" + publication.stageToken);
}
void validate(const SiteProject& project, const SiteProjectPublication& publication) {
    if (!validateSiteProject(project).empty() || publication.revision < 1 ||
        publication.revision > project.revision || publication.stageToken.size() != 32 ||
        !std::all_of(publication.stageToken.begin(), publication.stageToken.end(),
                     [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        publication.indexName != "index.html" || publication.files.empty() || publication.files.size() > 2048)
        reject();
    auto inventory = project;
    inventory.state = SiteProjectState::Draft;
    // This copy validates only publication inventory, independent of the live scheduling checkpoint.
    inventory.syncIntervalMinutes = inventory.syncRemainingRuns = 0;
    inventory.syncNextAt = inventory.syncLastAt = 0;
    inventory.automaticRevision = false;
    inventory.pendingPublication.reset();
    inventory.published = publication;
    if (!validateSiteProject(inventory).empty())
        reject();
    std::set<std::string> names;
    std::int64_t total = 0;
    const auto limit =
        publication.revision == project.revision ? project.options.maxOutputBytes : 512LL * 1024 * 1024;
    for (const auto& file : publication.files) {
        std::string folded = file.name;
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!flat(file.name) || !names.insert(folded).second || file.bytes < 0 ||
            file.bytes > limit - total || file.sha256.size() != 64 ||
            !std::all_of(file.sha256.begin(), file.sha256.end(),
                         [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            reject();
        total += file.bytes;
    }
    if (!names.contains("index.html"))
        reject();
    safe(rootFor(project));
}
void ownedRoot(const SiteProject& project, bool create) {
    const auto root = rootFor(project);
    safe(root);
    if (!fs::exists(root)) {
        if (!create)
            reject();
        fs::create_directories(project.options.outputDirectory);
        safe(project.options.outputDirectory);
        if (!fs::create_directory(root))
            reject();
        writeNew(root / ".maskpilot-site-owner", bytes(project.id + "\n"));
    }
    if (!fs::is_directory(fs::symlink_status(root)) ||
        !exactFile(root / ".maskpilot-site-owner", bytes(project.id + "\n")))
        reject();
}
void checkInventory(const SiteProject& project, const SiteProjectPublication& publication,
                    const fs::path& directory, bool complete) {
    safe(directory);
    if (!fs::is_directory(fs::symlink_status(directory)) ||
        !exactFile(directory / markerName, receipt(project, publication)))
        reject();
    std::set<std::string> expected{markerName};
    for (const auto& file : publication.files) {
        expected.insert(file.name);
        const auto path = directory / file.name;
        safe(path);
        if (!fs::exists(path)) {
            if (complete)
                reject();
            continue;
        }
        if (!fs::is_regular_file(fs::symlink_status(path)) ||
            fs::file_size(path) != static_cast<std::uintmax_t>(file.bytes) || sha256File(path) != file.sha256)
            reject();
    }
    for (const auto& entry : fs::directory_iterator(directory))
        if (!expected.contains(entry.path().filename().string()))
            reject();
}
void renameNew(const fs::path& source, const fs::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
        reject();
#elif defined(__linux__)
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), 1) != 0)
        reject();
#else
    (void)source;
    (void)target;
    reject();
#endif
}
class Publisher final : public ISiteProjectPublisher {
  public:
    SiteProjectPublication plan(const SiteProject& project,
                                const std::vector<SiteProjectArtifact>& artifacts) override {
        if (!validateSiteProject(project).empty() || artifacts.empty() || artifacts.size() > 1000)
            reject();
        SiteProjectPublication publication;
        publication.revision = project.revision;
        publication.stageToken = newTaskId();
        std::int64_t total = 0;
        for (const auto& artifact : artifacts) {
            if (!flat(artifact.name) ||
                artifact.body.size() > static_cast<std::uint64_t>(project.options.maxOutputBytes - total))
                reject();
            total += static_cast<std::int64_t>(artifact.body.size());
            publication.files.push_back(
                {artifact.name, digest(artifact.body), static_cast<std::int64_t>(artifact.body.size())});
        }
        validate(project, publication);
        return publication;
    }
    void commit(const SiteProject& project, const SiteProjectPublication& publication,
                const std::vector<SiteProjectArtifact>& artifacts) override {
        validate(project, publication);
        if (publication.revision != project.revision || artifacts.size() != publication.files.size())
            reject();
        for (std::size_t i = 0; i < artifacts.size(); ++i)
            if (artifacts[i].name != publication.files[i].name ||
                artifacts[i].body.size() != static_cast<std::size_t>(publication.files[i].bytes) ||
                digest(artifacts[i].body) != publication.files[i].sha256)
                reject();
        ownedRoot(project, true);
        const auto target = rootFor(project) / ("r-" + std::to_string(publication.revision));
        safe(target);
        if (fs::exists(target))
            reject();
        const auto stage = stageFor(project, publication);
        safe(stage);
        if (!fs::create_directory(stage))
            reject();
        writeNew(stage / markerName, receipt(project, publication));
        for (const auto& artifact : artifacts)
            writeNew(stage / artifact.name, bytes(artifact.body));
        checkInventory(project, publication, stage, true);
        renameNew(stage, target);
    }
    std::optional<fs::path> resolve(const SiteProject& project,
                                    const SiteProjectPublication& publication) override {
        validate(project, publication);
        const auto target = rootFor(project) / ("r-" + std::to_string(publication.revision));
        safe(target);
        if (!fs::exists(target))
            return std::nullopt;
        ownedRoot(project, false);
        checkInventory(project, publication, target, true);
        return target / publication.indexName;
    }
    void discardStage(const SiteProject& project, const SiteProjectPublication& publication) override {
        validate(project, publication);
        const auto stage = stageFor(project, publication);
        safe(stage);
        if (!fs::exists(stage))
            return;
        ownedRoot(project, false);
        checkInventory(project, publication, stage, false);
        for (const auto& file : publication.files) {
            const auto path = stage / file.name;
            safe(path);
            if (fs::exists(path)) {
                if (!fs::is_regular_file(fs::symlink_status(path)) ||
                    fs::file_size(path) != static_cast<std::uintmax_t>(file.bytes) ||
                    sha256File(path) != file.sha256 || !fs::remove(path))
                    reject();
            }
        }
        if (!exactFile(stage / markerName, receipt(project, publication)) ||
            !fs::remove(stage / markerName) || !fs::remove(stage))
            reject();
    }
};
} // namespace
std::unique_ptr<ISiteProjectPublisher> makeSiteProjectPublisher() { return std::make_unique<Publisher>(); }
} // namespace maskpilot
