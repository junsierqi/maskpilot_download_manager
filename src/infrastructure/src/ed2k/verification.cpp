#include "verification.h"
#include "integrity.h"
#include <QCryptographicHash>
#include <QFile>
#include <algorithm>
#include <array>
#include <functional>
#include <maskpilot/infrastructure/adapters.h>
#include <stdexcept>

namespace maskpilot::ed2k {
namespace {
void stop(const TransferControl& control) {
    if (control.pause || control.cancel)
        throw std::runtime_error("ED2K verification stopped.");
}
QByteArray readHash(QFile& file, QCryptographicHash::Algorithm algorithm, std::uint64_t bytes,
                    const TransferControl& control) {
    QCryptographicHash hash(algorithm);
    std::array<char, 128 * 1024> buffer;
    while (bytes) {
        stop(control);
        const auto count =
            file.read(buffer.data(), static_cast<qint64>(std::min<std::uint64_t>(bytes, buffer.size())));
        if (count <= 0)
            throw std::runtime_error("Cannot read the complete ED2K stage for verification.");
        hash.addData(QByteArrayView(buffer.data(), count));
        bytes -= static_cast<std::uint64_t>(count);
    }
    return hash.result();
}
std::string base32(const QByteArray& bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string result;
    unsigned bits = 0, accumulator = 0;
    for (unsigned char byte : bytes) {
        accumulator = (accumulator << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            result += alphabet[(accumulator >> bits) & 31];
        }
    }
    if (bits)
        result += alphabet[(accumulator << (5 - bits)) & 31];
    return result;
}
// The auxiliary tree aligns large subtrees to file parts, and leaves to 180 KiB.
QByteArray auxiliary(QFile& file, std::uint64_t length, bool left, const TransferControl& control) {
    constexpr std::uint64_t block = 180 * 1024;
    const auto unit = length > ed2kPartBytes ? ed2kPartBytes : block;
    if (length <= unit)
        return readHash(file, QCryptographicHash::Sha1, length, control);
    const auto units = (length + unit - 1) / unit;
    const auto leftLength = ((units + (left ? 1 : 0)) / 2) * unit;
    const auto a = auxiliary(file, leftLength, true, control);
    const auto b = auxiliary(file, length - leftLength, false, control);
    return QCryptographicHash::hash(a + b, QCryptographicHash::Sha1);
}
} // namespace
void verifyStage(const Task& task, const Ed2kFileLink& link, const std::filesystem::path& stage,
                 const TransferControl& control) {
    const auto native = stage.u8string();
    QFile file(QString::fromUtf8(reinterpret_cast<const char*>(native.data()),
                                 static_cast<qsizetype>(native.size())));
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || std::uint64_t(file.size()) != link.size)
        throw std::runtime_error("ED2K final file size does not match its identity.");
    std::vector<Hash> hashes;
    std::uint64_t remaining = link.size;
    while (remaining) {
        const auto size = std::min(remaining, ed2kPartBytes);
        const auto digest = readHash(file, QCryptographicHash::Md4, size, control);
        if (digest.size() != 16)
            throw std::runtime_error("ED2K final part hash failed.");
        Hash part;
        std::copy_n(reinterpret_cast<const std::uint8_t*>(digest.constData()), part.size(), part.begin());
        hashes.push_back(part);
        remaining -= size;
    }
    if (link.size % ed2kPartBytes == 0)
        hashes.push_back(hashPart({}));
    if (!file.atEnd() || fileHash(link.size, hashes) != parseHash(link.hash))
        throw std::runtime_error("ED2K final file integrity verification failed.");
    if (!link.aichRoot.empty()) {
        if (!file.seek(0) || base32(auxiliary(file, link.size, true, control)) != link.aichRoot)
            throw std::runtime_error("ED2K auxiliary integrity verification failed.");
    }
    file.close();
    if (!task.spec.expectedSha256.empty()) {
        auto expected = task.spec.expectedSha256;
        std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c);
        });
        if (sha256File(stage, &control) != expected)
            throw std::runtime_error("ED2K final SHA256 verification failed.");
    }
    stop(control);
}
} // namespace maskpilot::ed2k
