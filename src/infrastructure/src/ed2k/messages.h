#pragma once
#include "integrity.h"
#include "protocol.h"
#include <map>
#include <maskpilot/domain/ed2k.h>

namespace maskpilot::ed2k {
struct Tag {
    std::uint64_t number = 0;
    std::string text;
};
std::map<std::uint8_t, Tag> readTags(Reader& reader);
void numberTag(Bytes& bytes, std::uint8_t id, std::uint32_t value);
void textTag(Bytes& bytes, std::uint8_t id, std::string_view value);
Bytes inflateBounded(std::span<const std::uint8_t> input, std::size_t limit);
Frame unpack(Frame frame);
Frame hello(const Hash& identity, std::uint32_t clientId, bool answer = false);
Frame indexLogin(const Hash& identity);
Frame sourceRequest(const Ed2kFileLink& link);
struct HelloInfo {
    Hash identity{};
    std::uint32_t clientId = 0;
    bool largeFiles = false, requiresEncryption = false;
};
HelloInfo parseHello(const Frame& frame);
std::vector<Ed2kEndpoint> parseSources(const Frame& frame, const Hash& expected);
std::vector<Hash> parseHashset(const Frame& frame, const Ed2kFileLink& link);
std::vector<bool> parsePartStatus(const Frame& frame, const Ed2kFileLink& link);
void requireHash(Reader& reader, const Hash& expected);
Bytes hashBytes(const Hash& hash);
} // namespace maskpilot::ed2k
