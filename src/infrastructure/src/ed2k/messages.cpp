#include "messages.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <zlib.h>

namespace maskpilot::ed2k {
namespace {
void append(Bytes& target, const Hash& hash) { target.insert(target.end(), hash.begin(), hash.end()); }
std::string text(std::span<const std::uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
} // namespace
std::map<std::uint8_t, Tag> readTags(Reader& reader) {
    const auto count = reader.u32();
    if (count > 128)
        throw std::invalid_argument("Too many ED2K metadata tags.");
    std::map<std::uint8_t, Tag> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto type = reader.u8();
        std::uint8_t id = 0;
        if (type & 0x80) {
            type &= 0x7f;
            id = reader.u8();
        } else {
            const auto length = reader.u16();
            if (!length || length > 256)
                throw std::invalid_argument("Invalid ED2K tag name.");
            const auto name = reader.take(length);
            if (length == 1)
                id = name[0];
        }
        Tag value;
        switch (type) {
        case 0x01:
            reader.take(16);
            break;
        case 0x02: {
            const auto length = reader.u16();
            if (length > 4096)
                throw std::invalid_argument("ED2K metadata string is too long.");
            value.text = text(reader.take(length));
            break;
        }
        case 0x03:
            value.number = reader.u32();
            break;
        case 0x04:
            reader.take(4);
            break;
        case 0x05:
            value.number = reader.u8();
            break;
        case 0x06: {
            const auto bits = reader.u16();
            reader.take((bits + 7U) / 8U);
            break;
        }
        case 0x07: {
            const auto length = reader.u32();
            if (length > 65536)
                throw std::invalid_argument("ED2K metadata blob is too long.");
            reader.take(length);
            break;
        }
        case 0x08:
            value.number = reader.u16();
            break;
        case 0x09:
            value.number = reader.u8();
            break;
        case 0x0a:
            reader.take(reader.u8());
            break;
        case 0x0b:
            value.number = reader.u64();
            break;
        default:
            if (type >= 0x11 && type <= 0x20)
                value.text = text(reader.take(type - 0x10));
            else
                throw std::invalid_argument("Unsupported ED2K metadata type.");
        }
        if (id && !result.emplace(id, std::move(value)).second)
            throw std::invalid_argument("Duplicate ED2K metadata tag.");
    }
    return result;
}
void numberTag(Bytes& bytes, std::uint8_t id, std::uint32_t value) {
    bytes.push_back(0x03);
    append16(bytes, 1);
    bytes.push_back(id);
    append32(bytes, value);
}
void textTag(Bytes& bytes, std::uint8_t id, std::string_view value) {
    if (value.size() > 4096)
        throw std::invalid_argument("ED2K metadata string is too long.");
    bytes.push_back(0x02);
    append16(bytes, 1);
    bytes.push_back(id);
    append16(bytes, static_cast<std::uint16_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
Bytes inflateBounded(std::span<const std::uint8_t> input, std::size_t limit) {
    if (input.empty() || input.size() > maxFrameBytes || !limit || limit > maxFrameBytes)
        throw std::invalid_argument("Invalid compressed ED2K payload bounds.");
    Bytes result(limit);
    uLongf length = static_cast<uLongf>(result.size());
    uLong consumed = static_cast<uLong>(input.size());
    if (uncompress2(result.data(), &length, input.data(), &consumed) != Z_OK || consumed != input.size())
        throw std::invalid_argument("Invalid or oversized compressed ED2K payload.");
    result.resize(length);
    return result;
}
Frame unpack(Frame frame) {
    if (frame.protocol == packedProtocol) {
        frame.payload = inflateBounded(frame.payload, maxFrameBytes - 1);
        frame.protocol = extendedProtocol;
    }
    return frame;
}
Bytes hashBytes(const Hash& hash) { return {hash.begin(), hash.end()}; }
void requireHash(Reader& reader, const Hash& expected) {
    const auto actual = reader.take(expected.size());
    if (!std::equal(actual.begin(), actual.end(), expected.begin()))
        throw std::invalid_argument("ED2K response identifies a different file.");
}
Frame hello(const Hash& identity, std::uint32_t clientId, bool answer) {
    Bytes payload;
    if (!answer)
        payload.push_back(16);
    append(payload, identity);
    append32(payload, clientId);
    append16(payload, 0);
    append32(payload, 5);
    textTag(payload, 0x01, "MaskPilot");
    numberTag(payload, 0x11, 0x3c);
    // Unicode, compression and refusal to expose shared files; no source exchange.
    numberTag(payload, 0xfa, (1U << 28) | (1U << 20) | (1U << 2));
    // Large-file support; no UDP, callbacks or encryption advertisement.
    numberTag(payload, 0xfe, 1U << 4);
    numberTag(payload, 0xfb, 0xff000000U | (3U << 10));
    append32(payload, 0);
    append16(payload, 0);
    return {classicProtocol, static_cast<std::uint8_t>(answer ? 0x4c : 0x01), std::move(payload)};
}
Frame indexLogin(const Hash& identity) {
    Bytes payload = hashBytes(identity);
    append32(payload, 0);
    append16(payload, 0);
    append32(payload, 3);
    textTag(payload, 0x01, "MaskPilot");
    numberTag(payload, 0x11, 0x3c);
    // Compression, compact tags, Unicode and large-file source requests.
    numberTag(payload, 0x20, 0x119);
    return {classicProtocol, 0x01, std::move(payload)};
}
Frame sourceRequest(const Ed2kFileLink& link) {
    Bytes payload = hashBytes(parseHash(link.hash));
    if (link.size > ed2kLegacyMaxFileBytes) {
        append32(payload, 0);
        append64(payload, link.size);
    } else
        append32(payload, static_cast<std::uint32_t>(link.size));
    return {classicProtocol, 0x19, std::move(payload)};
}
HelloInfo parseHello(const Frame& frame) {
    Reader reader(frame.payload);
    if (frame.opcode == 0x01 && reader.u8() != 16)
        throw std::invalid_argument("Invalid ED2K handshake hash length.");
    HelloInfo info;
    const auto hash = reader.take(16);
    std::copy(hash.begin(), hash.end(), info.identity.begin());
    info.clientId = reader.u32();
    reader.u16();
    const auto tags = readTags(reader);
    reader.take(6);
    if (reader.remaining() == 4)
        reader.take(4); // Optional legacy compatibility marker.
    reader.requireEnd();
    if (const auto found = tags.find(0xfe); found != tags.end()) {
        info.largeFiles = bool(found->second.number & (1U << 4));
        info.requiresEncryption = bool(found->second.number & (1U << 9));
    }
    return info;
}
std::vector<Ed2kEndpoint> parseSources(const Frame& frame, const Hash& expected) {
    Reader reader(frame.payload);
    requireHash(reader, expected);
    const auto count = reader.u8();
    std::vector<Ed2kEndpoint> result;
    for (unsigned i = 0; i < count; ++i) {
        const auto ip = reader.u32();
        Ed2kEndpoint endpoint;
        endpoint.port = reader.u16();
        if (frame.opcode == 0x44) {
            endpoint.cryptOptions = reader.u8();
            if (endpoint.cryptOptions & 0x80) {
                Hash hash;
                const auto data = reader.take(16);
                std::copy(data.begin(), data.end(), hash.begin());
                endpoint.clientHash = hashText(hash);
            }
        }
        // Low IDs require inbound callbacks, which this download-only engine does not advertise.
        if (ip < (1U << 24) || !endpoint.port)
            continue;
        endpoint.host = std::to_string(ip & 255) + '.' + std::to_string((ip >> 8) & 255) + '.' +
                        std::to_string((ip >> 16) & 255) + '.' + std::to_string(ip >> 24);
        if (result.size() < maxEd2kSources)
            result.push_back(std::move(endpoint));
    }
    reader.requireEnd();
    return result;
}
std::vector<Hash> parseHashset(const Frame& frame, const Ed2kFileLink& link) {
    const auto expected = parseHash(link.hash);
    if (!link.partHashes.empty()) {
        std::vector<Hash> proof;
        for (const auto& hash : link.partHashes)
            proof.push_back(parseHash(hash));
        if (fileHash(link.size, proof) != expected)
            throw std::invalid_argument("ED2K link part proofs disagree with its identity.");
    }
    Reader reader(frame.payload);
    requireHash(reader, expected);
    const auto count = reader.u16();
    if (!count && link.size < ed2kPartBytes) {
        reader.requireEnd();
        return {expected};
    }
    if (count != ed2kHashCount(link.size))
        throw std::invalid_argument("ED2K hashset does not match file size.");
    std::vector<Hash> result(count);
    for (auto& hash : result) {
        const auto bytes = reader.take(16);
        std::copy(bytes.begin(), bytes.end(), hash.begin());
    }
    reader.requireEnd();
    if (fileHash(link.size, result) != expected)
        throw std::invalid_argument("ED2K hashset does not prove the requested identity.");
    if (!link.partHashes.empty())
        for (std::size_t i = 0; i < result.size(); ++i)
            if (result[i] != parseHash(link.partHashes[i]))
                throw std::invalid_argument("ED2K peer and link part proofs disagree.");
    return result;
}
std::vector<bool> parsePartStatus(const Frame& frame, const Ed2kFileLink& link) {
    Reader reader(frame.payload);
    requireHash(reader, parseHash(link.hash));
    const auto count = reader.u16();
    const auto parts = static_cast<std::size_t>((link.size + ed2kPartBytes - 1) / ed2kPartBytes);
    std::vector<bool> result(parts, true);
    if (!count) {
        reader.requireEnd();
        return result;
    }
    if (count != parts && count != ed2kHashCount(link.size))
        throw std::invalid_argument("ED2K peer part bitmap does not match file size.");
    const auto bitmap = reader.take((count + 7U) / 8U);
    reader.requireEnd();
    for (std::size_t i = 0; i < parts; ++i)
        result[i] = bool(bitmap[i / 8] & (1U << (i % 8)));
    return result;
}
} // namespace maskpilot::ed2k
