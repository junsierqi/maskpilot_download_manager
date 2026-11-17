#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace maskpilot {
inline constexpr std::size_t maxTorrentMetadataBytes = 16 * 1024 * 1024;
inline constexpr std::size_t maxTorrentFiles = 20000;
using TorrentMetadata = std::shared_ptr<const std::vector<char>>;
struct TorrentOptions {
    TorrentMetadata metadata;
    bool allFiles = true;
    std::vector<int> selectedFiles;
    bool seedAfterDownload = false;
    std::int64_t uploadLimit = 0;
};
struct TorrentFile {
    int index = 0;
    std::string path;
    std::int64_t size = 0;
    bool padding = false;
};
struct TransferDescription {
    std::string name, infoHash;
    std::shared_ptr<const std::vector<TorrentFile>> files;
};
std::string validateTorrentPath(const std::string& relativePath);
bool torrentFileWanted(const TorrentOptions& options, const TorrentFile& file);
} // namespace maskpilot
