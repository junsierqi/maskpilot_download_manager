#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace maskpilot {
inline constexpr std::uint64_t ed2kPartBytes = 9'728'000;
// Legacy peers stop at the last complete part representable by a 32-bit offset.
inline constexpr std::uint64_t ed2kLegacyMaxFileBytes = (0xffffffffULL / ed2kPartBytes) * ed2kPartBytes;
inline constexpr std::size_t maxEd2kSources = 64;
inline constexpr std::size_t maxEd2kLinkBytes = 3 * 1024 * 1024;
// The wire hashset count is 16-bit, including the canonical empty trailing part.
inline constexpr std::uint64_t maxEd2kFileBytes = ed2kPartBytes * 65535 - 1;
struct Ed2kEndpoint {
    std::string host;
    std::uint16_t port = 0;
    std::uint8_t cryptOptions = 0;
    std::string clientHash;
    bool operator==(const Ed2kEndpoint&) const = default;
};
struct Ed2kOptions {
    std::vector<Ed2kEndpoint> servers;
    std::vector<Ed2kEndpoint> sources;
    bool operator==(const Ed2kOptions&) const = default;
};
struct Ed2kFileLink {
    std::string filename;
    std::uint64_t size = 0;
    std::string hash;
    std::vector<std::string> partHashes;
    std::string aichRoot;
    std::vector<std::string> webSeeds;
    std::vector<Ed2kEndpoint> sources;
};
bool isEd2kUrl(std::string_view url);
// Throws invalid_argument on unsupported or malformed fields; never silently drops proofs.
Ed2kFileLink parseEd2kFileLink(std::string_view url);
// Server fields also accept ed2k://|server|HOST|PORT|/ metadata links.
Ed2kEndpoint parseEd2kEndpoint(std::string_view text, bool server = false);
std::string formatEd2kEndpoint(const Ed2kEndpoint& endpoint);
std::vector<Ed2kEndpoint> parseEd2kEndpointList(std::string_view text, bool servers = false);
std::string formatEd2kEndpointList(const std::vector<Ed2kEndpoint>& endpoints);
std::string validateEd2kOptions(const Ed2kOptions& options);
std::uint16_t ed2kHashCount(std::uint64_t size);
} // namespace maskpilot
