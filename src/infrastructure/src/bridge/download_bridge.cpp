#include <maskpilot/domain/download_context.h>
#include <maskpilot/infrastructure/download_bridge.h>

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace maskpilot {
namespace {
using namespace std::chrono_literals;
QString qs(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
QString pathText(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qsizetype>(bytes.size()));
}
std::filesystem::path nativePath(const QString& text) {
    const auto bytes = text.toUtf8();
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(bytes.data()),
                                               static_cast<std::size_t>(bytes.size())));
}
[[noreturn]] void invalid() {
    throw std::invalid_argument("Invalid protocol fields or unsupported command.");
}
void fields(const QJsonObject& object, std::initializer_list<QString> allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
            invalid();
}
bool controls(const QString& value);
QString text(const QJsonObject& object, const QString& key, qsizetype limit, bool required = false) {
    if (!object.contains(key)) {
        if (required)
            invalid();
        return {};
    }
    const auto value = object.value(key);
    if (!value.isString() || value.toString().toUtf8().size() > limit || controls(value.toString()))
        invalid();
    return value.toString();
}
bool flag(const QJsonObject& object, const QString& key) {
    if (object.contains(key) && !object.value(key).isBool())
        invalid();
    return object.value(key).toBool();
}
std::int64_t number(const QJsonObject& object, const QString& key, std::int64_t minimum, std::int64_t maximum,
                    std::int64_t fallback = 0) {
    if (!object.contains(key))
        return fallback;
    const auto value = object.value(key);
    const auto number = value.toDouble();
    if (!value.isDouble() || !std::isfinite(number) || std::trunc(number) != number ||
        number < -9007199254740991.0 || number > 9007199254740991.0 ||
        number < static_cast<double>(minimum) || number > static_cast<double>(maximum))
        invalid();
    return static_cast<std::int64_t>(number);
}
std::string id(const QJsonObject& object, const QString& key, bool required = true) {
    const auto value = text(object, key, 32, required).toStdString();
    if ((!value.empty() || required) && !isOpaqueId(value))
        invalid();
    return value;
}
bool controls(const QString& value) {
    return std::any_of(value.begin(), value.end(),
                       [](QChar ch) { return ch.unicode() < 0x20 || ch.unicode() == 0x7f; });
}
std::string httpSource(const QJsonObject& object, const QString& key, bool required = false) {
    const auto raw = text(object, key, 32768, required);
    if (raw.isEmpty() && !required)
        return {};
    auto url = QUrl::fromEncoded(raw.toUtf8(), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "http" && url.scheme() != "https") ||
        !url.userName().isEmpty() || !url.password().isEmpty())
        invalid();
    url.setFragment({});
    return url.toEncoded().toStdString();
}
QJsonObject safeContext(const DownloadContext& context) {
    return {{"id", qs(context.id)},
            {"externalKey", qs(context.externalKey)},
            {"label", qs(context.label)},
            {"downloadRoot", pathText(context.downloadRoot)},
            {"proxyEndpoint", qs(context.proxyEndpoint)},
            {"proxyAuthenticationRequired", context.proxyAuthenticationRequired},
            {"originAuthenticationRequired", context.originAuthenticationRequired},
            {"revision", static_cast<double>(context.revision)},
            {"enabled", context.enabled},
            {"hasStoredCredentials", !context.proxyCredentialRef.empty()}};
}
QJsonObject taskPage(const ScopedTaskPage& page, bool includeRecovery = false) {
    QJsonArray tasks;
    for (const auto& task : page.tasks) {
        QJsonObject value{{"id", qs(task.id)},
                          {"name", qs(task.name)},
                          {"state", QString::fromLatin1(stateName(task.state))},
                          {"downloaded", static_cast<double>(task.downloaded)},
                          {"total", static_cast<double>(task.total)}};
        if (includeRecovery)
            value.insert("canRecover", task.browserRecoveryAvailable);
        tasks.append(value);
    }
    return {{"tasks", tasks}, {"nextCursor", qs(page.nextCursor)}};
}
ScopedTaskCommand control(const QString& command) {
    if (command == "resume")
        return ScopedTaskCommand::Resume;
    if (command == "pause")
        return ScopedTaskCommand::Pause;
    if (command == "cancel")
        return ScopedTaskCommand::Cancel;
    if (command == "remove")
        return ScopedTaskCommand::Remove;
    invalid();
}
} // namespace

struct DownloadBridge::Impl {
    using Answer = std::function<void(bool, QJsonObject, QString)>;
    DownloadService& service;
    std::function<DownloadSpec(const QString&)> defaults;
    std::function<void()> activity;
    QTimer timer;
    std::vector<std::function<bool()>> pending;
    std::string legacyTicket;
    std::int64_t legacyTicketExpires = 0;

    Impl(DownloadService& service, std::function<DownloadSpec(const QString&)> defaults,
         std::function<void()> activity, QObject* owner)
        : service(service), defaults(std::move(defaults)), activity(std::move(activity)) {
        QObject::connect(&timer, &QTimer::timeout, owner, [this] {
            auto batch = std::move(pending);
            pending.clear();
            for (auto& operation : batch)
                if (!operation())
                    pending.push_back(std::move(operation));
        });
        timer.start(10);
    }
    ~Impl() { timer.stop(); }
    template <typename T, typename Transform>
    void await(std::future<T> future, Answer answer, Transform transform, bool acceptedActivity = false) {
        auto value = std::make_shared<std::future<T>>(std::move(future));
        pending.emplace_back([this, value, answer = std::move(answer), transform = std::move(transform),
                              acceptedActivity]() mutable {
            if (value->wait_for(0ms) != std::future_status::ready)
                return false;
            try {
                QJsonObject result;
                if constexpr (std::is_void_v<T>) {
                    value->get();
                    result = transform();
                } else {
                    result = transform(value->get());
                }
                if (acceptedActivity && activity)
                    activity();
                answer(true, std::move(result), {});
            } catch (const std::exception&) {
                answer(false, {}, "Request was rejected. Check input, context authorization and task state.");
            }
            return true;
        });
    }
    void withLegacyTicket(Answer answer, std::function<void(const std::string&, Answer)> action) {
        if (!legacyTicket.empty() && unixTime() < legacyTicketExpires) {
            action(legacyTicket, std::move(answer));
            return;
        }
        // Renewal is actor-authorized standalone access, never a global snapshot fallback.
        auto value = std::make_shared<std::future<std::string>>(service.issueClient("standalone"));
        pending.emplace_back([this, value, action = std::move(action), answer = std::move(answer)]() mutable {
            if (value->wait_for(0ms) != std::future_status::ready)
                return false;
            try {
                legacyTicket = value->get();
                legacyTicketExpires = unixTime() + 3500;
                action(legacyTicket, answer);
            } catch (const std::exception&) {
                answer(false, {}, "Standalone task access is unavailable.");
            }
            return true;
        });
    }
    DownloadSpec legacySpec(const QJsonObject& request, bool paused) {
        const auto raw = text(request, "url", 32768, true);
        auto url = QUrl::fromEncoded(raw.toUtf8(), QUrl::StrictMode);
        if (controls(raw) || !url.isValid() || url.host().isEmpty() ||
            (url.scheme() != "http" && url.scheme() != "https") || !url.userName().isEmpty() ||
            !url.password().isEmpty())
            invalid();
        url.setFragment({});
        if (!defaults)
            invalid();
        auto spec = defaults(QString::fromUtf8(url.toEncoded()));
        spec.url = url.toEncoded().toStdString();
        for (const auto& field : {QString("referrer"), QString("cookie"), QString("userAgent")}) {
            if (!request.contains(field))
                continue;
            const auto value = text(request, field,
                                    field == "cookie"      ? 16384
                                    : field == "userAgent" ? 1024
                                                           : 32768);
            if (controls(value))
                invalid();
            if (field == "referrer")
                spec.referrer = value.toStdString();
            else if (field == "cookie")
                spec.cookie = value.toStdString();
            else
                spec.userAgent = value.toStdString();
        }
        if (request.contains("filename")) {
            const auto filename = text(request, "filename", 720);
            if (filename.size() > 180 || controls(filename) || filename.contains('/') ||
                filename.contains('\\') || filename.contains(':') || filename.contains('<') ||
                filename.contains('>') || filename.contains('"') || filename.contains('|') ||
                filename.contains('?') || filename.contains('*') || filename == "." || filename == ".." ||
                filename.endsWith('.') || filename.endsWith(' '))
                invalid();
            if (!filename.isEmpty())
                spec.destination = spec.destination.parent_path() / nativePath(filename);
        }
        spec.kind = flag(request, "media") ? TransferKind::Media : TransferKind::Http;
        spec.audioOnly = flag(request, "audioOnly");
        spec.startPaused = paused;
        if (spec.kind == TransferKind::Media) {
            spec.category = spec.audioOnly ? "Music" : "Video";
            spec.destination.replace_extension(spec.audioOnly ? ".m4a" : ".mkv");
            if (!QFileInfo(pathText(spec.mediaDownloader)).isExecutable() ||
                !QFileInfo(pathText(spec.mediaMuxer)).isExecutable())
                throw std::invalid_argument("Media components are missing.");
        }
        if (!validateSpec(spec).empty())
            invalid();
        return spec;
    }
    void legacy(const QJsonObject& request, const QString& command, const std::string& requestId,
                Answer answer) {
        if (command == "add" || command == "prepare") {
            fields(request, {"version", "requestId", "command", "url", "filename", "referrer", "cookie",
                             "userAgent", "media", "audioOnly"});
            auto spec = legacySpec(request, command == "prepare");
            await(
                service.add(std::move(spec), requestId), std::move(answer),
                [](const std::string& value) { return QJsonObject{{"taskId", qs(value)}}; }, true);
        } else if (command == "recovery.begin") {
            fields(request, {"version", "requestId", "command", "taskId"});
            await(
                service.beginBrowserRecovery(id(request, "taskId")), std::move(answer),
                [](const BrowserRecoverySession& session) {
                    return QJsonObject{{"taskId", qs(session.taskId)},
                                       {"recoveryToken", qs(session.token)},
                                       {"name", qs(session.name)},
                                       {"pageUrl", qs(session.pageUrl)},
                                       {"expiresAt", static_cast<double>(session.expiresAt)}};
                },
                true);
        } else if (command == "recovery.apply") {
            fields(request, {"version", "requestId", "command", "recoveryToken", "url", "cookie", "referrer",
                             "userAgent"});
            BrowserRecoveryMaterial material{
                httpSource(request, "url", true), text(request, "cookie", 16384).toStdString(),
                httpSource(request, "referrer"), text(request, "userAgent", 1024).toStdString()};
            await(
                service.applyBrowserRecovery(id(request, "recoveryToken"), std::move(material)),
                std::move(answer),
                [](const std::string& value) { return QJsonObject{{"taskId", qs(value)}}; }, true);
        } else if (command == "recovery.resume") {
            fields(request, {"version", "requestId", "command", "recoveryToken"});
            await(
                service.resumeBrowserRecovery(id(request, "recoveryToken")), std::move(answer),
                [](const std::string& value) { return QJsonObject{{"taskId", qs(value)}}; }, true);
        } else if (command == "recovery.cancel") {
            fields(request, {"version", "requestId", "command", "recoveryToken"});
            await(service.cancelBrowserRecovery(id(request, "recoveryToken")), std::move(answer),
                  [] { return QJsonObject{}; });
        } else if (command == "ping") {
            fields(request, {"version", "requestId", "command"});
            answer(true, {}, {});
        } else if (command == "list") {
            fields(request, {"version", "requestId", "command"});
            withLegacyTicket(std::move(answer), [this](const auto& token, Answer result) {
                await(service.listScoped(token), std::move(result),
                      [](const auto& page) { return taskPage(page, true); });
            });
        } else {
            fields(request, {"version", "requestId", "command", "taskId"});
            const auto taskId = id(request, "taskId");
            const auto action = control(command);
            withLegacyTicket(std::move(answer), [this, taskId, action](const auto& token, Answer result) {
                await(
                    service.controlScoped(token, action, taskId), std::move(result),
                    [taskId] { return QJsonObject{{"taskId", qs(taskId)}}; },
                    action == ScopedTaskCommand::Resume);
            });
        }
    }
    void scoped(const QJsonObject& params, const QString& command, const std::string& requestId,
                const std::string& token, Answer answer) {
        if (!isOpaqueId(token))
            invalid();
        if (command == "add" || command == "prepare") {
            fields(params,
                   command == "prepare"
                       ? std::initializer_list<QString>{"url", "filename", "originGrantId"}
                       : std::initializer_list<QString>{"url", "filename", "originGrantId", "paused"});
            ScopedDownloadDraft draft;
            draft.url = text(params, "url", 32768, true).toStdString();
            draft.filename = text(params, "filename", 1024, true).toStdString();
            draft.originGrantId = id(params, "originGrantId", false);
            draft.startPaused = command == "prepare" || flag(params, "paused");
            await(
                service.addScoped(token, draft, requestId), std::move(answer),
                [](const std::string& value) { return QJsonObject{{"taskId", qs(value)}}; }, true);
        } else if (command == "list") {
            fields(params, {"limit", "cursor"});
            const auto limit = number(params, "limit", 1, 1000, 1000);
            const auto cursor = id(params, "cursor", false);
            await(service.listScoped(token, static_cast<std::size_t>(limit), cursor), std::move(answer),
                  [](const auto& page) { return taskPage(page); });
        } else if (command == "ping") {
            fields(params, {});
            await(service.listScoped(token, 1), std::move(answer), [](const auto&) { return QJsonObject{}; });
        } else {
            fields(params, {"taskId"});
            const auto taskId = id(params, "taskId");
            const auto action = control(command);
            await(
                service.controlScoped(token, action, taskId), std::move(answer),
                [taskId] { return QJsonObject{{"taskId", qs(taskId)}}; },
                action == ScopedTaskCommand::Resume);
        }
    }
    void administrative(const QJsonObject& params, const QString& command, Answer answer) {
        if (command == "context.configure") {
            fields(params, {"id", "expectedRevision", "externalKey", "label", "downloadRoot", "proxyEndpoint",
                            "proxyAuthenticationRequired", "originAuthenticationRequired", "reactivate",
                            "rememberProxy", "proxyCredentials"});
            ContextConfiguration draft;
            draft.id = id(params, "id", false);
            draft.expectedRevision = number(params, "expectedRevision", 0, 9007199254740991LL);
            draft.externalKey = text(params, "externalKey", 256, true).toStdString();
            draft.label = text(params, "label", 256, true).toStdString();
            draft.downloadRoot = nativePath(text(params, "downloadRoot", 32768, true));
            draft.proxyEndpoint = text(params, "proxyEndpoint", 2048).toStdString();
            draft.proxyAuthenticationRequired = flag(params, "proxyAuthenticationRequired");
            draft.originAuthenticationRequired = flag(params, "originAuthenticationRequired");
            draft.reactivate = flag(params, "reactivate");
            const bool remember = flag(params, "rememberProxy");
            ProxyCredentials credentials;
            if (params.contains("proxyCredentials")) {
                if (!params.value("proxyCredentials").isObject())
                    invalid();
                const auto object = params.value("proxyCredentials").toObject();
                fields(object, {"username", "password"});
                credentials.username = text(object, "username", 4096, true).toStdString();
                credentials.password = text(object, "password", 4096, true).toStdString();
            }
            await(service.configureContext(std::move(draft), std::move(credentials), remember),
                  std::move(answer),
                  [](const auto& context) { return QJsonObject{{"context", safeContext(context)}}; });
        } else if (command == "context.list") {
            fields(params, {});
            await(service.contexts(), std::move(answer), [](const auto& contexts) {
                QJsonArray result;
                for (const auto& context : contexts)
                    result.append(safeContext(context));
                return QJsonObject{{"contexts", result}};
            });
        } else if (command == "context.issue") {
            fields(params, {"contextId"});
            await(service.issueClient(id(params, "contextId")), std::move(answer),
                  [](const auto& token) { return QJsonObject{{"sessionToken", qs(token)}}; });
        } else if (command == "origin.grant") {
            fields(params, {"contextId", "url", "cookie", "authorization", "expiresAt"});
            if (!params.contains("expiresAt"))
                invalid();
            await(service.issueOriginGrant(id(params, "contextId"),
                                           text(params, "url", 32768, true).toStdString(),
                                           text(params, "cookie", 16384).toStdString(),
                                           text(params, "authorization", 16384).toStdString(),
                                           number(params, "expiresAt", 1, 9007199254740991LL)),
                  std::move(answer),
                  [](const auto& grant) { return QJsonObject{{"originGrantId", qs(grant)}}; });
        } else if (command == "context.revoke") {
            fields(params, {"contextId"});
            await(service.revokeContext(id(params, "contextId")), std::move(answer),
                  [] { return QJsonObject{}; });
        } else if (command == "task.rebind") {
            fields(params, {"taskId", "originGrantId"});
            const auto taskId = id(params, "taskId");
            await(service.rebindTask(taskId, id(params, "originGrantId", false)), std::move(answer),
                  [taskId] { return QJsonObject{{"taskId", qs(taskId)}}; });
        } else
            invalid();
    }
};

DownloadBridge::DownloadBridge(DownloadService& service,
                               std::function<DownloadSpec(const QString&)> legacyDefaults,
                               std::function<void()> acceptedActivity, QObject* parent)
    : QObject(parent),
      impl_(std::make_unique<Impl>(service, std::move(legacyDefaults), std::move(acceptedActivity), this)) {}
DownloadBridge::~DownloadBridge() = default;
void DownloadBridge::handle(QJsonObject request, BridgeAccess access,
                            std::function<void(QJsonObject)> respond) {
    const auto requestId = request.value("requestId").toString();
    const auto version = request.value("version").toDouble();
    Impl::Answer answer = [respond = std::move(respond), requestId, version](bool ok, QJsonObject result,
                                                                             QString error) mutable {
        result.insert("version", version == 2 ? 2 : 1);
        result.insert("requestId", isOpaqueId(requestId.toStdString()) ? requestId : QString{});
        result.insert("ok", ok);
        if (!error.isEmpty())
            result.insert("error", error);
        respond(std::move(result));
    };
    try {
        if (impl_->pending.size() >= 256 ||
            QJsonDocument(request).toJson(QJsonDocument::Compact).size() > 1024 * 1024)
            throw std::invalid_argument("Bridge is busy or request exceeds its limit.");
        if (!request.value("version").isDouble() || (version != 1 && version != 2) ||
            !isOpaqueId(requestId.toStdString()))
            invalid();
        const auto command = text(request, "command", 64, true);
        if (version == 1) {
            if (access.authority != BridgeAuthority::Legacy)
                invalid();
            impl_->legacy(request, command, requestId.toStdString(), answer);
        } else {
            fields(request, {"version", "requestId", "command", "params"});
            if (access.authority == BridgeAuthority::Legacy ||
                (request.contains("params") && !request.value("params").isObject()))
                invalid();
            const auto params = request.value("params").toObject();
            if (access.authority == BridgeAuthority::Administrative)
                impl_->administrative(params, command, answer);
            else
                impl_->scoped(params, command, requestId.toStdString(), access.sessionToken, answer);
        }
    } catch (const std::exception&) {
        answer(false, {}, "Invalid or unauthorized download bridge request.");
    }
}
} // namespace maskpilot
