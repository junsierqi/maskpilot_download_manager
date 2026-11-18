#include "http_layout.h"
#include "../filesystem/transfer_files.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace maskpilot::http {
namespace {
bool controls(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}
} // namespace
std::string validateLayout(const Layout& layout) {
    if (layout.etag.size() < 2 || layout.etag.front() != '"' || layout.etag.back() != '"' ||
        controls(layout.etag) || layout.etag.size() > 1024 * 1024 || layout.total < 0 ||
        layout.effectiveUrl.size() > 32768 || controls(layout.effectiveUrl) ||
        !(layout.effectiveUrl.starts_with("http://") || layout.effectiveUrl.starts_with("https://")))
        return "Invalid HTTP recovery identity.";
    const auto hostBegin = layout.effectiveUrl.find("://") + 3;
    const auto hostEnd = layout.effectiveUrl.find_first_of("/?#", hostBegin);
    const auto authority = layout.effectiveUrl.substr(hostBegin, hostEnd - hostBegin);
    if (authority.empty() || authority.find('@') != std::string::npos)
        return "Invalid HTTP recovery resource URL.";
    if (layout.extents.empty() || layout.extents.size() > maxRecoveryExtents ||
        (layout.legacy && (layout.extents.size() > 8 || layout.generation != 0)) ||
        (!layout.legacy &&
         (layout.generation == 0 ||
          layout.generation > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))))
        return "Invalid HTTP recovery version, generation or extent count.";
    std::array<bool, maxRecoveryExtents> ids{};
    std::int64_t previous = 0;
    for (const auto& extent : layout.extents) {
        if (extent.id < 0 || extent.id >= static_cast<int>(maxRecoveryExtents) || ids[extent.id] ||
            extent.begin != previous || extent.end < extent.begin || extent.end > layout.total ||
            (extent.begin == extent.end && !(layout.total == 0 && layout.extents.size() == 1)))
            return "HTTP recovery extents contain a gap, overlap or invalid identity.";
        ids[extent.id] = true;
        previous = extent.end;
    }
    return previous == layout.total ? std::string{} : "HTTP recovery extents do not cover the resource.";
}
Layout initialLayout(std::string etag, std::string effectiveUrl, std::int64_t total, int connections) {
    if (total < 0 || connections < 1 || connections > 8)
        throw std::invalid_argument("Invalid initial HTTP recovery layout.");
    Layout layout{std::move(etag), std::move(effectiveUrl), total, 1, {}, false};
    for (const auto& range : splitSegments(total, connections))
        layout.extents.push_back({static_cast<int>(layout.extents.size()), range.begin, range.end});
    if (const auto error = validateLayout(layout); !error.empty())
        throw std::invalid_argument(error);
    return layout;
}
std::optional<Layout> decodeLayout(std::string_view data) {
    if (data.empty() || data.size() > maxLayoutBytes)
        return std::nullopt;
    std::istringstream input{std::string(data)};
    input >> std::ws;
    Layout layout;
    int count = 0;
    if (input.peek() == '"') {
        layout.legacy = true;
        if (!(input >> std::quoted(layout.etag) >> layout.total >> count >>
              std::quoted(layout.effectiveUrl)) ||
            count < 1 || count > 8 || layout.total < 0)
            return std::nullopt;
        for (const auto& range : splitSegments(layout.total, count))
            layout.extents.push_back({static_cast<int>(layout.extents.size()), range.begin, range.end});
        if (layout.extents.size() != static_cast<std::size_t>(count))
            return std::nullopt;
    } else {
        std::string magic;
        int version = 0;
        if (!(input >> magic >> version) || magic != "MPHTTP" || version != 2 ||
            !(input >> std::quoted(layout.etag) >> layout.total >> std::quoted(layout.effectiveUrl) >>
              layout.generation >> count) ||
            count < 1 || count > static_cast<int>(maxRecoveryExtents))
            return std::nullopt;
        for (int n = 0; n < count; ++n) {
            Extent extent;
            if (!(input >> extent.id >> extent.begin >> extent.end))
                return std::nullopt;
            layout.extents.push_back(extent);
        }
    }
    input >> std::ws;
    if (!input.eof() || !validateLayout(layout).empty())
        return std::nullopt;
    return layout;
}
std::string encodeLayout(const Layout& value) {
    if (const auto error = validateLayout(value); !error.empty())
        throw std::invalid_argument(error);
    auto layout = value;
    if (layout.legacy) {
        layout.legacy = false;
        layout.generation = 1;
    }
    if (const auto error = validateLayout(layout); !error.empty())
        throw std::invalid_argument(error);
    std::ostringstream output;
    output << "MPHTTP 2\n"
           << std::quoted(layout.etag) << ' ' << layout.total << ' ' << std::quoted(layout.effectiveUrl)
           << ' ' << layout.generation << ' ' << layout.extents.size() << '\n';
    for (const auto& extent : layout.extents)
        output << extent.id << ' ' << extent.begin << ' ' << extent.end << '\n';
    auto result = output.str();
    if (result.size() > maxLayoutBytes)
        throw std::invalid_argument("HTTP recovery metadata exceeds its size limit.");
    return result;
}
std::optional<Layout> readLayout(const std::filesystem::path& metadata) {
    std::error_code error;
    const auto size = std::filesystem::file_size(metadata, error);
    if (error || size == 0 || size > maxLayoutBytes)
        return std::nullopt;
    std::ifstream input(metadata, std::ios::binary);
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!input.read(data.data(), static_cast<std::streamsize>(data.size())) ||
        input.peek() != std::char_traits<char>::eof())
        return std::nullopt;
    return decodeLayout(data);
}
void saveLayout(const std::filesystem::path& metadata, const Layout& layout) {
    const auto encoded = encodeLayout(layout);
    // Short-lived readers/indexers can deny replacement on Windows even with delete sharing.
    // Retry the same complete generation; persistent errors still leave the old one authoritative.
    for (int attempt = 0;; ++attempt) {
        try {
            transfer_files::atomicText(metadata, encoded);
            return;
        } catch (const std::runtime_error&) {
            if (attempt == 24)
                throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}
std::optional<Layout> splitLayout(const Layout& input, int extentId, std::int64_t committedBytes) {
    if (!validateLayout(input).empty() || input.extents.size() >= maxRecoveryExtents)
        return std::nullopt;
    auto layout = input;
    if (layout.legacy) {
        layout.legacy = false;
        layout.generation = 1;
    }
    if (layout.generation == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    const auto victim = std::find_if(layout.extents.begin(), layout.extents.end(),
                                     [&](const auto& extent) { return extent.id == extentId; });
    if (victim == layout.extents.end() || committedBytes < 0 || committedBytes > victim->end - victim->begin)
        return std::nullopt;
    const auto remaining = victim->end - victim->begin - committedBytes;
    if (remaining < 2 * minSplitBytes)
        return std::nullopt;
    int id = 0;
    for (const auto& extent : layout.extents)
        id = std::max(id, extent.id + 1);
    if (id >= static_cast<int>(maxRecoveryExtents))
        return std::nullopt;
    const auto cut = victim->begin + committedBytes + remaining / 2;
    const Extent child{id, cut, victim->end};
    victim->end = cut;
    layout.extents.push_back(child);
    std::sort(layout.extents.begin(), layout.extents.end(),
              [](const auto& a, const auto& b) { return a.begin < b.begin; });
    ++layout.generation;
    if (!validateLayout(layout).empty())
        return std::nullopt;
    try {
        (void)encodeLayout(layout);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    return layout;
}
} // namespace maskpilot::http
