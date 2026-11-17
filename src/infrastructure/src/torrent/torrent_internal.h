#pragma once
#include <libtorrent/add_torrent_params.hpp>
#include <maskpilot/infrastructure/torrent_engine.h>

namespace maskpilot::torrent_internal {
struct ParsedTorrent {
    libtorrent::add_torrent_params params;
    TransferDescription description;
    TorrentMetadata metadata;
};
ParsedTorrent parse(const DownloadSpec& spec);
TorrentMetadata readMetadata(const DownloadSpec& spec);
TransferDescription describe(const libtorrent::torrent_info& info, const std::string& identity = {});
TorrentMetadata metadataFrom(const libtorrent::add_torrent_params& params);
bool hashesMatch(const libtorrent::info_hash_t& expected, const libtorrent::info_hash_t& actual);
std::string hashIdentity(const libtorrent::info_hash_t& hashes);
std::filesystem::path nativePath(const std::string& text);
std::string pathText(const std::filesystem::path& path);
} // namespace maskpilot::torrent_internal
