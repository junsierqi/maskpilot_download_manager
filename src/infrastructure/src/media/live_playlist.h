#pragma once
#include <QByteArray>
#include <QUrl>
#include <cstdint>
#include <vector>

namespace maskpilot::live {
struct Playlist {
    QUrl variant;
    std::vector<QUrl> segments;
    std::int64_t sequence = 0;
    int targetSeconds = 0;
    bool ended = false;
};
bool sameOrigin(const QUrl&, const QUrl&);
QUrl resolve(const QUrl& origin, const QUrl& base, const QString& reference);
Playlist parsePlaylist(const QByteArray&, const QUrl& origin, const QUrl& base);
} // namespace maskpilot::live
