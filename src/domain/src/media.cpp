#include <algorithm>
#include <cmath>
#include <maskpilot/domain/media.h>
#include <set>

namespace maskpilot {
namespace {
bool label(const std::string& value, std::size_t maximum) {
    return value.size() <= maximum && std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.';
           });
}
bool prefix(const std::string& codec, std::initializer_list<const char*> values) {
    return std::any_of(values.begin(), values.end(),
                       [&](const auto* value) { return codec.starts_with(value); });
}
bool compatible(const MediaFormat& format, MediaContainer container) {
    if (container == MediaContainer::Mp4)
        return (!format.hasVideo ||
                prefix(format.videoCodec, {"avc", "h264", "hevc", "h265", "hev1", "hvc1", "av01", "av1"})) &&
               (!format.hasAudio || prefix(format.audioCodec, {"aac", "mp4a", "mp3", "alac"}));
    if (container == MediaContainer::Webm)
        return (!format.hasVideo ||
                prefix(format.videoCodec, {"vp8", "vp9", "vp08", "vp09", "av01", "av1"})) &&
               (!format.hasAudio || prefix(format.audioCodec, {"opus", "vorbis"}));
    return true;
}
} // namespace
bool validMediaFormatId(const std::string& id) { return !id.empty() && label(id, 128); }
bool mediaAudioOnly(const MediaSelection& selection, bool legacy) {
    return selection.mode == MediaSelectionMode::Automatic ? legacy
                                                           : selection.mode == MediaSelectionMode::AudioOnly;
}
std::string mediaContainerExtension(MediaContainer container, bool audio) {
    switch (container) {
    case MediaContainer::Automatic:
        return audio ? "m4a" : "mkv";
    case MediaContainer::Mkv:
        return "mkv";
    case MediaContainer::Mp4:
        return "mp4";
    case MediaContainer::Webm:
        return "webm";
    case MediaContainer::M4a:
        return "m4a";
    case MediaContainer::Mp3:
        return "mp3";
    case MediaContainer::Opus:
        return "opus";
    case MediaContainer::Flac:
        return "flac";
    }
    return {};
}
std::string selectedMediaFormat(const MediaSelection& selection) {
    if (!validateMediaSelection(selection).empty())
        return {};
    if (selection.mode == MediaSelectionMode::Automatic)
        return {};
    const auto exactFormat = [](const std::string& id) { return "best*[format_id='" + id + "']"; };
    return selection.mode == MediaSelectionMode::VideoAudio
               ? exactFormat(selection.primaryFormat) + "+" + exactFormat(selection.audioFormat)
               : exactFormat(selection.primaryFormat);
}
std::string validateMediaSelection(const MediaSelection& selection, bool legacy) {
    if (selection.mode < MediaSelectionMode::Automatic || selection.mode > MediaSelectionMode::VideoOnly ||
        selection.container < MediaContainer::Automatic || selection.container > MediaContainer::Flac)
        return "Invalid media selection mode or container.";
    if (selection.mode == MediaSelectionMode::Automatic) {
        if (!selection.primaryFormat.empty() || !selection.audioFormat.empty())
            return "Automatic selection cannot retain explicit format IDs.";
    } else if (!validMediaFormatId(selection.primaryFormat))
        return "Select a valid current media format.";
    if (selection.mode == MediaSelectionMode::VideoAudio) {
        if (!validMediaFormatId(selection.audioFormat) || selection.audioFormat == selection.primaryFormat)
            return "Select distinct video and audio formats.";
    } else if (!selection.audioFormat.empty())
        return "An additional audio format requires video and audio mode.";
    const auto audio = mediaAudioOnly(selection, legacy);
    if (selection.container != MediaContainer::Automatic &&
        (audio ? selection.container < MediaContainer::M4a : selection.container >= MediaContainer::M4a))
        return "Choose a container appropriate for the selected audio or video mode.";
    return {};
}
std::string validateMediaInventory(const MediaInventory& inventory) {
    if (inventory.title.size() > 256 || inventory.title.find("://") != std::string::npos ||
        std::any_of(inventory.title.begin(), inventory.title.end(),
                    [](unsigned char c) { return c < 0x20 || c == 0x7f; }) ||
        inventory.formats.empty() || inventory.formats.size() > 2000)
        return "Media inventory is empty, oversized or contains unsafe metadata.";
    std::set<std::string> ids;
    for (const auto& format : inventory.formats) {
        if (!validMediaFormatId(format.id) || !ids.insert(format.id).second || !label(format.extension, 32) ||
            !label(format.videoCodec, 128) || !label(format.audioCodec, 128) ||
            (format.protocol != "http" && format.protocol != "https" && format.protocol != "m3u8" &&
             format.protocol != "m3u8_native" && format.protocol != "http_dash_segments") ||
            (!format.hasVideo && !format.hasAudio) || format.width < 0 || format.width > 65536 ||
            format.height < 0 || format.height > 65536 || !std::isfinite(format.fps) || format.fps < 0 ||
            format.fps > 1000 || !std::isfinite(format.bitrate) || format.bitrate < 0 ||
            format.bitrate > 1e9 || format.bytes < -1)
            return "Media inventory contains ambiguous or invalid format metadata.";
    }
    return {};
}
std::string validateMediaChoice(const MediaSelection& selection, const MediaInventory& inventory,
                                bool legacy) {
    if (auto error = validateMediaSelection(selection, legacy); !error.empty())
        return error;
    if (auto error = validateMediaInventory(inventory); !error.empty())
        return error;
    if (selection.mode == MediaSelectionMode::Automatic)
        return {};
    const auto find = [&](const std::string& id) {
        return std::find_if(inventory.formats.begin(), inventory.formats.end(),
                            [&](const auto& format) { return format.id == id; });
    };
    const auto primary = find(selection.primaryFormat);
    if (primary == inventory.formats.end())
        return "Selected media format is no longer available. Inspect the source again.";
    if ((selection.mode == MediaSelectionMode::Muxed && (!primary->hasVideo || !primary->hasAudio)) ||
        (selection.mode == MediaSelectionMode::AudioOnly && !primary->hasAudio) ||
        ((selection.mode == MediaSelectionMode::VideoOnly ||
          selection.mode == MediaSelectionMode::VideoAudio) &&
         (!primary->hasVideo || primary->hasAudio)))
        return "Selected format does not match the requested stream mode.";
    if (selection.mode == MediaSelectionMode::AudioOnly)
        return {}; // Local extraction/conversion handles audio output.
    if (!compatible(*primary, selection.container))
        return "Selected codecs are unknown or incompatible with the requested container. "
               "Choose the default/MKV container or a source that reports compatible codecs.";
    if (selection.mode == MediaSelectionMode::VideoAudio) {
        const auto audio = find(selection.audioFormat);
        if (audio == inventory.formats.end())
            return "Selected audio format is no longer available. Inspect the source again.";
        if (!audio->hasAudio || audio->hasVideo || !compatible(*audio, selection.container))
            return "Selected audio format is incompatible with the requested video/container.";
    }
    return {};
}
} // namespace maskpilot
