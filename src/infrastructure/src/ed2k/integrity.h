#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace maskpilot::ed2k {
using Hash = std::array<std::uint8_t, 16>;
Hash parseHash(std::string_view hex);
std::string hashText(const Hash& hash);
Hash hashPart(std::span<const std::uint8_t> bytes);
// For exact part multiples, require and validate the extra MD4(empty) hash.
Hash fileHash(std::uint64_t size, std::span<const Hash> hashes);
} // namespace maskpilot::ed2k
