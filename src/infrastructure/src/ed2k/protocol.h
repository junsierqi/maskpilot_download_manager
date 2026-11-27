#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace maskpilot::ed2k {
using Bytes = std::vector<std::uint8_t>;
// The largest 16-bit hashset, including its identity, is slightly over one MiB.
inline constexpr std::size_t maxFrameBytes = 2 * 1024 * 1024;
inline constexpr std::uint8_t classicProtocol = 0xe3;
inline constexpr std::uint8_t extendedProtocol = 0xc5;
inline constexpr std::uint8_t packedProtocol = 0xd4;
struct Frame {
    std::uint8_t protocol = classicProtocol;
    std::uint8_t opcode = 0;
    Bytes payload;
};
// Framing length includes opcode, excludes protocol and the four length bytes.
Bytes encodeFrame(const Frame& frame);
class FrameDecoder {
    Bytes buffer_;
    void validateHeader() const;

  public:
    // Feed bounded read chunks, then drain next() before feeding another chunk.
    void feed(std::span<const std::uint8_t> bytes);
    std::optional<Frame> next();
    std::size_t pendingBytes() const { return buffer_.size(); }
};
class Reader {
    std::span<const std::uint8_t> bytes_;

  public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::size_t remaining() const { return bytes_.size(); }
    std::span<const std::uint8_t> take(std::size_t count);
    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    void requireEnd() const;
};
void append16(Bytes& bytes, std::uint16_t value);
void append32(Bytes& bytes, std::uint32_t value);
void append64(Bytes& bytes, std::uint64_t value);
} // namespace maskpilot::ed2k
