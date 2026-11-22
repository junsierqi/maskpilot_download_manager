#include "live_assembly.h"
#include "../process/owned_process.h"
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdexcept>

namespace maskpilot::live {
namespace {
QString path(const std::filesystem::path& value) { return QString::fromStdU16String(value.u16string()); }
QString workspace(const std::filesystem::path& work) {
    // Reuse the existing owned media-cache namespace, including crash/restart cleanup.
    const auto directory = work / "media";
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory)))
        throw std::runtime_error("Unsafe live media workspace.");
    std::filesystem::create_directories(directory);
    return path(directory / "live-local-XXXXXX");
}
void fragmentedMp4(const QByteArray& bytes, bool initialization) {
    qsizetype offset = 0;
    bool movie = false, fragment = false, payload = false;
    int boxes = 0;
    while (offset < bytes.size()) {
        if (++boxes > 4096 || bytes.size() - offset < 8)
            throw std::runtime_error("Incomplete live fragmented MP4 box.");
        const auto integer = [&](qsizetype start, int length) {
            std::uint64_t value = 0;
            for (int i = 0; i < length; ++i)
                value = (value << 8) | static_cast<unsigned char>(bytes[start + i]);
            return value;
        };
        auto size = integer(offset, 4);
        const auto type = bytes.mid(offset + 4, 4);
        std::uint64_t header = 8;
        if (size == 1) {
            if (bytes.size() - offset < 16)
                throw std::runtime_error("Incomplete live MP4 extended box.");
            size = integer(offset + 8, 8);
            header = 16;
        }
        if (size < header || size > static_cast<std::uint64_t>(bytes.size() - offset) ||
            (type != "ftyp" && type != "moov" && type != "styp" && type != "sidx" && type != "moof" &&
             type != "mdat" && type != "free" && type != "emsg" && type != "prft"))
            throw std::runtime_error("Unsupported or malformed local live MP4 container.");
        movie |= type == "moov";
        fragment |= type == "moof";
        payload |= type == "mdat";
        offset += static_cast<qsizetype>(size);
    }
    if (initialization ? (!movie || fragment || payload) : (!fragment || !payload || movie))
        throw std::runtime_error("Live MP4 initialization or media fragment is missing.");
}
void transportOrAudio(const QByteArray& bytes) {
    if (!bytes.isEmpty() && bytes.size() % 188 == 0) {
        bool valid = true;
        for (qsizetype i = 0; i < bytes.size(); i += 188)
            valid &= static_cast<unsigned char>(bytes[i]) == 0x47;
        if (valid)
            return;
    }
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto left = bytes.size() - offset;
        if (left < 7 || static_cast<unsigned char>(bytes[offset]) != 0xff ||
            (static_cast<unsigned char>(bytes[offset + 1]) & 0xf6) != 0xf0)
            throw std::runtime_error("External live audio requires complete TS or ADTS AAC segments.");
        const auto length = ((static_cast<unsigned char>(bytes[offset + 3]) & 3) << 11) |
                            (static_cast<unsigned char>(bytes[offset + 4]) << 3) |
                            (static_cast<unsigned char>(bytes[offset + 5]) >> 5);
        if (length < 7 || length > left)
            throw std::runtime_error("Incomplete external live audio frame.");
        offset += length;
    }
}
} // namespace
QByteArray decrypt(const QByteArray& ciphertext, const QByteArray& key, QByteArray iv,
                   std::int64_t sequence) {
    if (key.size() != 16 || ciphertext.isEmpty() || ciphertext.size() > 32 * 1024 * 1024 ||
        ciphertext.size() % 16 != 0 || sequence < 0)
        throw std::runtime_error("Invalid live AES-128 key or ciphertext length.");
    if (iv.isEmpty()) {
        iv = QByteArray(16, '\0');
        auto value = static_cast<std::uint64_t>(sequence);
        for (int index = 15; index >= 8; --index) {
            iv[index] = static_cast<char>(value & 255);
            value >>= 8;
        }
    }
    if (iv.size() != 16)
        throw std::runtime_error("Invalid live AES-128 IV.");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(EVP_CIPHER_CTX_new(),
                                                                            EVP_CIPHER_CTX_free);
    QByteArray plaintext(ciphertext.size() + 16, Qt::Uninitialized);
    int size = 0, tail = 0;
    if (!context ||
        EVP_DecryptInit_ex(context.get(), EVP_aes_128_cbc(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.constData()),
                           reinterpret_cast<const unsigned char*>(iv.constData())) != 1 ||
        EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &size,
                          reinterpret_cast<const unsigned char*>(ciphertext.constData()),
                          static_cast<int>(ciphertext.size())) != 1 ||
        EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(plaintext.data()) + size,
                            &tail) != 1) {
        OPENSSL_cleanse(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
        throw std::runtime_error("Live AES-128 decryption or padding validation failed.");
    }
    plaintext.resize(size + tail);
    return plaintext;
}
void requireMuxer(const std::filesystem::path& program) {
    const QFileInfo file(path(program));
    if (program.empty() || !program.is_absolute() || !file.isFile() || !file.isExecutable() ||
        path(program).startsWith("//") || path(program).startsWith("\\\\"))
        throw std::runtime_error(
            "This HLS stream needs a trusted local media processor. Select it in Media access.");
}
Assembly::Assembly(const std::filesystem::path& work) : directory_(workspace(work)) {
    if (!directory_.isValid())
        throw std::runtime_error("Cannot create private live media workspace.");
}
QString Assembly::save(const QByteArray& bytes, const QString& extension) {
    const auto name = QString("part-%1").arg(serial_++) + extension;
    QFile file(directory_.filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size() ||
        !file.flush())
        throw std::runtime_error("Cannot store complete local live media. Check disk space.");
    return name;
}
void Assembly::append(int track, const MediaSegment& segment, const QByteArray& bytes,
                      const QByteArray& initialization) {
    if (track < 0 || track > 1 || tracks_[track].size() >= 8192 || bytes.isEmpty())
        throw std::runtime_error("Live track is empty or exceeds the bounded segment count.");
    if (segment.initialization) {
        fragmentedMp4(initialization, true);
        fragmentedMp4(bytes, false);
    } else
        transportOrAudio(bytes);
    auto& entries = tracks_[track];
    mux_ |= track == 1 || segment.initialization != nullptr ||
            (!entries.empty() && entries.back().discontinuity != segment.discontinuity);
    const auto extension = segment.initialization ? ".m4s" : (bytes.startsWith('G') ? ".ts" : ".aac");
    entries.push_back({save(bytes, extension),
                       initialization.isEmpty() ? QString{} : save(initialization, ".mp4"), segment.duration,
                       segment.discontinuity});
}
QString Assembly::playlist(int track) {
    const auto& entries = tracks_[track];
    double target = 1;
    for (const auto& entry : entries)
        target = std::max(target, std::ceil(entry.duration));
    QByteArray text =
        "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:" + QByteArray::number(static_cast<int>(target)) +
        "\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n";
    std::int64_t discontinuity = entries.front().discontinuity;
    for (const auto& entry : entries) {
        if (discontinuity != entry.discontinuity) {
            text += "#EXT-X-DISCONTINUITY\n";
            discontinuity = entry.discontinuity;
        }
        if (!entry.initialization.isEmpty())
            text += "#EXT-X-MAP:URI=\"" + entry.initialization.toLatin1() + "\"\n";
        text +=
            "#EXTINF:" + QByteArray::number(entry.duration, 'f', 6) + ",\n" + entry.file.toLatin1() + "\n";
    }
    text += "#EXT-X-ENDLIST\n";
    if (text.size() > 1024 * 1024)
        throw std::runtime_error("Local live manifest exceeds its bounded size.");
    const auto name = QString("track-%1.m3u8").arg(track);
    QFile file(directory_.filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(text) != text.size() ||
        !file.flush())
        throw std::runtime_error("Cannot write owned local live manifest.");
    return directory_.filePath(name);
}
std::filesystem::path Assembly::mux(const DownloadSpec& spec, TransferControl& control) {
    requireMuxer(spec.mediaMuxer);
    if (tracks_[0].empty())
        throw std::runtime_error("No complete live media to assemble.");
    struct Child {
        process::OwnedProcessGroup group;
        QProcess process;
        ~Child() { group.stop(process); }
    } child;
    const auto output = directory_.filePath("recording.ts");
    QStringList arguments{"-hide_banner", "-loglevel", "error", "-nostdin", "-n"};
    const int count = tracks_[1].empty() ? 1 : 2;
    for (int track = 0; track < count; ++track)
        arguments << "-protocol_whitelist" << "file,pipe" << "-format_whitelist" << "hls,mov,mpegts,aac"
                  << "-allowed_extensions" << "ts,m4s,mp4,aac" << "-i" << playlist(track);
    if (count == 2)
        arguments << "-map" << "0:v:0" << "-map" << "1:a:0" << "-shortest";
    else
        arguments << "-map" << "0:v:0?" << "-map" << "0:a:0?";
    arguments << "-c" << "copy" << "-f" << "mpegts" << output;
    QProcessEnvironment environment;
    const auto inherited = QProcessEnvironment::systemEnvironment();
    for (const auto* key :
         {"PATH", "SystemRoot", "WINDIR", "TEMP", "TMP", "TMPDIR", "LANG", "LC_ALL", "LD_LIBRARY_PATH"})
        if (inherited.contains(key))
            environment.insert(key, inherited.value(key));
    child.process.setProcessEnvironment(environment);
    child.process.setWorkingDirectory(directory_.path());
    child.process.setStandardInputFile(QProcess::nullDevice());
    child.group.prepare(child.process);
    child.process.start(path(spec.mediaMuxer), arguments);
    if (!child.process.waitForStarted(15000))
        throw std::runtime_error("Cannot start local live media processor.");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    qint64 diagnostics = 0;
    do {
        child.process.waitForFinished(50);
        diagnostics +=
            child.process.readAllStandardError().size() + child.process.readAllStandardOutput().size();
        if (control.pause || control.cancel ||
            (control.payloadBudget &&
             (control.payloadBudget->exhausted() || control.payloadBudget->failed())))
            throw std::runtime_error("Local live processing interrupted.");
        if (diagnostics > 1024 * 1024 || std::chrono::steady_clock::now() >= deadline ||
            QFileInfo(output).size() > spec.live.maxBytes)
            throw std::runtime_error("Local live processing exceeds its time, output or diagnostic limit.");
    } while (child.process.state() != QProcess::NotRunning);
    if (child.process.exitStatus() != QProcess::NormalExit || child.process.exitCode() != 0 ||
        QFileInfo(output).size() <= 0)
        throw std::runtime_error(
            "Cannot assemble the live container or aligned tracks. No output published.");
    return std::filesystem::path(output.toStdU16String());
}
} // namespace maskpilot::live
