#pragma once
#include "live_playlist.h"
#include <QTemporaryDir>
#include <filesystem>
#include <maskpilot/infrastructure/adapters.h>

namespace maskpilot::live {
QByteArray decrypt(const QByteArray&, const QByteArray& key, QByteArray iv, std::int64_t sequence);
void requireMuxer(const std::filesystem::path&);
// Owns only generated local names and complete, already decrypted media.
class Assembly {
    struct Entry {
        QString file, initialization;
        double duration;
        std::int64_t discontinuity;
    };
    QTemporaryDir directory_;
    std::vector<Entry> tracks_[2];
    bool mux_ = false;
    int serial_ = 0;
    QString save(const QByteArray&, const QString& extension);
    QString playlist(int track);

  public:
    explicit Assembly(const std::filesystem::path& work);
    void append(int track, const MediaSegment&, const QByteArray&, const QByteArray& initialization);
    bool needsMux() const { return mux_; }
    bool full() const { return tracks_[0].size() >= 8192 || tracks_[1].size() >= 8192; }
    std::filesystem::path mux(const DownloadSpec&, TransferControl&);
};
} // namespace maskpilot::live
