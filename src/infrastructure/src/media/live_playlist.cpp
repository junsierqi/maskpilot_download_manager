#include "live_playlist.h"
#include <QDateTime>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace maskpilot::live {
namespace {
[[noreturn]] void invalid() {
    throw std::runtime_error("Unsupported or malformed live HLS playlist. Check keys, ranges and track "
                             "alignment; DRM, gaps and delta playlists are unsupported.");
}
std::int64_t number(const QString& text) {
    bool ok = false;
    const auto value = text.toLongLong(&ok);
    if (!ok || text.isEmpty() ||
        !std::all_of(text.begin(), text.end(), [](QChar c) { return c >= '0' && c <= '9'; }) || value < 0 ||
        value > std::numeric_limits<std::int64_t>::max() - 4096)
        invalid();
    return value;
}
QMap<QString, QString> attributes(QString text) {
    QMap<QString, QString> out;
    while (!text.isEmpty()) {
        const auto equal = text.indexOf('=');
        if (equal <= 0)
            invalid();
        const auto name = text.left(equal);
        if (!std::all_of(
                name.begin(), name.end(),
                [](QChar c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; }) ||
            out.contains(name))
            invalid();
        text.remove(0, equal + 1);
        if ((name == "URI" || name == "GROUP-ID" || name == "AUDIO" || name == "BYTERANGE") &&
            !text.startsWith('"'))
            invalid();
        QString value;
        if (text.startsWith('"')) {
            const auto close = text.indexOf('"', 1);
            if (close < 0)
                invalid();
            value = text.mid(1, close - 1);
            text.remove(0, close + 1);
            if (value.contains('\\'))
                invalid();
        } else {
            const auto comma = text.indexOf(',');
            value = comma < 0 ? text : text.left(comma);
            text.remove(0, value.size());
            if (value.isEmpty() || value.contains('"'))
                invalid();
        }
        out.insert(name, value);
        if (!text.isEmpty()) {
            if (!text.startsWith(',') || text.size() == 1)
                invalid();
            text.remove(0, 1);
        }
    }
    return out;
}
ByteRange range(const QString& text, const QUrl& url, const Source* previous) {
    const auto parts = text.split('@');
    if (parts.size() > 2)
        invalid();
    ByteRange result;
    result.length = number(parts[0]);
    if (result.length < 1 || result.length > 32 * 1024 * 1024)
        invalid();
    if (parts.size() == 2)
        result.offset = number(parts[1]);
    else {
        if (!previous || previous->url != url || !previous->range)
            invalid();
        result.offset = previous->range->offset + previous->range->length;
    }
    if (result.offset > std::numeric_limits<std::int64_t>::max() - result.length - 4096)
        invalid();
    return result;
}
QByteArray iv(const QString& text) {
    if (!text.startsWith("0x") || text.size() <= 2 || text.size() > 34)
        invalid();
    auto digits = text.mid(2).toLatin1();
    if (!std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }))
        invalid();
    return QByteArray::fromHex(digits.rightJustified(32, '0'));
}
} // namespace
bool sameOrigin(const QUrl& a, const QUrl& b) {
    return a.isValid() && b.isValid() && b.userInfo().isEmpty() && b.fragment().isEmpty() &&
           (b.scheme() == "http" || b.scheme() == "https") && !b.host().isEmpty() &&
           a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0 &&
           a.host().compare(b.host(), Qt::CaseInsensitive) == 0 &&
           a.port(a.scheme() == "https" ? 443 : 80) == b.port(b.scheme() == "https" ? 443 : 80);
}
QUrl resolve(const QUrl& origin, const QUrl& base, const QString& reference,
             const std::vector<MediaOriginAccess>& approved) {
    if (reference.isEmpty() || reference.size() > 32768 ||
        std::any_of(reference.begin(), reference.end(), [](QChar c) { return c.unicode() < 32; }))
        invalid();
    const auto url = base.resolved(QUrl(reference, QUrl::StrictMode));
    const auto target = mediaOrigin(url.toEncoded().toStdString());
    if (!url.isValid() || !url.userInfo().isEmpty() || !url.fragment().isEmpty() || target.empty() ||
        (!sameOrigin(origin, url) && std::none_of(approved.begin(), approved.end(), [&](const auto& rule) {
            return mediaOrigin(rule.origin, true) == target;
        })))
        throw std::runtime_error("Live recording refused an unapproved source origin or unsafe URL.");
    return url;
}
Playlist parsePlaylist(const QByteArray& bytes, const QUrl& origin, const QUrl& base,
                       const std::vector<MediaOriginAccess>& approved) {
    if (bytes.size() > 1024 * 1024 || bytes.contains('\0') || QString::fromUtf8(bytes).toUtf8() != bytes)
        invalid();
    const auto lines = QString::fromUtf8(bytes).split('\n');
    if (lines.isEmpty() || lines.front().trimmed() != "#EXTM3U")
        invalid();
    Playlist result;
    bool pendingVariant = false, sequenceSeen = false, targetSeen = false, master = false;
    bool discontinuitySeen = false;
    double duration = 0;
    std::int64_t bandwidth = -1, best = -1, discontinuity = 0;
    QString pendingRange, audioGroup, selectedAudio;
    Encryption encryption;
    std::shared_ptr<const Source> initialization;
    std::optional<std::int64_t> programTime;
    bool anchorPending = false;
    struct Audio {
        QString group;
        QUrl url;
        bool preferred;
    };
    std::vector<Audio> audio;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const auto line = lines[i].trimmed();
        if (line.isEmpty())
            continue;
        if (line.size() > 32768)
            invalid();
        if (!line.startsWith('#')) {
            const auto url = resolve(origin, base, line, approved);
            if (pendingVariant) {
                if (bandwidth > best) {
                    result.variant = url;
                    selectedAudio = audioGroup;
                    best = bandwidth;
                }
                pendingVariant = false;
            } else {
                if (duration == 0 || master || result.ended || result.media.size() >= 4096)
                    invalid();
                MediaSegment segment;
                segment.source = {url, {}, encryption};
                if (!pendingRange.isEmpty()) {
                    segment.source.range = range(
                        pendingRange, url, result.media.empty() ? nullptr : &result.media.back().source);
                    pendingRange.clear();
                }
                segment.initialization = initialization;
                segment.duration = duration;
                segment.discontinuity = discontinuity;
                segment.programTimeMs = programTime;
                if (programTime)
                    *programTime += static_cast<std::int64_t>(std::llround(duration * 1000));
                anchorPending = false;
                result.media.push_back(segment);
                result.segments.push_back(url);
                duration = 0;
            }
            continue;
        }
        if (pendingVariant || result.ended)
            invalid();
        if (line.startsWith("#EXT-X-STREAM-INF:")) {
            if (!result.media.empty() || duration)
                invalid();
            const auto a = attributes(line.mid(18));
            if (a.contains("AUDIO") && a.value("AUDIO").isEmpty())
                invalid();
            if (a.contains("VIDEO") || a.contains("SUBTITLES") ||
                (a.contains("CLOSED-CAPTIONS") && a["CLOSED-CAPTIONS"] != "NONE"))
                invalid();
            bandwidth = number(a.value("BANDWIDTH"));
            audioGroup = a.value("AUDIO");
            pendingVariant = master = true;
        } else if (line.startsWith("#EXT-X-MEDIA:")) {
            if (!result.media.empty() || duration)
                invalid();
            const auto a = attributes(line.mid(13));
            if (a.value("TYPE") != "AUDIO" || a.value("GROUP-ID").isEmpty() || a.value("NAME").isEmpty() ||
                !a.contains("URI") || audio.size() >= 32)
                invalid();
            audio.push_back(
                {a["GROUP-ID"], resolve(origin, base, a["URI"], approved), a.value("DEFAULT") == "YES"});
            master = true;
        } else if (line.startsWith("#EXTINF:")) {
            bool ok = false;
            const auto n = line.mid(8).section(',', 0, 0).toDouble(&ok);
            if (!ok || !std::isfinite(n) || n <= 0 || n > 3600 || duration || master)
                invalid();
            duration = n;
        } else if (line.startsWith("#EXT-X-MEDIA-SEQUENCE:")) {
            if (sequenceSeen || !result.media.empty() || master)
                invalid();
            result.sequence = number(line.mid(22));
            sequenceSeen = true;
        } else if (line.startsWith("#EXT-X-TARGETDURATION:")) {
            if (targetSeen || master)
                invalid();
            const auto n = number(line.mid(22));
            if (n < 1 || n > 3600)
                invalid();
            result.targetSeconds = static_cast<int>(n);
            targetSeen = true;
        } else if (line == "#EXT-X-ENDLIST") {
            if (master || duration)
                invalid();
            result.ended = true;
        } else if (line.startsWith("#EXT-X-KEY:")) {
            if (duration || master)
                invalid();
            const auto a = attributes(line.mid(11));
            if (a.value("METHOD") == "NONE") {
                if (a.size() != 1)
                    invalid();
                encryption = {};
            } else {
                if (a.value("METHOD") != "AES-128" || !a.contains("URI") ||
                    (a.contains("KEYFORMAT") && a["KEYFORMAT"] != "identity") ||
                    (a.contains("KEYFORMATVERSIONS") && a["KEYFORMATVERSIONS"] != "1"))
                    invalid();
                encryption.key = resolve(origin, base, a["URI"], approved);
                encryption.iv = a.contains("IV") ? iv(a["IV"]) : QByteArray{};
            }
        } else if (line.startsWith("#EXT-X-MAP:")) {
            if (duration || master)
                invalid();
            const auto a = attributes(line.mid(11));
            auto map = std::make_shared<Source>();
            map->url = resolve(origin, base, a.value("URI"), approved);
            map->encryption = encryption;
            if (!encryption.key.isEmpty() && encryption.iv.isEmpty())
                invalid();
            if (a.contains("BYTERANGE"))
                map->range = range(a["BYTERANGE"], map->url, initialization.get());
            initialization = map;
        } else if (line.startsWith("#EXT-X-BYTERANGE:")) {
            if (master || !pendingRange.isEmpty())
                invalid();
            pendingRange = line.mid(17);
        } else if (line.startsWith("#EXT-X-DISCONTINUITY-SEQUENCE:")) {
            if (master || discontinuitySeen || !result.media.empty() || duration)
                invalid();
            discontinuity = number(line.mid(30));
            discontinuitySeen = true;
        } else if (line == "#EXT-X-DISCONTINUITY") {
            if (master || duration || discontinuity >= std::numeric_limits<std::int64_t>::max() - 4096)
                invalid();
            ++discontinuity;
            programTime.reset();
        } else if (line.startsWith("#EXT-X-PROGRAM-DATE-TIME:")) {
            const auto text = line.mid(25);
            if (master || anchorPending || text.size() > 40 ||
                !QRegularExpression("^[0-9]{4}-").match(text).hasMatch() ||
                !QRegularExpression("(Z|[+-][0-9]{2}:[0-9]{2})$").match(text).hasMatch())
                invalid();
            auto time = QDateTime::fromString(text, Qt::ISODateWithMs);
            if (!time.isValid())
                time = QDateTime::fromString(text, Qt::ISODate);
            if (!time.isValid())
                invalid();
            programTime = time.toMSecsSinceEpoch();
            anchorPending = true;
        } else if (line.startsWith("#EXT-X-PART:") || line.startsWith("#EXT-X-PRELOAD-HINT:") ||
                   line.startsWith("#EXT-X-SERVER-CONTROL:") || line.startsWith("#EXT-X-PART-INF:") ||
                   line.startsWith("#EXT-X-RENDITION-REPORT:")) {
            if (master || duration)
                invalid();
            attributes(
                line.mid(line.indexOf(':') + 1)); // Ignore hints; only complete segments are downloaded.
        } else if (line.startsWith("#EXT") && !line.startsWith("#EXT-X-VERSION:") &&
                   !line.startsWith("#EXT-X-PLAYLIST-TYPE:") && line != "#EXT-X-INDEPENDENT-SEGMENTS")
            invalid();
    }
    if (duration || pendingVariant || !pendingRange.isEmpty() ||
        (!master && (!targetSeen || result.media.empty())) || (master && result.variant.isEmpty()))
        invalid();
    if (!selectedAudio.isEmpty()) {
        std::vector<Audio> matches;
        for (const auto& entry : audio)
            if (entry.group == selectedAudio)
                matches.push_back(entry);
        const auto count =
            std::count_if(matches.begin(), matches.end(), [](const auto& entry) { return entry.preferred; });
        if (matches.empty() || count > 1 || (matches.size() > 1 && count != 1))
            invalid();
        result.audio = count ? std::find_if(matches.begin(), matches.end(),
                                            [](const auto& entry) { return entry.preferred; })
                                   ->url
                             : matches.front().url;
    }
    return result;
}
} // namespace maskpilot::live
