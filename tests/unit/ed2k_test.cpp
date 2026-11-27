#include "../../src/infrastructure/src/ed2k/integrity.h"
#include "../../src/infrastructure/src/ed2k/protocol.h"
#include <gtest/gtest.h>
#include <limits>
#include <maskpilot/domain/ed2k.h>
#include <stdexcept>

namespace {
using namespace maskpilot;
namespace wire = maskpilot::ed2k;
constexpr auto hash = "a448017aaf21d8525fc10ae87aa6729d";
std::string link(std::string name = "file.bin", std::string size = "3", std::string options = "") {
    return "ed2k://|file|" + name + "|" + size + "|" + hash + "|" + options + "/";
}
TEST(Ed2k, ParsesImmutableIdentityAndUnicodeName) {
    const auto parsed = parseEd2kFileLink(link("%E6%B5%8B%E8%AF%95%20a%2Bb.bin"));
    EXPECT_EQ(parsed.filename, "\xe6\xb5\x8b\xe8\xaf\x95 a+b.bin");
    EXPECT_EQ(parsed.hash, hash);
    EXPECT_EQ(parsed.size, 3);
    EXPECT_TRUE(isEd2kUrl("ED2K://|file|"));
    EXPECT_EQ(parseEd2kFileLink(link() + "|").filename, "file.bin");
}
TEST(Ed2k, RejectsUnsafeOrMalformedLinks) {
    for (const auto* name : {"..", "%2e%2e%2fa", "C%3Aevil", "a%5cb", "CON", "a%00b", "a%0ab", "a.", "a%20",
                             "%c0%af", "%ed%a0%80", "%f4%90%80%80", "a%", "%zz"})
        EXPECT_THROW(parseEd2kFileLink(link(name)), std::invalid_argument) << name;
    for (const auto* size : {"-1", "+1", "1x", "18446744073709551616", ""})
        EXPECT_THROW(parseEd2kFileLink(link("a", size)), std::invalid_argument) << size;
    EXPECT_THROW(parseEd2kFileLink(link() + "garbage"), std::invalid_argument);
    EXPECT_THROW(parseEd2kFileLink(link("raw space")), std::invalid_argument);
    EXPECT_THROW(parseEd2kFileLink("ed2k://|server|127.0.0.1|4661|/"), std::invalid_argument);
    EXPECT_THROW(parseEd2kFileLink(std::string(maxEd2kLinkBytes + 1, '|')), std::invalid_argument);
    EXPECT_THROW(parseEd2kFileLink(link("a", "3", std::string(10000, '|'))), std::invalid_argument);
}
TEST(Ed2k, PreservesOptionalProofAndSourceMaterial) {
    const auto parsed =
        parseEd2kFileLink(link("a", "3",
                               std::string("p=") + hash +
                                   "|h=ABCDEFGHIJKLMNOPQRSTUVWXYZ234567|s=https%3A%2F%2Fexample.test%2Fa|") +
                          "|sources,127.0.0.1:4662,peer.example:4663:128:" + hash + "|/");
    ASSERT_EQ(parsed.partHashes.size(), 1);
    EXPECT_EQ(parsed.partHashes[0], hash);
    EXPECT_EQ(parsed.aichRoot, "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567");
    ASSERT_EQ(parsed.webSeeds.size(), 1);
    EXPECT_EQ(parsed.webSeeds[0], "https://example.test/a");
    ASSERT_EQ(parsed.sources.size(), 2);
    EXPECT_EQ(parsed.sources[1].clientHash, hash);
    EXPECT_EQ(parsed.sources[1].cryptOptions, 128);
    EXPECT_EQ(parsed.sources[0].port, 4662);
}
TEST(Ed2k, RejectsSourceAndProofAmbiguity) {
    for (const auto* endpoint :
         {"127.0.0.1:0", "127.0.0.1:65536", "user@host:4662", "host:4662:128",
          "host:4662:0:a448017aaf21d8525fc10ae87aa6729d", "[::1]:4662", "-host:4662", "host:4662:256"})
        EXPECT_THROW(parseEd2kFileLink(link() + "|sources," + endpoint + "|/"), std::invalid_argument);
    EXPECT_THROW(parseEd2kFileLink(link("a", "3", std::string("p=") + hash + ':' + hash + '|')),
                 std::invalid_argument);
    EXPECT_THROW(parseEd2kFileLink(link("a", "3", "unknown=x|")), std::invalid_argument);
    EXPECT_THROW(parseEd2kFileLink(link("a", "3", "h=abcdefghijklmnopqrstuvwxyz234567|")),
                 std::invalid_argument);
    EXPECT_THROW(parseEd2kFileLink(link("a", "3", std::string("p=") + hash + "|p=" + hash + '|')),
                 std::invalid_argument);
    EXPECT_THROW(parseEd2kFileLink(link() + "|sources,host:4662"), std::invalid_argument);
}
TEST(Ed2k, ValidatesOptionsAndProtocolHashCountBounds) {
    EXPECT_TRUE(validateEd2kOptions({{{"server.example", 4661}}, {{"127.0.0.1", 4662}}}).empty());
    EXPECT_FALSE(validateEd2kOptions({{{"http://server", 4661}}, {}}).empty());
    EXPECT_FALSE(validateEd2kOptions({{{"server", 4661, 128, hash}}, {}}).empty());
    EXPECT_FALSE(validateEd2kOptions({{}, std::vector<Ed2kEndpoint>(65, {"host", 4662})}).empty());
    EXPECT_EQ(ed2kHashCount(0), 1);
    EXPECT_EQ(ed2kHashCount(ed2kPartBytes - 1), 1);
    EXPECT_EQ(ed2kHashCount(ed2kPartBytes), 2);
    EXPECT_EQ(ed2kHashCount(2 * ed2kPartBytes), 3);
    EXPECT_EQ(ed2kHashCount(maxEd2kFileBytes), 65535);
    EXPECT_THROW(ed2kHashCount(maxEd2kFileBytes + 1), std::invalid_argument);
}
TEST(Ed2k, FramesSurviveEveryFragmentBoundaryAndCoalescing) {
    const wire::Frame source{wire::classicProtocol, 0x46, {1, 2, 3, 4, 5}};
    const auto encoded = wire::encodeFrame(source);
    EXPECT_EQ(encoded, (wire::Bytes{0xe3, 6, 0, 0, 0, 0x46, 1, 2, 3, 4, 5}));
    for (std::size_t boundary = 0; boundary <= encoded.size(); ++boundary) {
        wire::FrameDecoder decoder;
        decoder.feed(std::span(encoded).first(boundary));
        if (boundary != encoded.size())
            EXPECT_FALSE(decoder.next());
        decoder.feed(std::span(encoded).subspan(boundary));
        const auto decoded = decoder.next();
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->payload, source.payload);
        EXPECT_EQ(decoded->opcode, source.opcode);
        EXPECT_EQ(decoder.pendingBytes(), 0);
    }
    wire::Bytes joined = encoded;
    joined.insert(joined.end(), encoded.begin(), encoded.end());
    wire::FrameDecoder decoder;
    decoder.feed(joined);
    ASSERT_TRUE(decoder.next());
    ASSERT_TRUE(decoder.next());
    EXPECT_FALSE(decoder.next());
}
TEST(Ed2k, RejectsMalformedFramingAndTruncatedPayloadReads) {
    for (const auto& bytes :
         {wire::Bytes{0x00}, wire::Bytes{0xe3, 0, 0, 0, 0}, wire::Bytes{0xc5, 1, 0, 32, 0}}) {
        wire::FrameDecoder decoder;
        EXPECT_THROW(decoder.feed(bytes), std::invalid_argument);
    }
    wire::FrameDecoder decoder;
    EXPECT_THROW(decoder.feed(wire::Bytes(wire::maxFrameBytes + 6)), std::invalid_argument);
    EXPECT_THROW(wire::encodeFrame({wire::classicProtocol, 1, wire::Bytes(wire::maxFrameBytes)}),
                 std::invalid_argument);
    const wire::Bytes shortData{1, 2, 3};
    wire::Reader reader(shortData);
    EXPECT_THROW(reader.u32(), std::invalid_argument);
    EXPECT_EQ(reader.remaining(), 3);
    EXPECT_THROW(reader.requireEnd(), std::invalid_argument);
}
TEST(Ed2k, ReadsUnsignedWireValuesWithoutAlignmentAssumptions) {
    wire::Bytes bytes{99};
    wire::append16(bytes, 0x1234);
    wire::append32(bytes, 0x89abcdef);
    wire::append64(bytes, 0xfedcba9876543210ULL);
    wire::Reader reader(std::span<const std::uint8_t>(bytes).subspan(1));
    EXPECT_EQ(reader.u16(), 0x1234);
    EXPECT_EQ(reader.u32(), 0x89abcdef);
    EXPECT_EQ(reader.u64(), 0xfedcba9876543210ULL);
    EXPECT_NO_THROW(reader.requireEnd());
}
TEST(Ed2k, MatchesIndependentStandardMd4Vectors) {
    for (const auto& [text, expected] : std::vector<std::pair<std::string, std::string>>{
             {"", "31d6cfe0d16ae931b73c59d7e0c089c0"},
             {"a", "bde52cb31de33e46245e05fbdbd6fb24"},
             {"abc", "a448017aaf21d8525fc10ae87aa6729d"},
             {"message digest", "d9130a8164549fe818874806e1c7014b"},
             {"abcdefghijklmnopqrstuvwxyz", "d79e1c308aa5bbcdeea8ed63df412da9"}}) {
        auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        EXPECT_EQ(wire::hashText(wire::hashPart(bytes)), expected);
        EXPECT_EQ(wire::hashText(wire::fileHash(text.size(), std::vector{wire::hashPart(bytes)})), expected);
    }
    EXPECT_EQ(wire::parseHash("A448017AAF21D8525FC10AE87AA6729D"), wire::parseHash(hash));
    EXPECT_THROW(wire::parseHash(std::string(32, 'g')), std::invalid_argument);
}
TEST(Ed2k, RequiresCanonicalEmptyTrailerForExactParts) {
    const auto empty = wire::hashPart({});
    const auto dataHash = wire::parseHash(hash);
    const std::vector<wire::Hash> canonical{dataHash, empty};
    EXPECT_NO_THROW(wire::fileHash(ed2kPartBytes, canonical));
    EXPECT_THROW(wire::fileHash(ed2kPartBytes, std::vector{dataHash}), std::invalid_argument);
    EXPECT_THROW(wire::fileHash(ed2kPartBytes, std::vector{dataHash, dataHash}), std::invalid_argument);
    EXPECT_THROW(wire::fileHash(0, std::vector{dataHash}), std::invalid_argument);
    EXPECT_THROW(wire::hashPart(wire::Bytes(ed2kPartBytes + 1)), std::invalid_argument);
}
TEST(Ed2k, MatchesIndependentPartBoundaryVectors) {
    // Golden values computed with the OS hash implementation, independent of Qt.
    const wire::Bytes zeroes(ed2kPartBytes);
    const auto full = wire::hashPart(zeroes);
    const auto empty = wire::hashPart({});
    const auto one = wire::hashPart(std::span(zeroes).first(1));
    EXPECT_EQ(wire::hashText(full), "d7def262a127cd79096a108e7a9fc138");
    EXPECT_EQ(wire::hashText(wire::hashPart(std::span(zeroes).first(ed2kPartBytes - 1))),
              "ac44b93fc9aff773ab0005c911f8396f");
    EXPECT_EQ(wire::hashText(wire::fileHash(ed2kPartBytes, std::vector{full, empty})),
              "fc21d9af828f92a8df64beac3357425d");
    EXPECT_EQ(wire::hashText(wire::fileHash(ed2kPartBytes + 1, std::vector{full, one})),
              "06329e9dba1373512c06386fe29e3c65");
    EXPECT_EQ(wire::hashText(wire::fileHash(2 * ed2kPartBytes, std::vector{full, full, empty})),
              "114b21c63a74b6ca922291a11177dd5c");
}
TEST(Ed2k, MaximumHashsetFitsLinkAndFrameBounds) {
    std::string proofs = "p=";
    for (int i = 0; i < 65535; ++i) {
        if (i)
            proofs += ':';
        proofs += hash;
    }
    const auto parsed = parseEd2kFileLink(link("a", std::to_string(maxEd2kFileBytes), proofs + '|'));
    ASSERT_EQ(parsed.partHashes.size(), 65535);
    wire::Bytes payload(16);
    wire::append16(payload, 65535);
    payload.resize(payload.size() + 65535 * 16);
    wire::FrameDecoder decoder;
    decoder.feed(wire::encodeFrame({wire::classicProtocol, 0x52, payload}));
    ASSERT_TRUE(decoder.next());
    EXPECT_EQ(decoder.pendingBytes(), 0);
}
} // namespace

TEST(Ed2kOptions, SharedEndpointGrammarAndLimits) {
    using namespace maskpilot;
    const auto servers = parseEd2kEndpointList(" EXAMPLE.test:4661\r\nexample.test:4661\n", true);
    ASSERT_EQ(servers.size(), 1);
    EXPECT_EQ(formatEd2kEndpointList(servers), "example.test:4661");
    EXPECT_EQ(parseEd2kEndpoint("ed2k://|server|example.test|4661|/", true), servers.front());
    EXPECT_THROW(parseEd2kEndpoint("example.test:0"), std::invalid_argument);
    EXPECT_THROW(parseEd2kEndpoint("user:pass@example.test:123"), std::invalid_argument);
    EXPECT_THROW(parseEd2kEndpoint("example.test:12:4", true), std::invalid_argument);
    EXPECT_THROW(parseEd2kEndpointList(std::string(65537, 'x')), std::invalid_argument);
}
TEST(Ed2kOptions, RejectsEmbeddedWebCredentialsAndPreservesUriEscapes) {
    using namespace maskpilot;
    const std::string prefix = "ed2k://|file|a.bin|3|a448017aaf21d8525fc10ae87aa6729d|s=";
    EXPECT_THROW(parseEd2kFileLink(prefix + "https://user:pass@example.test/a|/"), std::invalid_argument);
    EXPECT_THROW(parseEd2kFileLink(prefix + "https://user%40example.test/a|/"), std::invalid_argument);
    const auto link = parseEd2kFileLink(prefix + "https://example.test/a%20b|/");
    ASSERT_EQ(link.webSeeds.size(), 1);
    EXPECT_EQ(link.webSeeds.front(), "https://example.test/a%20b");
}
