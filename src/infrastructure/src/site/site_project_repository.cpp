#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <maskpilot/infrastructure/adapters.h>
#include <mutex>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace maskpilot {
namespace {
namespace fs = std::filesystem;
constexpr qsizetype manifestLimit = 4 * 1024 * 1024;
[[noreturn]] void invalid() {
    throw std::runtime_error("Invalid or changed project storage. Existing files were preserved.");
}
QString text(const fs::path& value) { return QString::fromStdU16String(value.u16string()); }
QString string(const std::string& value) { return QString::fromStdString(value); }
bool opaque(const std::string& value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
void safe(const fs::path& path) {
    if (!path.is_absolute() || path.lexically_normal() != path ||
        path.native().starts_with(fs::path("//").native()) ||
        path.native().starts_with(fs::path("\\\\").native()))
        invalid();
    fs::path current;
    for (const auto& component : path) {
        current /= component;
        std::error_code error;
        const auto status = fs::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory)
            continue;
        if (error || fs::is_symlink(status))
            invalid();
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            invalid();
#endif
    }
}
void keys(const QJsonObject& value, std::initializer_list<const char*> names) {
    if (value.size() != static_cast<qsizetype>(names.size()))
        invalid();
    for (const auto* name : names)
        if (!value.contains(name))
            invalid();
}
std::string getString(const QJsonObject& value, const char* name) {
    const auto item = value.value(name);
    if (!item.isString() || item.toString().contains(QChar::Null))
        invalid();
    return item.toString().toStdString();
}
fs::path getPath(const QJsonObject& value, const char* name) {
    const auto item = getString(value, name);
    return fs::path(std::u8string(item.begin(), item.end()));
}
std::int64_t number(const QJsonObject& value, const char* name) {
    const auto item = value.value(name);
    const auto result = item.toDouble(-1);
    if (!item.isDouble() || !std::isfinite(result) || result != std::floor(result) || result < 0 ||
        result > 9007199254740991.0)
        invalid();
    return static_cast<std::int64_t>(result);
}
int integer(const QJsonObject& value, const char* name) {
    const auto result = number(value, name);
    if (result > 1000000)
        invalid();
    return static_cast<int>(result);
}
bool boolean(const QJsonObject& value, const char* name) {
    if (!value.value(name).isBool())
        invalid();
    return value.value(name).toBool();
}
QJsonArray array(const QJsonObject& value, const char* name, qsizetype limit) {
    if (!value.value(name).isArray() || value.value(name).toArray().size() > limit)
        invalid();
    return value.value(name).toArray();
}
QJsonObject object(const QJsonValue& value) {
    if (!value.isObject())
        invalid();
    return value.toObject();
}
QJsonArray strings(const std::vector<std::string>& values) {
    QJsonArray result;
    for (const auto& value : values)
        result.append(string(value));
    return result;
}
std::vector<std::string> readStrings(const QJsonObject& value, const char* name) {
    std::vector<std::string> result;
    for (const auto& item : array(value, name, 16)) {
        if (!item.isString() || item.toString().contains(QChar::Null))
            invalid();
        result.push_back(item.toString().toStdString());
    }
    return result;
}
QJsonObject encodeOptions(const SiteProjectOptions& value) {
    return {{"name", string(value.name)},
            {"rootUrl", string(value.rootUrl)},
            {"queueId", string(value.queueId)},
            {"outputDirectory", text(value.outputDirectory)},
            {"maxDepth", value.maxDepth},
            {"maxPages", value.maxPages},
            {"maxResources", value.maxResources},
            {"maxPageBytes", static_cast<double>(value.maxPageBytes)},
            {"maxAssetBytes", static_cast<double>(value.maxAssetBytes)},
            {"maxStoredBytes", static_cast<double>(value.maxStoredBytes)},
            {"maxOutputBytes", static_cast<double>(value.maxOutputBytes)},
            {"includePrefixes", strings(value.includePrefixes)},
            {"excludePrefixes", strings(value.excludePrefixes)},
            {"styles", value.styles},
            {"images", value.images},
            {"fonts", value.fonts},
            {"media", value.media},
            {"proxy", string(value.proxy)},
            {"proxyAuthenticationRequired", value.proxyAuthenticationRequired}};
}
SiteProjectOptions decodeOptions(const QJsonObject& value) {
    keys(value,
         {"name", "rootUrl", "queueId", "outputDirectory", "maxDepth", "maxPages", "maxResources",
          "maxPageBytes", "maxAssetBytes", "maxStoredBytes", "maxOutputBytes", "includePrefixes",
          "excludePrefixes", "styles", "images", "fonts", "media", "proxy", "proxyAuthenticationRequired"});
    SiteProjectOptions result;
    result.name = getString(value, "name");
    result.rootUrl = getString(value, "rootUrl");
    result.queueId = getString(value, "queueId");
    result.outputDirectory = getPath(value, "outputDirectory");
    result.maxDepth = integer(value, "maxDepth");
    result.maxPages = integer(value, "maxPages");
    result.maxResources = integer(value, "maxResources");
    result.maxPageBytes = number(value, "maxPageBytes");
    result.maxAssetBytes = number(value, "maxAssetBytes");
    result.maxStoredBytes = number(value, "maxStoredBytes");
    result.maxOutputBytes = number(value, "maxOutputBytes");
    result.includePrefixes = readStrings(value, "includePrefixes");
    result.excludePrefixes = readStrings(value, "excludePrefixes");
    result.styles = boolean(value, "styles");
    result.images = boolean(value, "images");
    result.fonts = boolean(value, "fonts");
    result.media = boolean(value, "media");
    result.proxy = getString(value, "proxy");
    result.proxyAuthenticationRequired = boolean(value, "proxyAuthenticationRequired");
    return result;
}
QJsonObject encodeEntry(const SiteProjectEntry& value) {
    return {{"id", string(value.id)},
            {"requestId", string(value.requestId)},
            {"taskId", string(value.taskId)},
            {"url", string(value.url)},
            {"kind", static_cast<int>(value.kind)},
            {"depth", value.depth},
            {"state", static_cast<int>(value.state)},
            {"destination", text(value.destination)},
            {"maxStoredBytes", static_cast<double>(value.maxStoredBytes)},
            {"bytes", static_cast<double>(value.bytes)},
            {"sha256", string(value.sha256)},
            {"responseUrl", string(value.responseUrl)},
            {"contentType", string(value.contentType)},
            {"error", string(value.error)}};
}
SiteProjectEntry decodeEntry(const QJsonObject& value) {
    keys(value, {"id", "requestId", "taskId", "url", "kind", "depth", "state", "destination",
                 "maxStoredBytes", "bytes", "sha256", "responseUrl", "contentType", "error"});
    SiteProjectEntry result;
    result.id = getString(value, "id");
    result.requestId = getString(value, "requestId");
    result.taskId = getString(value, "taskId");
    result.url = getString(value, "url");
    result.kind = static_cast<SiteContentKind>(integer(value, "kind"));
    result.depth = integer(value, "depth");
    result.state = static_cast<SiteProjectEntryState>(integer(value, "state"));
    result.destination = getPath(value, "destination");
    result.maxStoredBytes = number(value, "maxStoredBytes");
    result.bytes = number(value, "bytes");
    result.sha256 = getString(value, "sha256");
    result.responseUrl = getString(value, "responseUrl");
    result.contentType = getString(value, "contentType");
    result.error = getString(value, "error");
    return result;
}
QJsonValue encodePublication(const std::optional<SiteProjectPublication>& value) {
    if (!value)
        return QJsonValue::Null;
    QJsonArray files;
    for (const auto& file : value->files)
        files.append(QJsonObject{{"name", string(file.name)},
                                 {"sha256", string(file.sha256)},
                                 {"bytes", static_cast<double>(file.bytes)}});
    return QJsonObject{{"revision", static_cast<double>(value->revision)},
                       {"stageToken", string(value->stageToken)},
                       {"indexName", string(value->indexName)},
                       {"files", files}};
}
std::optional<SiteProjectPublication> decodePublication(const QJsonValue& value) {
    if (value.isNull())
        return std::nullopt;
    const auto item = object(value);
    keys(item, {"revision", "stageToken", "indexName", "files"});
    SiteProjectPublication result;
    result.revision = number(item, "revision");
    result.stageToken = getString(item, "stageToken");
    result.indexName = getString(item, "indexName");
    for (const auto& file : array(item, "files", 2048)) {
        const auto entry = object(file);
        keys(entry, {"name", "sha256", "bytes"});
        result.files.push_back(
            {getString(entry, "name"), getString(entry, "sha256"), number(entry, "bytes")});
    }
    return result;
}
QByteArray encode(const SiteProject& project) {
    QJsonArray entries;
    for (const auto& entry : project.entries)
        entries.append(encodeEntry(entry));
    return QJsonDocument(QJsonObject{{"version", 2},
                                     {"id", string(project.id)},
                                     {"options", encodeOptions(project.options)},
                                     {"revision", static_cast<double>(project.revision)},
                                     {"state", static_cast<int>(project.state)},
                                     {"resumePhase", static_cast<int>(project.resumePhase)},
                                     {"entries", entries},
                                     {"published", encodePublication(project.published)},
                                     {"pendingPublication", encodePublication(project.pendingPublication)},
                                     {"excludedCount", static_cast<double>(project.excludedCount)},
                                     {"syncIntervalMinutes", project.syncIntervalMinutes},
                                     {"syncRemainingRuns", project.syncRemainingRuns},
                                     {"syncNextAt", static_cast<double>(project.syncNextAt)},
                                     {"syncLastAt", static_cast<double>(project.syncLastAt)},
                                     {"automaticRevision", project.automaticRevision},
                                     {"error", string(project.error)}})
        .toJson(QJsonDocument::Compact);
}
SiteProject decode(const QByteArray& bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        invalid();
    const auto value = document.object();
    const auto version = number(value, "version");
    if (version == 1)
        keys(value, {"version", "id", "options", "revision", "state", "resumePhase", "entries", "published",
                     "pendingPublication", "excludedCount", "error"});
    else if (version == 2)
        keys(value, {"version", "id", "options", "revision", "state", "resumePhase", "entries", "published",
                     "pendingPublication", "excludedCount", "error", "syncIntervalMinutes",
                     "syncRemainingRuns", "syncNextAt", "syncLastAt", "automaticRevision"});
    else
        invalid();
    SiteProject result;
    result.id = getString(value, "id");
    result.options = decodeOptions(object(value.value("options")));
    result.revision = number(value, "revision");
    result.state = static_cast<SiteProjectState>(integer(value, "state"));
    result.resumePhase = static_cast<SiteProjectState>(integer(value, "resumePhase"));
    for (const auto& entry : array(value, "entries", 1128))
        result.entries.push_back(decodeEntry(object(entry)));
    result.published = decodePublication(value.value("published"));
    result.pendingPublication = decodePublication(value.value("pendingPublication"));
    result.excludedCount = number(value, "excludedCount");
    result.error = getString(value, "error");
    if (version == 2) {
        result.syncIntervalMinutes = integer(value, "syncIntervalMinutes");
        result.syncRemainingRuns = integer(value, "syncRemainingRuns");
        result.syncNextAt = number(value, "syncNextAt");
        result.syncLastAt = number(value, "syncLastAt");
        result.automaticRevision = boolean(value, "automaticRevision");
    }
    if (!validateSiteProject(result).empty())
        invalid();
    return result;
}
QByteArray read(const fs::path& path, qsizetype limit) {
    safe(path);
    if (!fs::is_regular_file(fs::symlink_status(path)))
        invalid();
    QFile file(text(path));
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
        invalid();
    const auto result = file.read(limit + 1);
    if (result.size() > limit || file.error() != QFileDevice::NoError)
        invalid();
    return result;
}
void owner(const fs::path& directory, const char* marker, const std::string& identity) {
    safe(directory);
    const auto path = directory / marker;
    const auto expected = QByteArray::fromStdString(identity + "\n");
    if (!fs::exists(directory)) {
        if (!fs::create_directory(directory))
            invalid();
        QFile file(text(path));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            file.write(expected) != expected.size() || !file.flush())
            invalid();
    }
    if (!fs::is_directory(fs::symlink_status(directory)) || read(path, 256) != expected)
        invalid();
}
class Repository final : public ISiteProjectRepository {
    fs::path root_;
    std::mutex mutex_;
    fs::path revisionPath(const std::string& id, std::int64_t revision) const {
        if (!opaque(id) || revision < 1 || revision > 1000000)
            invalid();
        return root_ / id / ("r-" + std::to_string(revision));
    }
    void validatePaths(const SiteProject& project) const {
        if (!validateSiteProject(project).empty())
            invalid();
        safe(project.options.outputDirectory);
        for (const auto& entry : project.entries) {
            if (!entry.destination.empty() &&
                entry.destination != revisionPath(project.id, project.revision) / (entry.id + ".body"))
                invalid();
            if (!entry.destination.empty())
                safe(entry.destination);
        }
    }
    std::vector<SiteProject> loadUnlocked() const {
        safe(root_);
        std::vector<SiteProject> result;
        for (const auto& entry : fs::directory_iterator(root_)) {
            if (entry.path().extension() != ".json")
                continue;
            if (result.size() >= 32 || !opaque(entry.path().stem().string()))
                invalid();
            auto project = decode(read(entry.path(), manifestLimit));
            if (project.id != entry.path().stem().string())
                invalid();
            validatePaths(project);
            result.push_back(std::move(project));
        }
        return result;
    }

  public:
    explicit Repository(fs::path root) : root_(std::move(root)) {
        safe(root_);
        fs::create_directories(root_);
        safe(root_);
        if (!fs::is_directory(fs::symlink_status(root_)))
            invalid();
        root_ = fs::canonical(root_);
#ifdef _WIN32
        const auto handle = CreateFileW(
            root_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            invalid();
        const auto required =
            GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        std::wstring finalPath(required, L'\0');
        const auto length = required ? GetFinalPathNameByHandleW(handle, finalPath.data(), required,
                                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)
                                     : 0;
        CloseHandle(handle);
        if (!length || length >= required)
            invalid();
        finalPath.resize(length);
        if (finalPath.starts_with(L"\\\\?\\UNC\\"))
            invalid();
        if (finalPath.starts_with(L"\\\\?\\"))
            finalPath.erase(0, 4);
        root_ = fs::path(finalPath);
#endif
        safe(root_);
    }
    std::vector<SiteProject> load() override {
        std::lock_guard lock(mutex_);
        return loadUnlocked();
    }
    void save(const SiteProject& project) override {
        std::lock_guard lock(mutex_);
        validatePaths(project);
        const auto current = loadUnlocked();
        const auto prior = std::find_if(current.begin(), current.end(),
                                        [&](const auto& value) { return value.id == project.id; });
        if (prior == current.end() && current.size() >= 32)
            invalid();
        if (prior != current.end() &&
            (prior->options.rootUrl != project.options.rootUrl ||
             prior->options.outputDirectory != project.options.outputDirectory ||
             project.revision < prior->revision || project.revision > prior->revision + 1))
            invalid();
        const auto bytes = encode(project);
        if (bytes.size() > manifestLimit)
            invalid();
        const auto path = root_ / (project.id + ".json");
        safe(path);
        QSaveFile file(text(path));
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            invalid();
    }
    void erase(const std::string& id) override {
        std::lock_guard lock(mutex_);
        if (!opaque(id))
            invalid();
        const auto path = root_ / (id + ".json");
        safe(path);
        if (!fs::exists(path))
            return;
        const auto project = decode(read(path, manifestLimit));
        if (project.id != id)
            invalid();
        if (!fs::remove(path))
            invalid();
    }
    fs::path prepareRevision(const std::string& id, std::int64_t revision) override {
        std::lock_guard lock(mutex_);
        const auto path = revisionPath(id, revision);
        safe(root_);
        owner(root_ / id, ".maskpilot-project-owner", id);
        owner(path, ".maskpilot-revision-owner", id + ":" + std::to_string(revision));
        return path;
    }
    SiteProjectBody readBody(const SiteProject& project, const SiteProjectEntry& entry) override {
        std::lock_guard lock(mutex_);
        validatePaths(project);
        const auto known = std::find_if(project.entries.begin(), project.entries.end(),
                                        [&](const auto& item) { return item.id == entry.id; });
        if (known == project.entries.end() || known->destination != entry.destination ||
            known->maxStoredBytes != entry.maxStoredBytes || known->sha256 != entry.sha256 ||
            entry.maxStoredBytes <= 0 || entry.maxStoredBytes > 8 * 1024 * 1024 ||
            entry.destination != revisionPath(project.id, project.revision) / (entry.id + ".body"))
            invalid();
        const auto directory = entry.destination.parent_path();
        if (read(root_ / project.id / ".maskpilot-project-owner", 256) !=
                QByteArray::fromStdString(project.id + "\n") ||
            read(directory / ".maskpilot-revision-owner", 256) !=
                QByteArray::fromStdString(project.id + ":" + std::to_string(project.revision) + "\n"))
            invalid();
        const auto bytes = read(entry.destination, static_cast<qsizetype>(entry.maxStoredBytes));
        const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
        if (!entry.sha256.empty() && entry.sha256 != hash)
            invalid();
        return {std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())), hash};
    }
};
} // namespace
std::unique_ptr<ISiteProjectRepository> makeSiteProjectRepository(const std::filesystem::path& dataRoot) {
    return std::make_unique<Repository>(dataRoot);
}
} // namespace maskpilot
