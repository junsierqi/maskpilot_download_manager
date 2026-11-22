#pragma once
#include <QByteArray>
#include <QUrl>
#include <cstdint>
#include <maskpilot/domain/media_origins.h>
#include <memory>
#include <optional>
#include <vector>

namespace maskpilot::live {
struct ByteRange {
    std::int64_t offset = 0, length = 0;
    bool operator==(const ByteRange&) const = default;
};
struct Encryption {
    QUrl key;
    QByteArray iv;
};
struct Source {
    QUrl url;
    std::optional<ByteRange> range;
    Encryption encryption;
};
struct MediaSegment {
    Source source;
    std::shared_ptr<const Source> initialization;
    double duration = 0;
    std::int64_t discontinuity = 0;
    std::optional<std::int64_t> programTimeMs;
};
struct Playlist {
    QUrl variant;
    QUrl audio;
    std::vector<QUrl> segments;
    std::vector<MediaSegment> media;
    std::int64_t sequence = 0;
    int targetSeconds = 0;
    bool ended = false;
};
bool sameOrigin(const QUrl&, const QUrl&);
QUrl resolve(const QUrl& origin, const QUrl& base, const QString& reference,
             const std::vector<MediaOriginAccess>& approved = {});
Playlist parsePlaylist(const QByteArray&, const QUrl& origin, const QUrl& base,
                       const std::vector<MediaOriginAccess>& approved = {});
} // namespace maskpilot::live
