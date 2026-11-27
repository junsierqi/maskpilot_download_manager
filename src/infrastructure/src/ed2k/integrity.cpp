#include "integrity.h"
#include <QCryptographicHash>
#include <algorithm>
#include <maskpilot/domain/ed2k.h>
#include <stdexcept>

namespace maskpilot::ed2k {
namespace {
int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
Hash md4(std::span<const std::uint8_t> bytes) {
    if (!QCryptographicHash::supportsAlgorithm(QCryptographicHash::Md4))
        throw std::runtime_error("This runtime does not support ED2K integrity hashing.");
    const auto digest = QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())),
        QCryptographicHash::Md4);
    if (digest.size() != 16)
        throw std::runtime_error("Cannot compute ED2K integrity hash.");
    Hash result;
    std::copy_n(reinterpret_cast<const std::uint8_t*>(digest.constData()), result.size(), result.begin());
    return result;
}
} // namespace
Hash parseHash(std::string_view text) {
    if (text.size() != 32)
        throw std::invalid_argument("Invalid ED2K hash length.");
    Hash result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto high = hex(text[i * 2]), low = hex(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            throw std::invalid_argument("Invalid ED2K hexadecimal hash.");
        result[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return result;
}
std::string hashText(const Hash& hash) {
    std::string result;
    result.reserve(32);
    for (auto byte : hash) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
Hash hashPart(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > ed2kPartBytes)
        throw std::invalid_argument("Oversized ED2K hash part.");
    return md4(bytes);
}
Hash fileHash(std::uint64_t size, std::span<const Hash> hashes) {
    if (hashes.size() != ed2kHashCount(size))
        throw std::invalid_argument("Invalid ED2K hashset count.");
    if (size == 0 && hashes.front() != md4({}))
        throw std::invalid_argument("Invalid empty ED2K file hash.");
    if (size < ed2kPartBytes)
        return hashes.front();
    if (size % ed2kPartBytes == 0 && hashes.back() != md4({}))
        throw std::invalid_argument("ED2K exact-part hashset lacks the empty trailing part.");
    QCryptographicHash digest(QCryptographicHash::Md4);
    if (!QCryptographicHash::supportsAlgorithm(QCryptographicHash::Md4))
        throw std::runtime_error("This runtime does not support ED2K integrity hashing.");
    for (const auto& hash : hashes)
        digest.addData(QByteArrayView(reinterpret_cast<const char*>(hash.data()), hash.size()));
    const auto result = digest.result();
    if (result.size() != 16)
        throw std::runtime_error("Cannot compute ED2K file identity.");
    Hash value;
    std::copy_n(reinterpret_cast<const std::uint8_t*>(result.constData()), value.size(), value.begin());
    return value;
}
} // namespace maskpilot::ed2k
