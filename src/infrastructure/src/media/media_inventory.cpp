#include "media_process.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <maskpilot/infrastructure/media_tools.h>
#include <set>
#include <stdexcept>
#include <string_view>
namespace maskpilot {
namespace media_process {
namespace {
std::string label(QString text, int maximum) {
    for (auto& ch : text)
        if (ch.unicode() < 32 || ch.unicode() == 127)
            ch = QChar(' ');
    text = text.trimmed();
    if (text.contains("://"))
        text = "Media item";
    while (text.toUtf8().size() > maximum)
        text.chop(1);
    return text.toUtf8().toStdString();
}
double number(const QJsonObject& object, const char* key, double fallback, double maximum) {
    auto value = object.value(key);
    if (value.isUndefined() || value.isNull())
        return fallback;
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0 ||
        value.toDouble() > maximum)
        throw std::runtime_error("Media inventory contains invalid numeric fields.");
    return value.toDouble();
}
std::string field(const QJsonObject& object, const char* key, int maximum) {
    auto value = object.value(key);
    if (value.isUndefined() || value.isNull())
        return {};
    if (!value.isString())
        throw std::runtime_error("Media inventory contains invalid labels.");
    if (std::string_view(key) == "title")
        return label(value.toString(), maximum);
    const auto text = value.toString().toUtf8();
    if (text.size() > maximum ||
        std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::runtime_error("Media inventory contains invalid labels.");
    return text.toStdString();
}
bool drm(const QJsonObject& object) {
    auto value = object.value("has_drm");
    if (!value.isUndefined() && !value.isNull() && !value.isBool())
        throw std::runtime_error("Media inventory contains invalid protection metadata.");
    return value.toBool();
}
} // namespace
MediaInventory parseInventory(const QByteArray& data) {
    if (data.isEmpty() || data.size() > 8 * 1024 * 1024)
        throw std::runtime_error("Media inventory exceeded its output limit.");
    QJsonParseError error;
    auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Media parser returned an invalid inventory.");
    auto object = document.object();
    const auto live = object.value("is_live");
    if (!live.isUndefined() && !live.isNull() && !live.isBool())
        throw std::runtime_error("Media inventory contains invalid live metadata.");
    const auto liveStatus = object.value("live_status").toString();
    if (object.contains("entries") || object.value("_type").toString() == "playlist" ||
        object.value("is_live").toBool() || liveStatus == "is_live" || liveStatus == "is_upcoming" ||
        liveStatus == "post_live" || drm(object))
        throw std::runtime_error("Live, playlist and protected media are unsupported.");
    auto formats = object.value("formats");
    if (!formats.isArray() || formats.toArray().size() > 2000)
        throw std::runtime_error("Media inventory has an invalid format count.");
    MediaInventory inventory;
    inventory.title = field(object, "title", 256);
    if (inventory.title.empty())
        inventory.title = "Media item";
    std::set<std::string> identifiers;
    for (const auto& item : formats.toArray()) {
        if (!item.isObject())
            throw std::runtime_error("Media inventory contains an invalid format.");
        auto format = item.toObject();
        const auto identifier = field(format, "format_id", 128);
        if (!validMediaFormatId(identifier) || !identifiers.insert(identifier).second)
            throw std::runtime_error("Media inventory contains ambiguous or unsafe identifiers.");
        if (drm(format))
            continue;
        auto protocol = field(format, "protocol", 64);
        if (protocol != "http" && protocol != "https" && protocol != "m3u8" && protocol != "m3u8_native" &&
            protocol != "http_dash_segments")
            continue;
        MediaFormat result;
        result.id = field(format, "format_id", 128);
        if (!validMediaFormatId(result.id))
            throw std::runtime_error("Media inventory contains an unsafe format identifier.");
        result.extension = field(format, "ext", 32);
        result.videoCodec = field(format, "vcodec", 128);
        result.audioCodec = field(format, "acodec", 128);
        result.protocol = protocol;
        const bool audioExtension = result.extension == "m4a" || result.extension == "mp3" ||
                                    result.extension == "aac" || result.extension == "opus" ||
                                    result.extension == "flac" || result.extension == "ogg" ||
                                    result.extension == "wav";
        if (result.videoCodec.empty())
            result.videoCodec = audioExtension ? "none" : "unknown";
        if (result.audioCodec.empty())
            result.audioCodec = "unknown";
        result.hasVideo = result.videoCodec != "none";
        result.hasAudio = result.audioCodec != "none";
        if (!result.hasVideo && !result.hasAudio)
            continue;
        const auto width = number(format, "width", 0, 65536), height = number(format, "height", 0, 65536);
        if (std::floor(width) != width || std::floor(height) != height)
            throw std::runtime_error("Media inventory contains invalid dimensions.");
        result.width = static_cast<int>(width);
        result.height = static_cast<int>(height);
        result.fps = number(format, "fps", 0, 1000);
        result.bitrate = number(format, "tbr", 0, 1e9);
        auto bytes = number(format, "filesize", -1, 9007199254740991.0);
        if (bytes < 0) {
            bytes = number(format, "filesize_approx", -1, 9007199254740991.0);
            result.estimated = bytes >= 0;
        }
        if (bytes >= 0 && std::floor(bytes) != bytes)
            throw std::runtime_error("Media inventory contains an invalid file size.");
        result.bytes = static_cast<std::int64_t>(bytes);
        inventory.formats.push_back(std::move(result));
    }
    if (auto message = validateMediaInventory(inventory); !message.empty())
        throw std::runtime_error(message);
    return inventory;
}
} // namespace media_process
namespace {
class MediaInspector final : public IMediaInspector {
  public:
    MediaInventory inspect(const DownloadSpec& spec, std::stop_token stop) override {
        media_process::validateRequest(spec);
        if (stop.stop_requested())
            throw std::runtime_error("Media inspection cancelled.");
        QTemporaryDir temporary(QDir::tempPath() + "/maskpilot-media-inspection-XXXXXX");
        if (!temporary.isValid())
            throw std::runtime_error("Cannot create private media inspection workspace.");
        QProcess process;
        media_process::ProcessTree tree;
        auto args = media_process::commonArguments();
        args << "--dump-single-json" << "--skip-download" << "--no-warnings";
        const auto started = std::chrono::steady_clock::now();
        if (!media_process::launch(process, tree, spec, args,
                                   std::filesystem::path(temporary.path().toStdU16String()),
                                   [&] { return stop.stop_requested(); }))
            throw std::runtime_error("Media inspection cancelled.");
        QByteArray output, diagnostics;
        while (process.state() != QProcess::NotRunning) {
            if (stop.stop_requested()) {
                tree.stop(process);
                throw std::runtime_error("Media inspection cancelled.");
            }
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(60)) {
                tree.stop(process);
                throw std::runtime_error("Media inspection exceeded its time limit.");
            }
            process.waitForReadyRead(50);
            output += process.readAllStandardOutput();
            diagnostics = (diagnostics + process.readAllStandardError()).right(16384);
            if (output.size() > 8 * 1024 * 1024) {
                tree.stop(process);
                throw std::runtime_error("Media inventory exceeded its output limit.");
            }
        }
        output += process.readAllStandardOutput();
        diagnostics.clear();
        process.readAllStandardError();
        if (stop.stop_requested())
            throw std::runtime_error("Media inspection cancelled.");
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            throw std::runtime_error(
                "Media inspection failed. Check the source, proxy credentials and supported components.");
        return media_process::parseInventory(output);
    }
};
} // namespace
std::shared_ptr<IMediaInspector> makeMediaInspector() { return std::make_shared<MediaInspector>(); }
} // namespace maskpilot
