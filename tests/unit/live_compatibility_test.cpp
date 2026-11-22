#include "../../src/infrastructure/src/media/live_assembly.h"
#include "../../src/infrastructure/src/media/live_playlist.h"
#include <gtest/gtest.h>

using namespace maskpilot;
namespace {
const QUrl origin("https://source.example/live/index.m3u8");
const QByteArray base = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:42\n";
live::Playlist parse(const QByteArray& text) { return live::parsePlaylist(text, origin, origin); }
} // namespace
TEST(LiveCompatibility, ParsesRangesKeysMapsAndDiscontinuityWithoutLosingSequence) {
    auto value =
        parse(base + "#EXT-X-KEY:METHOD=AES-128,URI=\"key\",IV=0x2a\n"
                     "#EXT-X-MAP:URI=\"media.bin\",BYTERANGE=\"16@0\"\n"
                     "#EXTINF:2,\n#EXT-X-BYTERANGE:188@16\nmedia.bin\n"
                     "#EXT-X-DISCONTINUITY\n#EXTINF:2,\n#EXT-X-BYTERANGE:188\nmedia.bin\n#EXT-X-ENDLIST\n");
    ASSERT_EQ(value.media.size(), 2);
    EXPECT_EQ(value.media[0].source.range->offset, 16);
    EXPECT_EQ(value.media[1].source.range->offset, 204);
    EXPECT_EQ(value.media[1].discontinuity, 1);
    EXPECT_EQ(value.media[0].source.encryption.iv, QByteArray::fromHex("0000000000000000000000000000002a"));
    ASSERT_NE(value.media[0].initialization, nullptr);
    EXPECT_EQ(value.media[0].initialization->range->length, 16);
    EXPECT_EQ(value.media[0].source.encryption.key, QUrl("https://source.example/live/key"));
}
TEST(LiveCompatibility, RejectsAmbiguousRangesDrmKeysAndDeltas) {
    for (const auto* tag :
         {"#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"key\"",
          "#EXT-X-KEY:METHOD=AES-128,URI=\"key\",KEYFORMAT=\"drm\"",
          "#EXT-X-KEY:METHOD=AES-128,URI=\"key\",IV=0xz",
          "#EXT-X-KEY:METHOD=AES-128,URI=\"key\",URI=\"other\"",
          "#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n#EXT-X-MAP:URI=\"init.mp4\"", "#EXT-X-BYTERANGE:188",
          "#EXT-X-BYTERANGE:0@0", "#EXT-X-BYTERANGE:188@9223372036854775000",
          "#EXT-X-SKIP:SKIPPED-SEGMENTS=2", "#EXT-X-GAP"})
        EXPECT_THROW(parse(base + tag + "\n#EXTINF:2,\na.ts\n"), std::exception) << tag;
    EXPECT_THROW(parse(base + "#EXTINF:2,\n#EXT-X-BYTERANGE:188@0\na.ts\n"
                              "#EXTINF:2,\n#EXT-X-BYTERANGE:188\nb.ts\n"),
                 std::exception);
}
TEST(LiveCompatibility, SelectsOneDefaultAudioAndIgnoresOnlyPartialHints) {
    auto master = parse(
        "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"audio\",DEFAULT=YES,URI=\"audio.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO=\"a\"\nvideo.m3u8\n");
    EXPECT_EQ(master.audio, QUrl("https://source.example/live/audio.m3u8"));
    EXPECT_THROW(parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO=\"missing\"\nvideo.m3u8\n"),
                 std::exception);
    const auto ll = parse(
        base + "#EXT-X-PART-INF:PART-TARGET=0.5\n"
               "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES\n#EXT-X-PART:DURATION=0.5,URI=\"partial.ts\"\n"
               "#EXTINF:2,\na.ts\n#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"future.ts\"\n");
    EXPECT_EQ(ll.media.size(), 1);
    EXPECT_THROW(parse(base + "#EXT-X-PART:DURATION=0.5,URI=\"partial.ts\"\n"), std::exception);
}
TEST(LiveCompatibility, ApprovesExactForeignOriginsAndLivePolicyWithoutCredentialInheritance) {
    DownloadSpec spec;
    spec.kind = TransferKind::Live;
    spec.url = origin.toString().toStdString();
    spec.mediaOrigins = {{"https://cdn.example:443", "private", {}, {}, true}};
    spec.mediaOriginsConfirmed = true;
    EXPECT_TRUE(validateMediaOrigins(spec, true).empty());
    const auto value = live::parsePlaylist(base + "#EXTINF:2,\nhttps://cdn.example/a.ts\n", origin, origin,
                                           spec.mediaOrigins);
    EXPECT_EQ(value.media.size(), 1);
    EXPECT_THROW(live::parsePlaylist(base + "#EXTINF:2,\nhttps://cdn.example:444/a.ts\n", origin, origin,
                                     spec.mediaOrigins),
                 std::exception);
    invalidateMediaOrigins(spec);
    EXPECT_FALSE(validateMediaOrigins(spec, true).empty());
}
TEST(LiveCompatibility, DecryptsIndependentKnownCiphertextWithStrictPaddingAndSequenceIv) {
    const auto key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    auto cipher = QByteArray::fromHex("75d6d2bdaef64aa82631e537a7a0dc18b355f86b8f9d0032f754ecd0aacff500");
    EXPECT_EQ(live::decrypt(cipher, key, {}, 42), "owned-live-plaintext");
    EXPECT_EQ(live::decrypt(cipher, key, QByteArray::fromHex("0000000000000000000000000000002a"), 7),
              "owned-live-plaintext");
    cipher.chop(1);
    EXPECT_THROW(live::decrypt(cipher, key, {}, 42), std::exception);
    cipher.append('\x01');
    EXPECT_THROW(live::decrypt(cipher, key, {}, 42), std::exception);
    EXPECT_THROW(live::decrypt(cipher, key.left(15), {}, 42), std::exception);
}
TEST(LiveCompatibility, OwnedAssemblyRejectsDisguisedPlaylistsAndMalformedContainers) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto work = std::filesystem::path(temporary.path().toStdU16String());
    live::Assembly assembly(work);
    live::MediaSegment segment;
    segment.duration = 1;
    segment.initialization = std::make_shared<live::Source>();
    EXPECT_THROW(assembly.append(0, segment, "#EXTM3U\nfile:///private/data\n", "not-an-init"),
                 std::exception);
    EXPECT_THROW(assembly.append(0, segment, QByteArray::fromHex("000000086d6f6f66"),
                                 QByteArray::fromHex("000000086d6f6f76")),
                 std::exception);
    segment.initialization.reset();
    EXPECT_THROW(assembly.append(1, segment, "#EXTM3U\nhttps://foreign.invalid\n", {}), std::exception);
    EXPECT_THROW(assembly.append(1, segment, QByteArray::fromHex("fff10000000000"), {}), std::exception);
}
TEST(LiveCompatibility, ProgramClockPropagatesAndRequiresNewAnchorAtDiscontinuity) {
    const auto value = parse(base + "#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00.000Z\n#EXTINF:1,\na.ts\n"
                                    "#EXTINF:1,\nb.ts\n#EXT-X-DISCONTINUITY\n#EXTINF:1,\nc.ts\n");
    ASSERT_EQ(value.media.size(), 3);
    ASSERT_TRUE(value.media[0].programTimeMs.has_value());
    ASSERT_TRUE(value.media[1].programTimeMs.has_value());
    EXPECT_EQ(*value.media[1].programTimeMs - *value.media[0].programTimeMs, 1000);
    EXPECT_FALSE(value.media[2].programTimeMs.has_value());
    for (const auto* time : {"2026-01-01T00:00:00", "invalid", "2026-99-01T00:00:00Z"})
        EXPECT_THROW(parse(base + "#EXT-X-PROGRAM-DATE-TIME:" + time + "\n#EXTINF:1,\na.ts\n"),
                     std::exception);
}
