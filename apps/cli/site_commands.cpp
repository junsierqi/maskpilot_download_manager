#include "site_commands.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <maskpilot/application/site_project_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <stdexcept>
#include <type_traits>
namespace maskpilot {
namespace {
QString q(const std::string& s) { return QString::fromUtf8(s); }
void keys(const QJsonObject& o, const QSet<QString>& allowed) {
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!allowed.contains(it.key()))
            throw std::invalid_argument("Unknown project request field.");
}
std::string string(const QJsonObject& o, const char* name, std::string fallback = {},
                   std::size_t maximum = 32768) {
    auto value = o.value(name);
    if (value.isUndefined())
        return fallback;
    if (!value.isString())
        throw std::invalid_argument("Expected a project string field.");
    auto out = value.toString().toUtf8().toStdString();
    if (out.size() > maximum ||
        std::any_of(out.begin(), out.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::invalid_argument("Invalid project string field.");
    return out;
}
std::int64_t integer(const QJsonObject& o, const char* name, std::int64_t fallback,
                     std::int64_t maximum = 9007199254740991LL) {
    auto value = o.value(name);
    if (value.isUndefined())
        return fallback;
    auto number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 || number > static_cast<double>(maximum) ||
        std::floor(number) != number)
        throw std::invalid_argument("Invalid project integer field.");
    return static_cast<std::int64_t>(number);
}
bool boolean(const QJsonObject& o, const char* name, bool fallback) {
    auto value = o.value(name);
    if (value.isUndefined())
        return fallback;
    if (!value.isBool())
        throw std::invalid_argument("Invalid project boolean field.");
    return value.toBool();
}
std::vector<std::string> prefixes(const QJsonObject& o, const char* name) {
    auto value = o.value(name);
    if (value.isUndefined())
        return {};
    if (!value.isArray() || value.toArray().size() > 16)
        throw std::invalid_argument("Invalid project prefixes.");
    std::vector<std::string> result;
    for (auto item : value.toArray()) {
        if (!item.isString())
            throw std::invalid_argument("Invalid project prefix.");
        result.push_back(string(QJsonObject{{"x", item}}, "x", {}, 512));
    }
    return result;
}
SiteProjectOptions options(const QJsonObject& o) {
    keys(o,
         {"name", "rootUrl", "outputDirectory", "queueId", "maxDepth", "maxPages", "maxResources",
          "maxPageBytes", "maxAssetBytes", "maxStoredBytes", "maxOutputBytes", "includePrefixes",
          "excludePrefixes", "styles", "images", "fonts", "media", "proxy", "proxyAuthenticationRequired"});
    SiteProjectOptions value;
    value.name = string(o, "name", value.name, 256);
    value.rootUrl = string(o, "rootUrl");
    auto output = string(o, "outputDirectory");
    value.outputDirectory = std::filesystem::path(std::u8string(output.begin(), output.end()));
    value.queueId = string(o, "queueId", defaultQueueId, 32);
    value.maxDepth = static_cast<int>(integer(o, "maxDepth", 2, 8));
    value.maxPages = static_cast<int>(integer(o, "maxPages", 32, 128));
    value.maxResources = static_cast<int>(integer(o, "maxResources", 256, 1000));
    value.maxPageBytes = integer(o, "maxPageBytes", value.maxPageBytes, 8 * 1024 * 1024);
    value.maxAssetBytes = integer(o, "maxAssetBytes", value.maxAssetBytes, 8 * 1024 * 1024);
    value.maxStoredBytes = integer(o, "maxStoredBytes", value.maxStoredBytes, 512 * 1024 * 1024);
    value.maxOutputBytes = integer(o, "maxOutputBytes", value.maxOutputBytes, 512 * 1024 * 1024);
    value.includePrefixes = prefixes(o, "includePrefixes");
    value.excludePrefixes = prefixes(o, "excludePrefixes");
    value.styles = boolean(o, "styles", true);
    value.images = boolean(o, "images", true);
    value.fonts = boolean(o, "fonts", true);
    value.media = boolean(o, "media", false);
    value.proxy = string(o, "proxy", {}, 32768);
    value.proxyAuthenticationRequired = boolean(o, "proxyAuthenticationRequired", false);
    if (auto error = validateSiteProjectOptions(value); !error.empty())
        throw std::invalid_argument(error);
    return value;
}
std::string environment(const QJsonObject& request, const char* field) {
    const auto name = string(request, field, {}, 128);
    if (name.empty())
        return {};
    if (!std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }))
        throw std::invalid_argument("Invalid credential environment variable name.");
    if (!qEnvironmentVariableIsSet(name.c_str()))
        throw std::invalid_argument("Proxy credential environment variable is unavailable.");
    return qEnvironmentVariable(name.c_str()).toUtf8().toStdString();
}
template <class T> T await(std::future<T> future, std::chrono::steady_clock::time_point deadline) {
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Project command acknowledgement timed out.");
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    if constexpr (std::is_void_v<T>) {
        future.get();
        return;
    } else
        return future.get();
}
QString stateToken(SiteProjectState state) {
    static constexpr const char* names[] = {"Draft",          "Previewing", "PreviewReady", "Running",
                                            "Pausing",        "Paused",     "Publishing",   "Completed",
                                            "NeedsAttention", "Cancelling", "Cancelled"};
    return state >= SiteProjectState::Draft && state < SiteProjectState::Count
               ? QString::fromUtf8(names[static_cast<int>(state)])
               : QString("Invalid");
}
QJsonObject summary(const SiteProjectSummary& s) {
    return {{"id", q(s.id)},
            {"name", q(s.name)},
            {"rootUrl", q(s.rootUrl)},
            {"queueId", q(s.queueId)},
            {"state", stateToken(s.state)},
            {"revision", s.revision},
            {"publishedRevision", s.publishedRevision},
            {"storedBytes", s.storedBytes},
            {"reservedBytes", s.reservedBytes},
            {"documents", static_cast<qint64>(s.documents)},
            {"resources", static_cast<qint64>(s.resources)},
            {"ready", static_cast<qint64>(s.ready)},
            {"skipped", static_cast<qint64>(s.skipped)},
            {"needsProxyCredentials", s.needsProxyCredentials},
            {"error", q(s.error)}};
}
void respond(const QJsonObject& o) {
    auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
    std::cout.write(bytes.constData(), bytes.size());
    std::cout << std::endl;
}
} // namespace
int runSiteProjectCommand(int argc, char** argv, CompletionScanDependencies scanning) {
    try {
        if (argc != 4 || std::string(argv[1]) != "--project")
            throw std::invalid_argument("Usage: maskpilot_cli --project REQUEST_JSON DATA_DIRECTORY");
        QCoreApplication application(argc, argv);
        QCoreApplication::setOrganizationName("MaskPilot");
        QCoreApplication::setApplicationName("MaskPilot");
        QFile input(QString::fromUtf8(argv[2]));
        if (!input.open(QIODevice::ReadOnly) || input.size() <= 0 || input.size() > 65536)
            throw std::invalid_argument("Project request file is empty, unavailable or oversized.");
        QJsonParseError parseError;
        auto document = QJsonDocument::fromJson(input.readAll(), &parseError);
        input.close();
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            throw std::invalid_argument("Invalid project request JSON.");
        auto request = document.object();
        keys(request, {"action", "id", "revision", "options", "proxyUsernameEnv", "proxyPasswordEnv",
                       "timeoutSeconds"});
        auto action = string(request, "action", {}, 32);
        const QSet<QString> actions{"create", "preview", "start", "pause", "resume", "cancel",
                                    "update", "status",  "list",  "open",  "remove"};
        if (!actions.contains(q(action)))
            throw std::invalid_argument("Unknown project action.");
        if ((action == "create" || action == "list") &&
            (request.contains("id") || request.contains("revision")))
            throw std::invalid_argument("This project action does not accept an ID or revision.");
        auto id = string(request, "id", {}, 32);
        auto revision = integer(request, "revision", 0);
        if (action != "create" && action != "list" && (id.empty() || !isOpaqueId(id)))
            throw std::invalid_argument("Project ID is required.");
        if (action != "create" && action != "list" && action != "status" && action != "open" && revision < 1)
            throw std::invalid_argument("Exact project revision is required.");
        if (action != "create" && action != "preview" && action != "start" && action != "resume" &&
            action != "update" &&
            (request.contains("proxyUsernameEnv") || request.contains("proxyPasswordEnv")))
            throw std::invalid_argument("This project action does not accept proxy credentials.");
        if ((action == "create" || action == "update") && !request.value("options").isObject())
            throw std::invalid_argument("Project options are required.");
        if (action != "create" && action != "update" && request.contains("options"))
            throw std::invalid_argument("Options are only accepted for create/update.");
        auto timeout = integer(request, "timeoutSeconds", 60, 300);
        if (timeout < 1)
            throw std::invalid_argument("Invalid project timeout.");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        const auto argument = QString::fromUtf8(argv[3]);
        if (argument.isEmpty() || argument.startsWith("\\\\") || argument.startsWith("//") ||
            std::any_of(argument.begin(), argument.end(),
                        [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; }))
            throw std::invalid_argument("Choose a local project data directory.");
        const auto absoluteData =
            std::filesystem::absolute(std::filesystem::path(argument.toStdWString())).lexically_normal();
        const auto data = QString::fromStdWString(absoluteData.wstring());
        QDir().mkpath(data);
        QLockFile lock(QDir(data).filePath("instance.lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            throw std::runtime_error("This data directory is already in use.");
        auto directory = std::filesystem::path(data.toStdWString());
        auto store = makeSqliteStore(directory / L"tasks.db");
        QueueDependencies dependencies{std::move(store.queues), makeQueueCivilTime(), {}, store.traffic};
        DownloadService downloads(std::move(store.tasks), makeUnifiedTransferFactory(), directory / L"parts",
                                  {}, std::move(dependencies), std::move(scanning));
        SiteProjectService projects(downloads, makeSiteProjectRepository(directory / L"site-projects"),
                                    makeSiteContentProcessor(), makeSiteProjectPublisher());
        if (!projects.startupError().empty())
            throw std::runtime_error(projects.startupError());
        ProxyCredentials credentials{environment(request, "proxyUsernameEnv"),
                                     environment(request, "proxyPasswordEnv")};
        if (action == "create")
            id = await(projects.create(options(request.value("options").toObject()), std::move(credentials)),
                       deadline);
        else if (action == "preview")
            await(projects.preview(id, revision, std::move(credentials)), deadline);
        else if (action == "start")
            await(projects.start(id, revision, std::move(credentials)), deadline);
        else if (action == "pause")
            await(projects.pause(id, revision), deadline);
        else if (action == "resume")
            await(projects.resume(id, revision, std::move(credentials)), deadline);
        else if (action == "cancel")
            await(projects.cancel(id, revision), deadline);
        else if (action == "update")
            await(projects.update(id, revision, options(request.value("options").toObject()),
                                  std::move(credentials)),
                  deadline);
        else if (action == "remove")
            await(projects.removeHistory(id, revision), deadline);
        if (action == "preview" || action == "start" || action == "resume" || action == "update" ||
            action == "pause" || action == "cancel") {
            while (true) {
                auto detail = projects.inspect(id);
                if (!detail)
                    throw std::runtime_error("Project disappeared.");
                const auto state = detail->state;
                if (state == SiteProjectState::PreviewReady || state == SiteProjectState::Completed ||
                    state == SiteProjectState::NeedsAttention || state == SiteProjectState::Cancelled ||
                    state == SiteProjectState::Paused)
                    break;
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Project operation timed out; work is paused on shutdown.");
                QCoreApplication::processEvents();
                QThread::msleep(20);
            }
        }
        QJsonObject answer{{"ok", true}};
        if (action == "list") {
            QJsonArray items;
            for (auto& s : projects.snapshot())
                items.push_back(summary(s));
            answer.insert("projects", items);
        } else if (action != "remove") {
            auto summaries = projects.snapshot();
            auto found =
                std::find_if(summaries.begin(), summaries.end(), [&](const auto& s) { return s.id == id; });
            if (found == summaries.end())
                throw std::invalid_argument("Project is unavailable.");
            if ((action == "status" || action == "open") && request.contains("revision") &&
                revision != found->revision)
                throw std::invalid_argument("Project revision changed.");
            answer.insert("project", summary(*found));
            if (auto detail = projects.inspect(id)) {
                QJsonArray entries;
                for (auto& e : detail->entries)
                    entries.push_back(
                        QJsonObject{{"id", q(e.id)},
                                    {"url", q(e.url)},
                                    {"kind", static_cast<int>(e.kind)},
                                    {"state", QString::fromUtf8(siteProjectEntryStateName(e.state))},
                                    {"bytes", e.bytes},
                                    {"responseUrl", q(e.responseUrl)},
                                    {"contentType", q(e.contentType)},
                                    {"error", q(e.error)}});
                answer.insert("entries", entries);
            }
            if (action == "open") {
                auto path = await(projects.openPath(id), deadline);
                answer.insert("path", QString::fromStdWString(path.wstring()));
            }
            if (found->state == SiteProjectState::NeedsAttention && action != "status" && action != "open") {
                answer["ok"] = false;
                respond(answer);
                return 1;
            }
        }
        await(projects.pauseAll(), std::chrono::steady_clock::now() + std::chrono::seconds(10));
        respond(answer);
        return 0;
    } catch (const std::exception& e) {
        respond({{"ok", false}, {"error", QString::fromUtf8(e.what())}});
        return 1;
    }
}
} // namespace maskpilot
