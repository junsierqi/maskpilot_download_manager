#include "protocol.h"
#include <stdexcept>

namespace maskpilot::ed2k {
namespace {
bool validProtocol(std::uint8_t value) {
    return value == classicProtocol || value == extendedProtocol || value == packedProtocol;
}
template <class T> void append(Bytes& bytes, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value & 255));
        value >>= 8;
    }
}
template <class T> T read(Reader& reader) {
    const auto bytes = reader.take(sizeof(T));
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        value |= std::uint64_t(bytes[i]) << (8 * i);
    return static_cast<T>(value);
}
} // namespace
void append16(Bytes& bytes, std::uint16_t value) { append(bytes, value); }
void append32(Bytes& bytes, std::uint32_t value) { append(bytes, value); }
void append64(Bytes& bytes, std::uint64_t value) { append(bytes, value); }
std::span<const std::uint8_t> Reader::take(std::size_t count) {
    if (count > bytes_.size())
        throw std::invalid_argument("Truncated ED2K packet.");
    const auto result = bytes_.first(count);
    bytes_ = bytes_.subspan(count);
    return result;
}
std::uint8_t Reader::u8() { return take(1)[0]; }
std::uint16_t Reader::u16() { return read<std::uint16_t>(*this); }
std::uint32_t Reader::u32() { return read<std::uint32_t>(*this); }
std::uint64_t Reader::u64() { return read<std::uint64_t>(*this); }
void Reader::requireEnd() const {
    if (!bytes_.empty())
        throw std::invalid_argument("Unexpected ED2K packet data.");
}
Bytes encodeFrame(const Frame& frame) {
    if (!validProtocol(frame.protocol) || frame.payload.size() >= maxFrameBytes)
        throw std::invalid_argument("Invalid or oversized ED2K frame.");
    Bytes result;
    result.reserve(frame.payload.size() + 6);
    result.push_back(frame.protocol);
    append32(result, static_cast<std::uint32_t>(frame.payload.size() + 1));
    result.push_back(frame.opcode);
    result.insert(result.end(), frame.payload.begin(), frame.payload.end());
    return result;
}
void FrameDecoder::validateHeader() const {
    if (!buffer_.empty() && !validProtocol(buffer_[0]))
        throw std::invalid_argument("Unsupported ED2K protocol marker.");
    if (buffer_.size() >= 5) {
        Reader header(std::span<const std::uint8_t>(buffer_).subspan(1, 4));
        const auto length = header.u32();
        if (!length || length > maxFrameBytes)
            throw std::invalid_argument("Invalid or oversized ED2K frame length.");
    }
}
void FrameDecoder::feed(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > maxFrameBytes + 5 - buffer_.size())
        throw std::invalid_argument("ED2K receive buffer exceeds its bound.");
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    validateHeader();
}
std::optional<Frame> FrameDecoder::next() {
    validateHeader();
    if (buffer_.size() < 6)
        return {};
    Reader header(std::span<const std::uint8_t>(buffer_).subspan(1, 4));
    const auto length = header.u32();
    if (buffer_.size() < std::size_t(length) + 5)
        return {};
    Frame result{buffer_[0], buffer_[5], Bytes(buffer_.begin() + 6, buffer_.begin() + 5 + length)};
    buffer_.erase(buffer_.begin(), buffer_.begin() + 5 + length);
    return result;
}
} // namespace maskpilot::ed2k
