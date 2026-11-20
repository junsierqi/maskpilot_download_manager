#include "live_playlist.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace maskpilot::live {
namespace {
[[noreturn]] void invalid() {
    throw std::runtime_error("Unsupported or malformed live HLS playlist. Use clear MPEG-TS HLS without "
                             "encryption, separate audio, discontinuities or partial/byte-range segments.");
}
std::int64_t number(const QString& text) {
    bool ok = false;
    auto n = text.toLongLong(&ok);
    if (!ok || n < 0 || n > std::numeric_limits<std::int64_t>::max() - 4096)
        invalid();
    return n;
}
} // namespace
bool sameOrigin(const QUrl& a, const QUrl& b) {
    return a.isValid() && b.isValid() && b.userInfo().isEmpty() && b.fragment().isEmpty() &&
           (b.scheme() == "http" || b.scheme() == "https") && !b.host().isEmpty() &&
           a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0 &&
           a.host().compare(b.host(), Qt::CaseInsensitive) == 0 &&
           a.port(a.scheme() == "https" ? 443 : 80) == b.port(b.scheme() == "https" ? 443 : 80);
}
QUrl resolve(const QUrl& origin, const QUrl& base, const QString& reference) {
    if (reference.isEmpty() || reference.size() > 32768 ||
        std::any_of(reference.begin(), reference.end(), [](QChar c) { return c.unicode() < 32; }))
        invalid();
    auto url = base.resolved(QUrl(reference, QUrl::StrictMode));
    if (!sameOrigin(origin, url))
        throw std::runtime_error("Live recording refused a different source origin or unsafe URL.");
    return url;
}
Playlist parsePlaylist(const QByteArray& bytes, const QUrl& origin, const QUrl& base) {
    if (bytes.size() > 1024 * 1024 || bytes.contains('\0') || QString::fromUtf8(bytes).toUtf8() != bytes)
        invalid();
    auto lines = QString::fromUtf8(bytes).split('\n');
    if (lines.isEmpty() || lines.front().trimmed() != "#EXTM3U")
        invalid();
    Playlist result;
    bool duration = false, pendingVariant = false, sequenceSeen = false, targetSeen = false;
    bool master = false, media = false;
    std::int64_t bandwidth = -1, best = -1;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        auto line = lines[i].trimmed();
        if (line.isEmpty())
            continue;
        if (line.size() > 32768)
            invalid();
        if (!line.startsWith('#')) {
            auto url = resolve(origin, base, line);
            if (pendingVariant) {
                if (bandwidth > best) {
                    result.variant = url;
                    best = bandwidth;
                }
                pendingVariant = false;
            } else {
                if (!duration || master || result.ended || result.segments.size() >= 4096)
                    invalid();
                result.segments.push_back(url);
                duration = false;
                media = true;
            }
            continue;
        }
        if (pendingVariant || (duration && line.startsWith("#EXT")))
            invalid();
        if (line.startsWith("#EXT-X-STREAM-INF:")) {
            if (media || line.contains("AUDIO=") || line.contains("VIDEO=") || line.contains("SUBTITLES="))
                invalid();
            const auto match = QRegularExpression("(?:^|,)BANDWIDTH=([0-9]+)(?:,|$)").match(line.mid(18));
            if (!match.hasMatch())
                invalid();
            bandwidth = number(match.captured(1));
            pendingVariant = master = true;
        } else if (line.startsWith("#EXTINF:")) {
            bool ok = false;
            const auto n = line.mid(8).section(',', 0, 0).toDouble(&ok);
            if (!ok || !std::isfinite(n) || n <= 0 || n > 3600 || master || result.ended)
                invalid();
            duration = true;
        } else if (line.startsWith("#EXT-X-MEDIA-SEQUENCE:")) {
            if (sequenceSeen || media || master)
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
            if (result.ended || master)
                invalid();
            result.ended = true;
        } else if (line.startsWith("#EXT-X-KEY:")) {
            if (line != "#EXT-X-KEY:METHOD=NONE")
                invalid();
        } else if (line.startsWith("#EXT") && !line.startsWith("#EXT-X-VERSION:") &&
                   !line.startsWith("#EXT-X-PROGRAM-DATE-TIME:") &&
                   !line.startsWith("#EXT-X-PLAYLIST-TYPE:") && line != "#EXT-X-INDEPENDENT-SEGMENTS") {
            // Refuse unfamiliar extensions rather than misinterpreting encryption, gaps or init data.
            invalid();
        }
    }
    if (duration || pendingVariant || (!master && (!targetSeen || result.segments.empty())))
        invalid();
    return result;
}
} // namespace maskpilot::live
