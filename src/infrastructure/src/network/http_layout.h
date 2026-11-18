#pragma once
#include <cstdint>
#include <filesystem>
#include <maskpilot/application/ports.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maskpilot::http {
inline constexpr std::int64_t minSplitBytes = 256 * 1024;
inline constexpr std::size_t maxLayoutBytes = 2 * 1024 * 1024;
struct Extent {
    int id = 0;
    std::int64_t begin = 0, end = 0;
};
struct Layout {
    std::string etag, effectiveUrl;
    std::int64_t total = 0;
    std::uint64_t generation = 0;
    std::vector<Extent> extents;
    bool legacy = false;
};
std::string validateLayout(const Layout&);
Layout initialLayout(std::string etag, std::string effectiveUrl, std::int64_t total, int connections);
std::optional<Layout> decodeLayout(std::string_view);
std::string encodeLayout(const Layout&);
std::optional<Layout> readLayout(const std::filesystem::path& metadata);
void saveLayout(const std::filesystem::path& metadata, const Layout&);
// committedBytes is relative to the selected extent, not the resource or its latest request.
std::optional<Layout> splitLayout(const Layout&, int extentId, std::int64_t committedBytes);
} // namespace maskpilot::http
