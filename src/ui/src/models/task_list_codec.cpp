#include "task_list_codec.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cmath>
#include <stdexcept>

namespace maskpilot::ui {
namespace {
constexpr qsizetype maxBytes = 32 * 1024 * 1024;
[[noreturn]] void invalid() {
    throw std::invalid_argument("Invalid task list. Existing tasks were preserved.");
}
QString text(const std::string& value) { return QString::fromStdString(value); }
QString pathText(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qsizetype>(bytes.size()));
}
std::filesystem::path path(const QString& value) {
    const auto bytes = value.toUtf8();
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(bytes.data()),
                                               static_cast<std::size_t>(bytes.size())));
}
void keys(const QJsonObject& object, const QSet<QString>& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key()))
            invalid();
}
QString string(const QJsonObject& object, const char* key, QString fallback = {}) {
    if (!object.contains(key))
        return fallback;
    const auto value = object.value(key);
    if (!value.isString() || value.toString().contains(QChar::Null))
        invalid();
    return value.toString();
}
bool boolean(const QJsonObject& object, const char* key, bool fallback = false, bool required = false) {
    if (!object.contains(key)) {
        if (required)
            invalid();
        return fallback;
    }
    if (!object.value(key).isBool())
        invalid();
    return object.value(key).toBool();
}
int integer(const QJsonObject& object, const char* key, int fallback, int minimum, int maximum) {
    if (!object.contains(key))
        return fallback;
    const auto value = object.value(key);
    const auto number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number != std::floor(number) || number < minimum ||
        number > maximum)
        invalid();
    return static_cast<int>(number);
}
std::int64_t storedBytes(const QJsonObject& object) {
    if (!object.contains("maxStoredBytes"))
        return 0;
    const auto value = object.value("maxStoredBytes");
    const auto number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number != std::floor(number) || number < 0 ||
        number > 9007199254740991.0)
        invalid();
    return static_cast<std::int64_t>(number);
}
} // namespace
QByteArray encodeTaskList(const std::vector<Task>& tasks) {
    if (tasks.size() > 10000)
        invalid();
    QJsonArray array;
    qsizetype estimatedBytes = 0;
    for (const auto& task : tasks) {
        const auto& spec = task.spec;
        if (isEd2kUrl(spec.url) ? (!validateSpec(spec).empty() || task.ownerId != "standalone")
                                : spec.ed2k != Ed2kOptions{})
            invalid();
        if (!validateMediaOrigins(spec).empty())
            invalid();
        if (!validateSftpOptions(spec.sftp, isSftpUrl(spec.url)).empty())
            invalid();
        if (!validateHttpRequest(spec.httpRequest).empty())
            invalid();
        if (!validateMediaSelection(spec.mediaSelection, spec.audioOnly).empty())
            invalid();
        if (spec.maxStoredBytes < 0 || spec.maxStoredBytes > 9007199254740991LL ||
            ((spec.maxStoredBytes || spec.sameOriginRedirectOnly) &&
             (sourceKind(spec) != TransferKind::Http ||
              !(spec.url.starts_with("http://") || spec.url.starts_with("https://")))))
            invalid();
        QJsonObject object{{"url", text(spec.url)},
                           {"destination", pathText(spec.destination)},
                           {"category", text(spec.category)},
                           {"connections", spec.connections},
                           {"proxy", text(spec.proxy)},
                           {"proxyAuthenticationRequired", spec.proxyAuthenticationRequired ||
                                                               !spec.proxyUsername.empty() ||
                                                               !spec.proxyPassword.empty()},
                           {"originAuthenticationRequired",
                            task.originAuthenticationRequired || requiresOriginAuthentication(spec)},
                           {"contextBound", task.ownerId != "standalone"},
                           {"media", spec.kind == TransferKind::Media},
                           {"audioOnly", spec.audioOnly},
                           {"mediaFormat", text(spec.mediaFormat)},
                           {"sha256", text(spec.expectedSha256)},
                           {"ftpTls", spec.ftpExplicitTls},
                           {"ftpActive", spec.ftpActiveMode},
                           {"ftpCa", pathText(spec.ftpCaBundle)}};
        if (!validateLiveRecording(spec.live).empty() ||
            (spec.kind != TransferKind::Live && spec.live != LiveRecordingOptions{}))
            invalid();
        if (spec.kind == TransferKind::Live)
            object.insert("live", QJsonObject{{"format", static_cast<int>(spec.live.format)},
                                              {"durationSeconds", spec.live.durationSeconds},
                                              {"maxBytes", static_cast<double>(spec.live.maxBytes)}});
        if (isEd2kUrl(spec.url)) {
            QJsonArray servers, sources;
            for (const auto& endpoint : spec.ed2k.servers)
                servers.append(text(formatEd2kEndpoint(endpoint)));
            for (const auto& endpoint : spec.ed2k.sources)
                sources.append(text(formatEd2kEndpoint(endpoint)));
            object.insert("ed2k", QJsonObject{{"servers", servers}, {"sources", sources}});
        }
        if (isSftpUrl(spec.url))
            object.insert("sftp",
                          QJsonObject{{"authentication", static_cast<int>(spec.sftp.authentication)},
                                      {"hostKeySha256", text(canonicalSftpHostKey(spec.sftp.hostKeySha256))},
                                      {"privateKeyFile", pathText(spec.sftp.privateKeyFile)}});
        if (spec.httpRequest.method == HttpMethod::Post) {
            object.insert("httpMethod", "POST");
            object.insert("postContentType", text(spec.httpRequest.contentType));
        }
        if (spec.maxStoredBytes)
            object.insert("maxStoredBytes", static_cast<double>(spec.maxStoredBytes));
        if (spec.sameOriginRedirectOnly)
            object.insert("sameOriginRedirectOnly", true);
        if (!spec.mediaOrigins.empty())
            object.insert("mediaOrigins", text(encodeMediaOrigins(spec.mediaOrigins)));
        if (spec.kind == TransferKind::Media)
            object.insert("mediaSelection",
                          QJsonObject{{"mode", static_cast<int>(spec.mediaSelection.mode)},
                                      {"primaryFormat", text(spec.mediaSelection.primaryFormat)},
                                      {"audioFormat", text(spec.mediaSelection.audioFormat)},
                                      {"container", static_cast<int>(spec.mediaSelection.container)}});
        if (spec.kind == TransferKind::BitTorrent) {
            QJsonArray selected;
            for (const auto index : spec.torrent.selectedFiles)
                selected.append(index);
            QByteArray metadata;
            if (spec.torrent.metadata) {
                if (spec.torrent.metadata->size() > maxTorrentMetadataBytes)
                    invalid();
                metadata = QByteArray(spec.torrent.metadata->data(),
                                      static_cast<qsizetype>(spec.torrent.metadata->size()))
                               .toBase64();
            }
            object.insert("torrent",
                          QJsonObject{{"allFiles", spec.torrent.allFiles},
                                      {"files", selected},
                                      {"seedAfterDownload", spec.torrent.seedAfterDownload},
                                      {"uploadLimit", static_cast<double>(spec.torrent.uploadLimit)},
                                      {"metadata", QString::fromLatin1(metadata)}});
        }
        estimatedBytes += QJsonDocument(object).toJson(QJsonDocument::Compact).size() + 2;
        if (estimatedBytes > maxBytes - 128)
            invalid();
        array.append(object);
    }
    const auto result =
        QJsonDocument(QJsonObject{{"version", 2}, {"downloads", array}}).toJson(QJsonDocument::Compact);
    if (result.size() > maxBytes)
        invalid();
    return result;
}
DecodedTaskList decodeTaskList(const QByteArray& bytes,
                               const std::function<DownloadSpec(const QString&)>& defaults) {
    if (bytes.size() > maxBytes || !defaults)
        invalid();
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        invalid();
    const auto root = document.object();
    keys(root, {"version", "downloads"});
    const auto version = integer(root, "version", 0, 1, 2);
    if (version == 0 || !root.value("downloads").isArray())
        invalid();
    const auto array = root.value("downloads").toArray();
    if (array.size() > 10000)
        invalid();
    DecodedTaskList result;
    result.version = version;
    std::size_t metadataBytes = 0;
    for (const auto& entry : array) {
        if (!entry.isObject())
            invalid();
        const auto object = entry.toObject();
        if (object.contains("responseContentType") || object.contains("response_content_type"))
            invalid();
        keys(object, {"url",
                      "destination",
                      "category",
                      "connections",
                      "proxy",
                      "proxyAuthenticationRequired",
                      "originAuthenticationRequired",
                      "contextBound",
                      "media",
                      "audioOnly",
                      "mediaFormat",
                      "sha256",
                      "ftpTls",
                      "ftpActive",
                      "ftpCa",
                      "torrent",
                      "mediaSelection",
                      "maxStoredBytes",
                      "sameOriginRedirectOnly",
                      "httpMethod",
                      "postContentType",
                      "sftp",
                      "ed2k",
                      "live",
                      "mediaOrigins"});
        if (version != 2 && (object.contains("maxStoredBytes") || object.contains("sameOriginRedirectOnly")))
            invalid();
        if (version != 2 && (object.contains("httpMethod") || object.contains("postContentType")))
            invalid();
        const auto method = string(object, "httpMethod", "GET");
        if (method != "GET" && method != "POST")
            invalid();
        if (method == "GET" && object.contains("postContentType"))
            invalid();
        const auto url = string(object, "url");
        if (url.isEmpty() ||
            url.toUtf8().size() >
                static_cast<qsizetype>(isEd2kUrl(url.toStdString()) ? maxEd2kLinkBytes : 32768))
            invalid();
        auto spec = defaults(url);
        spec.url = url.toStdString();
        spec.mediaOrigins.clear();
        spec.mediaOriginsConfirmed = false;
        if (object.contains("mediaOrigins")) {
            if (version != 2)
                invalid();
            spec.mediaOrigins = decodeMediaOrigins(string(object, "mediaOrigins").toStdString());
        }
        spec.httpRequest = {};
        spec.sftp = {};
        spec.ed2k = {};
        if (object.contains("ed2k")) {
            if (version != 2 || !isEd2kUrl(spec.url) || !object["ed2k"].isObject())
                invalid();
            const auto options = object["ed2k"].toObject();
            keys(options, {"servers", "sources"});
            for (const auto* key : {"servers", "sources"}) {
                if (!options.contains(key))
                    continue;
                if (!options[key].isArray() ||
                    options[key].toArray().size() > static_cast<qsizetype>(maxEd2kSources))
                    invalid();
                auto& endpoints = std::string_view(key) == "servers" ? spec.ed2k.servers : spec.ed2k.sources;
                for (const auto& endpoint : options[key].toArray()) {
                    if (!endpoint.isString())
                        invalid();
                    endpoints.push_back(parseEd2kEndpoint(endpoint.toString().toStdString(),
                                                          std::string_view(key) == "servers"));
                }
            }
        }
        if (object.contains("sftp")) {
            if (version != 2 || !isSftpUrl(spec.url) || !object["sftp"].isObject())
                invalid();
            const auto ssh = object["sftp"].toObject();
            keys(ssh, {"authentication", "hostKeySha256", "privateKeyFile"});
            if (!ssh.contains("authentication"))
                invalid();
            spec.sftp.authentication =
                static_cast<SftpAuthentication>(integer(ssh, "authentication", 0, 0, 1));
            spec.sftp.hostKeySha256 = string(ssh, "hostKeySha256").toStdString();
            spec.sftp.privateKeyFile = path(string(ssh, "privateKeyFile"));
        }
        if (method == "POST") {
            spec.httpRequest.method = HttpMethod::Post;
            spec.httpRequest.contentType =
                string(object, "postContentType", "application/x-www-form-urlencoded").toStdString();
        }
        spec.maxStoredBytes = storedBytes(object);
        spec.sameOriginRedirectOnly = boolean(object, "sameOriginRedirectOnly");
        spec.cookie.clear();
        spec.authorization.clear();
        spec.headers.clear();
        spec.ftpUsername.clear();
        spec.ftpPassword.clear();
        spec.proxyUsername.clear();
        spec.proxyPassword.clear();
        spec.destination = path(string(object, "destination"));
        spec.category = string(object, "category", "General").toStdString();
        spec.connections = integer(object, "connections", 4, 1, 8);
        spec.proxy = string(object, "proxy", text(spec.proxy)).toStdString();
        spec.proxyAuthenticationRequired =
            boolean(object, "proxyAuthenticationRequired", false, version == 2);
        spec.originAuthenticationRequired =
            boolean(object, "originAuthenticationRequired", false, version == 2);
        if (boolean(object, "contextBound"))
            ++result.contextBoundCount;
        spec.expectedSha256 = string(object, "sha256").toStdString();
        spec.kind = boolean(object, "media") ? TransferKind::Media : TransferKind::Http;
        spec.live = {};
        if (object.contains("live")) {
            if (version != 2 || spec.kind == TransferKind::Media || !object["live"].isObject())
                invalid();
            const auto live = object["live"].toObject();
            keys(live, {"format", "durationSeconds", "maxBytes"});
            if (!live.contains("format") || !live.contains("durationSeconds") || !live.contains("maxBytes"))
                invalid();
            spec.kind = TransferKind::Live;
            spec.live.format = static_cast<LiveFormat>(integer(live, "format", -1, 0, 1));
            spec.live.durationSeconds = integer(live, "durationSeconds", 0, 1, 172800);
            const auto n = live["maxBytes"].toDouble(-1);
            if (!live["maxBytes"].isDouble() || !std::isfinite(n) || std::floor(n) != n || n < 1048576 ||
                n > 1099511627776.0)
                invalid();
            spec.live.maxBytes = static_cast<std::int64_t>(n);
        }
        spec.audioOnly = boolean(object, "audioOnly");
        spec.mediaSelection = {};
        if (object.contains("mediaSelection")) {
            if (version != 2 || !object.value("mediaSelection").isObject() ||
                spec.kind != TransferKind::Media)
                invalid();
            const auto selection = object.value("mediaSelection").toObject();
            keys(selection, {"mode", "primaryFormat", "audioFormat", "container"});
            if (!selection.contains("mode") || !selection.contains("container") ||
                !selection.value("primaryFormat").isString() || !selection.value("audioFormat").isString())
                invalid();
            spec.mediaSelection.mode = static_cast<MediaSelectionMode>(integer(selection, "mode", 0, 0, 4));
            spec.mediaSelection.container =
                static_cast<MediaContainer>(integer(selection, "container", 0, 0, 7));
            spec.mediaSelection.primaryFormat = string(selection, "primaryFormat").toStdString();
            spec.mediaSelection.audioFormat = string(selection, "audioFormat").toStdString();
            if (!validateSftpOptions(spec.sftp, isSftpUrl(spec.url)).empty())
                invalid();
            if (!validateHttpRequest(spec.httpRequest).empty())
                invalid();
            if (!validateMediaSelection(spec.mediaSelection, spec.audioOnly).empty())
                invalid();
        }
        spec.mediaFormat = string(object, "mediaFormat", text(spec.mediaFormat)).toStdString();
        spec.ftpExplicitTls = boolean(object, "ftpTls");
        spec.ftpActiveMode = boolean(object, "ftpActive");
        spec.ftpCaBundle = path(string(object, "ftpCa"));
        spec.torrent = {};
        if (object.contains("torrent")) {
            if (!object.value("torrent").isObject() || !isTorrentUrl(spec.url))
                invalid();
            const auto torrent = object.value("torrent").toObject();
            keys(torrent, {"allFiles", "files", "seedAfterDownload", "uploadLimit", "metadata"});
            spec.torrent.allFiles = boolean(torrent, "allFiles", true, true);
            spec.torrent.seedAfterDownload = boolean(torrent, "seedAfterDownload", false, true);
            if (!torrent.contains("uploadLimit"))
                invalid();
            spec.torrent.uploadLimit = integer(torrent, "uploadLimit", 0, 0, 2147483647);
            if (!torrent.value("files").isArray())
                invalid();
            const auto selected = torrent.value("files").toArray();
            if (selected.size() > static_cast<qsizetype>(maxTorrentFiles))
                invalid();
            for (const auto& index : selected) {
                if (!index.isDouble() || index.toDouble(-1) != index.toInt(-1) || index.toInt(-1) < 0)
                    invalid();
                spec.torrent.selectedFiles.push_back(index.toInt());
            }
            if (!torrent.value("metadata").isString())
                invalid();
            const auto encoded = string(torrent, "metadata").toLatin1();
            if (encoded.size() > static_cast<qsizetype>((maxTorrentMetadataBytes + 2) / 3 * 4))
                invalid();
            if (!encoded.isEmpty()) {
                const auto metadata =
                    QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
                if (metadata.isEmpty() || metadata.size() > static_cast<qsizetype>(maxTorrentMetadataBytes))
                    invalid();
                metadataBytes += static_cast<std::size_t>(metadata.size());
                if (metadataBytes > 24 * 1024 * 1024)
                    invalid();
                spec.torrent.metadata =
                    std::make_shared<const std::vector<char>>(metadata.begin(), metadata.end());
            }
        }
        spec.kind = sourceKind(spec);
        spec.startPaused = true;
        if (!validateSpec(spec).empty())
            invalid();
        result.downloads.push_back(std::move(spec));
    }
    return result;
}
} // namespace maskpilot::ui
