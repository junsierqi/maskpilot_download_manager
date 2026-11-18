#include <gtest/gtest.h>
#include <limits>
#include <maskpilot/domain/task.h>

namespace {
maskpilot::MediaFormat format(std::string id, bool video, bool audio, std::string vc = "avc1.640028",
                              std::string ac = "mp4a.40.2") {
    maskpilot::MediaFormat result;
    result.id = std::move(id);
    result.extension = "mp4";
    result.protocol = "https";
    result.hasVideo = video;
    result.hasAudio = audio;
    result.videoCodec = video ? vc : "none";
    result.audioCodec = audio ? ac : "none";
    return result;
}
maskpilot::MediaInventory inventory() {
    return {"Fixture",
            {format("mux", true, true), format("video", true, false), format("audio", false, true)}};
}
} // namespace
TEST(MediaSelection, TypedModesAreAuthoritativeAndNeverAcceptSelectorExpressions) {
    using namespace maskpilot;
    EXPECT_TRUE(mediaAudioOnly({}, true));
    MediaSelection chosen{MediaSelectionMode::VideoAudio, "video", "audio", MediaContainer::Mkv};
    EXPECT_FALSE(mediaAudioOnly(chosen, true));
    EXPECT_EQ(selectedMediaFormat(chosen), "best*[format_id='video']+best*[format_id='audio']");
    EXPECT_EQ(mediaContainerExtension(MediaContainer::Automatic, false), "mkv");
    EXPECT_EQ(mediaContainerExtension(MediaContainer::Automatic, true), "m4a");
    EXPECT_TRUE(validateMediaChoice(chosen, inventory(), true).empty());
    for (const auto* id : {"", "video+audio", "best/best", "[height<720]", "video,other", "url://private",
                           "id\nsecret", "id'quoted", "id\"quoted"})
        EXPECT_FALSE(validMediaFormatId(id));
    EXPECT_TRUE(validMediaFormatId("hls-720_audio.en"));
    EXPECT_FALSE(validMediaFormatId(std::string(129, 'a')));
    chosen.audioFormat = chosen.primaryFormat;
    EXPECT_FALSE(validateMediaSelection(chosen).empty());
    chosen = {static_cast<MediaSelectionMode>(99), "", "", MediaContainer::Automatic};
    EXPECT_FALSE(validateMediaSelection(chosen).empty());
}
TEST(MediaSelection, ExplicitFormatIdsCannotBecomeSelectorKeywords) {
    using namespace maskpilot;
    for (const auto* id : {"best", "all", "mp4", "webm", "137"}) {
        const MediaSelection chosen{MediaSelectionMode::VideoOnly, id, "", MediaContainer::Mkv};
        EXPECT_TRUE(validateMediaSelection(chosen).empty());
        EXPECT_EQ(selectedMediaFormat(chosen), "best*[format_id='" + std::string(id) + "']");
    }
    const MediaSelection pair{MediaSelectionMode::VideoAudio, "137", "all", MediaContainer::Mkv};
    EXPECT_EQ(selectedMediaFormat(pair), "best*[format_id='137']+best*[format_id='all']");
}
TEST(MediaSelection, FreshInventoryRejectsDisappearedStreamsAndIncompatibleContainerWithoutFallback) {
    using namespace maskpilot;
    auto data = inventory();
    EXPECT_TRUE(
        validateMediaChoice({MediaSelectionMode::Muxed, "mux", "", MediaContainer::Mp4}, data).empty());
    EXPECT_FALSE(
        validateMediaChoice({MediaSelectionMode::Muxed, "video", "", MediaContainer::Mkv}, data).empty());
    EXPECT_TRUE(
        validateMediaChoice({MediaSelectionMode::AudioOnly, "mux", "", MediaContainer::Mp3}, data).empty());
    EXPECT_FALSE(
        validateMediaChoice({MediaSelectionMode::VideoOnly, "mux", "", MediaContainer::Mkv}, data).empty());
    EXPECT_FALSE(
        validateMediaChoice({MediaSelectionMode::VideoAudio, "video", "missing", MediaContainer::Mkv}, data)
            .empty());
    EXPECT_FALSE(
        validateMediaChoice({MediaSelectionMode::VideoAudio, "video", "audio", MediaContainer::Webm}, data)
            .empty());
    data.formats[1].videoCodec = "unknown";
    EXPECT_FALSE(
        validateMediaChoice({MediaSelectionMode::VideoOnly, "video", "", MediaContainer::Mp4}, data).empty());
    EXPECT_TRUE(
        validateMediaChoice({MediaSelectionMode::VideoOnly, "video", "", MediaContainer::Mkv}, data).empty());
    data.formats[1].videoCodec = "vp09.00.51.08";
    data.formats[2].audioCodec = "opus";
    EXPECT_TRUE(
        validateMediaChoice({MediaSelectionMode::VideoAudio, "video", "audio", MediaContainer::Webm}, data)
            .empty());
}
TEST(MediaSelection, InventoryRejectsDuplicatesUnsafeMetadataAndInvalidNumbers) {
    using namespace maskpilot;
    auto data = inventory();
    EXPECT_TRUE(validateMediaInventory(data).empty());
    data.formats.push_back(data.formats.front());
    EXPECT_FALSE(validateMediaInventory(data).empty());
    data = inventory();
    data.formats[0].fps = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(validateMediaInventory(data).empty());
    data = inventory();
    data.formats[0].bitrate = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(validateMediaInventory(data).empty());
    data = inventory();
    data.formats[0].protocol = "file";
    EXPECT_FALSE(validateMediaInventory(data).empty());
    data = inventory();
    data.title = "https://private.invalid/token";
    EXPECT_FALSE(validateMediaInventory(data).empty());
    data = inventory();
    data.formats[0].audioCodec = "https://private.invalid/token";
    EXPECT_FALSE(validateMediaInventory(data).empty());
    data = inventory();
    data.formats[0].bytes = -2;
    EXPECT_FALSE(validateMediaInventory(data).empty());
    DownloadSpec spec;
    spec.url = "https://example.test/media";
    spec.destination = "selected.mkv";
    spec.mediaSelection = {MediaSelectionMode::Muxed, "mux", "", MediaContainer::Mkv};
    EXPECT_FALSE(validateSpec(spec).empty());
    spec.kind = TransferKind::Media;
    EXPECT_TRUE(validateSpec(spec).empty());
}
