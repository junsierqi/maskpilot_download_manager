#include "../../src/infrastructure/src/network/http_layout.h"
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace http = maskpilot::http;
constexpr auto megabyte = std::int64_t{1024 * 1024};
http::Layout sample(int connections = 2) {
    return http::initialLayout("\"fixture-etag\"", "http://127.0.0.1/file?fixture=1", 16 * megabyte,
                               connections);
}
std::filesystem::path pathFor(const QTemporaryDir& directory, const char* name) {
    return std::filesystem::path(directory.path().toStdWString()) / name;
}
} // namespace

TEST(HttpLayout, StrongIdentityAndSortedFullCoverageRoundTrip) {
    const auto original = sample(8);
    const auto encoded = http::encodeLayout(original);
    EXPECT_TRUE(encoded.starts_with("MPHTTP 2\n"));
    const auto restored = http::decodeLayout(encoded);
    ASSERT_TRUE(restored);
    EXPECT_FALSE(restored->legacy);
    EXPECT_EQ(restored->generation, 1u);
    EXPECT_EQ(restored->etag, original.etag);
    EXPECT_EQ(restored->effectiveUrl, original.effectiveUrl);
    EXPECT_EQ(restored->total, original.total);
    ASSERT_EQ(restored->extents.size(), 8u);
    for (std::size_t n = 0; n < restored->extents.size(); ++n) {
        EXPECT_EQ(restored->extents[n].id, static_cast<int>(n));
        EXPECT_EQ(restored->extents[n].begin, original.extents[n].begin);
        EXPECT_EQ(restored->extents[n].end, original.extents[n].end);
    }
}
TEST(HttpLayout, LegacyOffsetsRetainTheirOriginalExtentIdentityDuringMigration) {
    std::ostringstream legacy;
    legacy << std::quoted(std::string("\"old-tag\"")) << ' ' << 16 * megabyte << " 8 "
           << std::quoted(std::string("http://127.0.0.1/legacy"));
    const auto decoded = http::decodeLayout(legacy.str());
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->legacy);
    EXPECT_EQ(decoded->generation, 0u);
    const auto migrated = http::decodeLayout(http::encodeLayout(*decoded));
    ASSERT_TRUE(migrated);
    EXPECT_FALSE(migrated->legacy);
    EXPECT_EQ(migrated->generation, 1u);
    ASSERT_EQ(migrated->extents.size(), 8u);
    for (std::size_t n = 0; n < migrated->extents.size(); ++n) {
        EXPECT_EQ(migrated->extents[n].id, decoded->extents[n].id);
        EXPECT_EQ(migrated->extents[n].begin, decoded->extents[n].begin);
        EXPECT_EQ(migrated->extents[n].end, decoded->extents[n].end);
    }
}
TEST(HttpLayout, SplitMovesOnlyUnwrittenTailAndDoesNotReassignTheCommittedPrefix) {
    const auto original = sample();
    const auto prefix = 3 * megabyte;
    const auto split = http::splitLayout(original, 0, prefix);
    ASSERT_TRUE(split);
    EXPECT_EQ(original.extents.size(), 2u);
    EXPECT_EQ(split->generation, original.generation + 1);
    ASSERT_EQ(split->extents.size(), 3u);
    EXPECT_EQ(split->extents[0].id, 0);
    EXPECT_EQ(split->extents[1].id, 2);
    EXPECT_EQ(split->extents[2].id, 1); // Assembly order is not stable-ID order.
    EXPECT_GT(split->extents[0].end, prefix);
    EXPECT_EQ(split->extents[0].end, split->extents[1].begin);
    EXPECT_EQ(split->extents[1].end, original.extents[0].end);
    EXPECT_EQ(split->extents[2].begin, original.extents[1].begin);
    EXPECT_TRUE(http::validateLayout(*split).empty());
    EXPECT_FALSE(http::splitLayout(original, 99, 0));
    EXPECT_FALSE(http::splitLayout(original, 0, -1));
    EXPECT_FALSE(http::splitLayout(original, 0, original.extents[0].end + 1));
    EXPECT_FALSE(http::splitLayout(original, 0, original.extents[0].end - http::minSplitBytes));
}
TEST(HttpLayout, MaximumStableExtentCountStopsSplittingWithoutInvalidatingCoverage) {
    auto layout = http::initialLayout("\"large\"", "http://127.0.0.1/large", 1024 * megabyte, 1);
    while (layout.extents.size() < maskpilot::maxRecoveryExtents) {
        const auto largest =
            std::max_element(layout.extents.begin(), layout.extents.end(),
                             [](const auto& a, const auto& b) { return a.end - a.begin < b.end - b.begin; });
        const auto next = http::splitLayout(layout, largest->id, 0);
        ASSERT_TRUE(next);
        layout = *next;
    }
    EXPECT_EQ(layout.generation, maskpilot::maxRecoveryExtents);
    EXPECT_TRUE(http::validateLayout(layout).empty());
    EXPECT_FALSE(http::splitLayout(layout, 0, 0));
    const auto decoded = http::decodeLayout(http::encodeLayout(layout));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->extents.size(), maskpilot::maxRecoveryExtents);
}
TEST(HttpLayout, RejectsMalformedVersionsCoverageIdsAndIntegerBounds) {
    for (const auto& encoded : {std::string{}, std::string("MPHTTP 99\n"), std::string("MPHTTP 2\ntruncated"),
                                http::encodeLayout(sample()) + "unexpected"})
        EXPECT_FALSE(http::decodeLayout(encoded));
    auto invalid = sample();
    invalid.extents[1].id = invalid.extents[0].id;
    EXPECT_FALSE(http::validateLayout(invalid).empty());
    invalid = sample();
    invalid.extents[1].begin -= 1;
    EXPECT_FALSE(http::validateLayout(invalid).empty());
    invalid = sample();
    invalid.extents[1].begin += 1;
    EXPECT_FALSE(http::validateLayout(invalid).empty());
    invalid = sample();
    invalid.extents[0].id = 256;
    EXPECT_FALSE(http::validateLayout(invalid).empty());
    invalid = sample();
    invalid.total = -1;
    EXPECT_FALSE(http::validateLayout(invalid).empty());
    invalid = sample();
    invalid.generation = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(http::validateLayout(invalid).empty());
    invalid = sample();
    invalid.etag = "W/\"weak\"";
    EXPECT_FALSE(http::validateLayout(invalid).empty());
    EXPECT_THROW(http::encodeLayout(invalid), std::invalid_argument);
    invalid = sample();
    invalid.legacy = true; // An invalid legacy generation cannot be repaired by encoding.
    EXPECT_THROW(http::encodeLayout(invalid), std::invalid_argument);
    EXPECT_FALSE(http::decodeLayout(std::string(http::maxLayoutBytes + 1, 'x')));
}
TEST(HttpLayout, ZeroLengthAndLargeIntegerRangesStayWellDefined) {
    const auto empty = http::initialLayout("\"empty\"", "https://example.invalid/empty", 0, 8);
    ASSERT_EQ(empty.extents.size(), 1u);
    EXPECT_EQ(empty.extents.front().begin, 0);
    EXPECT_EQ(empty.extents.front().end, 0);
    EXPECT_TRUE(http::decodeLayout(http::encodeLayout(empty)));
    auto maximum = http::initialLayout("\"maximum\"", "https://example.invalid/maximum",
                                       std::numeric_limits<std::int64_t>::max(), 1);
    const auto split = http::splitLayout(maximum, 0, std::numeric_limits<std::int64_t>::max() / 3);
    ASSERT_TRUE(split);
    EXPECT_TRUE(http::validateLayout(*split).empty());
    EXPECT_EQ(split->extents.back().end, maximum.total);
}
TEST(HttpLayout, ReadsOnlyTheAuthoritativeFileAndAcceptsBoundedLargeIdentityFields) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = pathFor(directory, "identity");
    const auto original = sample();
    http::saveLayout(path, original);
    const auto split = http::splitLayout(original, 0, megabyte);
    ASSERT_TRUE(split);
    {
        std::ofstream temporary(pathFor(directory, "identity.new"));
        temporary << http::encodeLayout(*split);
    }
    const auto read = http::readLayout(path);
    ASSERT_TRUE(read);
    EXPECT_EQ(read->generation, original.generation);
    auto large = original;
    large.etag = "\"" + std::string(900000, 'x') + "\"";
    large.effectiveUrl = "https://example.invalid/" + std::string(32000, 'a');
    EXPECT_TRUE(http::decodeLayout(http::encodeLayout(large)));
}
#ifdef _WIN32
TEST(HttpLayout, TransientReaderReleasesBeforeBoundedAtomicReplacementRetryEnds) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = pathFor(directory, "identity");
    const auto original = sample();
    http::saveLayout(path, original);
    const auto next = http::splitLayout(original, 0, megabyte);
    ASSERT_TRUE(next);
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
    std::jthread reader([lock = std::move(lock)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        lock.reset();
    });
    EXPECT_NO_THROW(http::saveLayout(path, *next));
    reader.join();
    const auto stored = http::readLayout(path);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->generation, next->generation);
}
TEST(HttpLayout, FailedAtomicReplacementPreservesThePriorAuthoritativeGeneration) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = pathFor(directory, "identity");
    const auto original = sample();
    http::saveLayout(path, original);
    struct Handle {
        HANDLE value;
        ~Handle() {
            if (value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    } lock{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr)};
    ASSERT_NE(lock.value, INVALID_HANDLE_VALUE);
    const auto next = http::splitLayout(original, 0, megabyte);
    ASSERT_TRUE(next);
    EXPECT_THROW(http::saveLayout(path, *next), std::runtime_error);
    const auto read = http::readLayout(path);
    ASSERT_TRUE(read);
    EXPECT_EQ(read->generation, original.generation);
    EXPECT_EQ(read->extents.size(), original.extents.size());
    EXPECT_TRUE(std::filesystem::exists(pathFor(directory, "identity.new")));
}
#endif
