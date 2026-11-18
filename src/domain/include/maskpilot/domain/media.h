#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace maskpilot {
enum class MediaSelectionMode { Automatic, Muxed, VideoAudio, AudioOnly, VideoOnly };
enum class MediaContainer { Automatic, Mkv, Mp4, Webm, M4a, Mp3, Opus, Flac };
struct MediaSelection {
    MediaSelectionMode mode = MediaSelectionMode::Automatic;
    std::string primaryFormat, audioFormat;
    MediaContainer container = MediaContainer::Automatic;
};
struct MediaFormat {
    std::string id, extension, videoCodec, audioCodec, protocol;
    int width = 0, height = 0;
    double fps = 0, bitrate = 0;
    std::int64_t bytes = -1;
    bool estimated = false, hasVideo = false, hasAudio = false;
};
struct MediaInventory {
    std::string title;
    std::vector<MediaFormat> formats;
};
bool validMediaFormatId(const std::string&);
bool mediaAudioOnly(const MediaSelection&, bool legacyAudioOnly = false);
std::string mediaContainerExtension(MediaContainer, bool audioOnly = false);
std::string selectedMediaFormat(const MediaSelection&);
std::string validateMediaSelection(const MediaSelection&, bool legacyAudioOnly = false);
std::string validateMediaInventory(const MediaInventory&);
std::string validateMediaChoice(const MediaSelection&, const MediaInventory&, bool legacyAudioOnly = false);
} // namespace maskpilot
