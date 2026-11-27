#include "checkpoint.h"
#include "../filesystem/transfer_files.h"
#include "identity.h"
#include "protocol.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <maskpilot/domain/download_context.h>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace maskpilot::ed2k {
namespace {
QString qpath(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qsizetype>(bytes.size()));
}
void checkStop(const TransferControl& control) {
    if (control.pause || control.cancel)
        throw std::runtime_error("ED2K operation stopped.");
}
void regular(const std::filesystem::path& path, bool directory = false) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error ||
        (directory ? !std::filesystem::is_directory(status) : !std::filesystem::is_regular_file(status)))
        throw std::runtime_error("ED2K recovery path is not a regular owned file or directory.");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("ED2K recovery does not follow reparse points.");
#endif
}
void sync(QFile& file) {
    if (!file.flush())
        throw std::runtime_error("Cannot flush ED2K recovery data.");
#ifdef _WIN32
    if (!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()))))
#else
    if (fsync(file.handle()) != 0)
#endif
        throw std::runtime_error("Cannot synchronize ED2K recovery data.");
}
void syncDirectory(const std::filesystem::path& path) {
#ifndef _WIN32
    const auto descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
        throw std::runtime_error("Cannot open ED2K recovery directory.");
    const auto result = fsync(descriptor);
    close(descriptor);
    if (result != 0)
        throw std::runtime_error("Cannot synchronize ED2K recovery directory.");
#else
    (void)path; // replaceOwnedFile/publishFile use write-through renames on Windows.
#endif
}
Bytes read(const std::filesystem::path& path, std::size_t maximum, TransferControl* control = nullptr) {
    regular(path);
    QFile file(qpath(path));
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || std::uint64_t(file.size()) > maximum)
        throw std::runtime_error("Invalid or oversized ED2K recovery file.");
    Bytes result(static_cast<std::size_t>(file.size()));
    std::size_t offset = 0;
    while (offset < result.size()) {
        if (control)
            checkStop(*control);
        const auto count =
            file.read(reinterpret_cast<char*>(result.data() + offset),
                      static_cast<qint64>(std::min<std::size_t>(128 * 1024, result.size() - offset)));
        if (count <= 0)
            throw std::runtime_error("Cannot read ED2K recovery data.");
        offset += static_cast<std::size_t>(count);
    }
    if (!file.atEnd())
        throw std::runtime_error("ED2K recovery file changed during read.");
    return result;
}
void writeNew(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
              TransferControl& control) {
    QFile file(qpath(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot exclusively create ED2K recovery data.");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            checkStop(control);
            const auto count =
                file.write(reinterpret_cast<const char*>(bytes.data() + offset),
                           static_cast<qint64>(std::min<std::size_t>(128 * 1024, bytes.size() - offset)));
            if (count <= 0)
                throw std::runtime_error("Cannot write ED2K recovery data. Check disk space.");
            offset += static_cast<std::size_t>(count);
        }
        sync(file);
        file.close();
    } catch (...) {
        file.close();
        QFile::remove(qpath(path)); // Only this invocation's exclusively created temporary file.
        throw;
    }
}
QJsonObject readManifest(const std::filesystem::path& path) {
    const auto bytes = read(path, 64 * 1024);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())),
        &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Invalid ED2K recovery checkpoint.");
    return document.object();
}
std::string fileIdentity(QFile& file) {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION value{};
    if (!GetFileInformationByHandle(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())), &value))
        throw std::runtime_error("Cannot identify owned ED2K stage.");
    return std::to_string(value.dwVolumeSerialNumber) + ':' + std::to_string(value.nFileIndexHigh) + ':' +
           std::to_string(value.nFileIndexLow);
#else
    struct stat value{};
    if (fstat(file.handle(), &value) != 0)
        throw std::runtime_error("Cannot identify owned ED2K stage.");
    return std::to_string(value.st_dev) + ':' + std::to_string(value.st_ino);
#endif
}
std::string fileIdentity(const std::filesystem::path& path) {
    regular(path);
    QFile file(qpath(path));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot inspect owned ED2K stage.");
    return fileIdentity(file);
}
std::string destinationIdentity(const std::filesystem::path& path) {
    return QCryptographicHash::hash(qpath(path).toUtf8(), QCryptographicHash::Sha256).toHex().toStdString();
}
std::filesystem::path stagePath(const Task& task) {
    if (!isOpaqueId(task.id))
        throw std::invalid_argument("Invalid ED2K task identity.");
    auto stage = task.spec.destination;
    stage += ".maskpilot-" + task.id + ".tmp";
    return stage;
}
} // namespace
struct Checkpoint::Impl {
    Task task;
    std::filesystem::path folder;
    Ed2kFileLink link;
    TransferControl& control;
    Hash client{};
    std::string route, payloadIdentity, proofIdentity, stageIdentity;
    std::vector<Hash> proof;
    std::vector<bool> done;
    Impl(const Task& value, std::filesystem::path path, Ed2kFileLink source, TransferControl& flags)
        : task(value), folder(std::move(path)), link(std::move(source)), control(flags) {}
    std::size_t size(std::size_t part) const {
        if (part >= done.size())
            throw std::out_of_range("ED2K recovery part is out of range.");
        return static_cast<std::size_t>(std::min(ed2kPartBytes, link.size - part * ed2kPartBytes));
    }
    void openPayload(QFile& file, QIODevice::OpenMode mode) const {
        const auto path = folder / "payload.bin";
        regular(path);
        file.setFileName(qpath(path));
        if (!file.open(mode) || fileIdentity(file) != payloadIdentity || file.size() < 0 ||
            std::uint64_t(file.size()) > link.size)
            throw std::runtime_error(
                "ED2K recovery data was replaced or exceeds its identity. Existing files were preserved.");
    }
    Hash hashRange(QFile& file, std::size_t part) const {
        if (!file.seek(static_cast<qint64>(part * ed2kPartBytes)))
            throw std::runtime_error("Cannot seek ED2K recovery part.");
        QCryptographicHash digest(QCryptographicHash::Md4);
        std::array<char, 128 * 1024> buffer;
        auto remaining = size(part);
        while (remaining) {
            checkStop(control);
            const auto count =
                file.read(buffer.data(), static_cast<qint64>(std::min(remaining, buffer.size())));
            if (count <= 0)
                throw std::runtime_error("ED2K recovery part changed during verification.");
            digest.addData(QByteArrayView(buffer.data(), count));
            remaining -= static_cast<std::size_t>(count);
        }
        const auto bytes = digest.result();
        if (bytes.size() != 16)
            throw std::runtime_error("Cannot hash ED2K recovery data.");
        Hash result;
        std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.constData()), result.size(), result.begin());
        return result;
    }
    void rehash() {
        std::fill(done.begin(), done.end(), false);
        QFile file;
        openPayload(file, QIODevice::ReadOnly);
        const auto bytes = static_cast<std::uint64_t>(file.size());
        if (proof.empty())
            return;
        for (std::size_t i = 0; i < done.size(); ++i) {
            checkStop(control);
            if (i * ed2kPartBytes + size(i) <= bytes)
                done[i] = hashRange(file, i) == proof[i];
        }
    }
    void save() {
        regular(folder, true);
        const auto target = folder / "identity.json";
        if (std::filesystem::exists(target))
            regular(target);
        QByteArray bitmap(static_cast<qsizetype>((done.size() + 7) / 8), char(0));
        for (std::size_t i = 0; i < done.size(); ++i)
            if (done[i])
                bitmap[static_cast<qsizetype>(i / 8)] = static_cast<char>(
                    static_cast<unsigned char>(bitmap[static_cast<qsizetype>(i / 8)]) | (1U << (i % 8)));
        const auto payload =
            QJsonDocument(QJsonObject{{"version", 2},
                                      {"task", QString::fromStdString(task.id)},
                                      {"owner", QString::fromStdString(task.ownerId)},
                                      {"hash", QString::fromStdString(link.hash)},
                                      {"size", QString::number(static_cast<qulonglong>(link.size))},
                                      {"route", QString::fromStdString(route)},
                                      {"client", QString::fromStdString(hashText(client))},
                                      {"payload", QString::fromStdString(payloadIdentity)},
                                      {"proof", QString::fromStdString(proofIdentity)},
                                      {"verified", QString::fromLatin1(bitmap.toHex())},
                                      {"stage", QString::fromStdString(stageIdentity)},
                                      {"destination",
                                       QString::fromStdString(destinationIdentity(task.spec.destination))}})
                .toJson(QJsonDocument::Compact);
        const auto temporary = folder / ("checkpoint-" + newTaskId());
        writeNew(temporary,
                 {reinterpret_cast<const std::uint8_t*>(payload.constData()),
                  static_cast<std::size_t>(payload.size())},
                 control);
        try {
            transfer_files::replaceOwnedFile(temporary, target);
            syncDirectory(folder);
        } catch (...) {
            QFile::remove(qpath(temporary));
            throw;
        }
    }
    std::vector<Hash> readProof() const {
        const auto path = folder / "hashset.bin";
        const auto bytes = read(path, 65535 * 16, &control);
        if (bytes.size() != std::size_t(ed2kHashCount(link.size)) * 16)
            throw std::runtime_error("Invalid ED2K recovery hashset size.");
        std::vector<Hash> hashes(bytes.size() / 16);
        for (std::size_t i = 0; i < hashes.size(); ++i)
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(i * 16), 16, hashes[i].begin());
        if (fileHash(link.size, hashes) != parseHash(link.hash))
            throw std::runtime_error("ED2K recovery hashset does not prove the file identity.");
        return hashes;
    }
    void storeProof() {
        if (proof.empty() || link.size < ed2kPartBytes)
            return;
        const auto target = folder / "hashset.bin";
        if (std::filesystem::exists(target)) {
            if ((!proofIdentity.empty() && fileIdentity(target) != proofIdentity) || readProof() != proof)
                throw std::runtime_error("Unrecognized ED2K proof data was preserved.");
        } else {
            Bytes bytes;
            bytes.reserve(proof.size() * 16);
            for (const auto& hash : proof)
                bytes.insert(bytes.end(), hash.begin(), hash.end());
            const auto temporary = folder / ("incoming-" + newTaskId());
            writeNew(temporary, bytes, control);
            try {
                transfer_files::publishFile(temporary, target);
                syncDirectory(folder);
            } catch (...) {
                QFile::remove(qpath(temporary));
                throw;
            }
        }
        proofIdentity = fileIdentity(target);
    }
};
Checkpoint::Checkpoint(const Task& task, const std::filesystem::path& work, Ed2kFileLink link,
                       TransferControl& control)
    : impl_(std::make_unique<Impl>(task, work / "ed2k", std::move(link), control)) {
    auto& value = *impl_;
    for (auto parent = value.folder.parent_path(); !parent.empty(); parent = parent.parent_path()) {
        if (std::filesystem::exists(parent))
            regular(parent, true);
        if (parent == parent.root_path())
            break;
    }
    if (std::filesystem::exists(value.folder))
        regular(value.folder, true);
    else
        std::filesystem::create_directories(value.folder);
    value.done.assign(static_cast<std::size_t>((value.link.size + ed2kPartBytes - 1) / ed2kPartBytes), false);
    value.route = publicRouteKey(task);
    const auto manifest = value.folder / "identity.json";
    if (std::filesystem::exists(manifest)) {
        const auto object = readManifest(manifest);
        if (object.value("version") != 2 || object.value("task").toString().toStdString() != task.id ||
            object.value("owner").toString().toStdString() != task.ownerId ||
            object.value("hash").toString().toStdString() != value.link.hash ||
            object.value("size").toString() != QString::number(static_cast<qulonglong>(value.link.size)))
            throw std::runtime_error(
                "ED2K recovery belongs to a different task or format. Existing data was preserved.");
        const auto bitmap = object.value("verified").toString().toLatin1();
        if (bitmap.size() != static_cast<qsizetype>(2 * ((value.done.size() + 7) / 8)) ||
            QByteArray::fromHex(bitmap).toHex() != bitmap)
            throw std::runtime_error("Invalid ED2K recovery bitmap.");
        value.payloadIdentity = object.value("payload").toString().toStdString();
        value.proofIdentity = object.value("proof").toString().toStdString();
        if (value.payloadIdentity.size() > 128 || value.proofIdentity.size() > 128)
            throw std::runtime_error("Invalid ED2K recovery file identity.");
        if (object.value("destination").toString().toStdString() ==
            destinationIdentity(task.spec.destination))
            value.stageIdentity = object.value("stage").toString().toStdString();
        if (value.stageIdentity.size() > 128)
            throw std::runtime_error("Invalid ED2K stage identity.");
    } else if (!std::filesystem::is_empty(value.folder)) {
        throw std::runtime_error("ED2K recovery contains unrecognized data. Existing files were preserved.");
    }
    value.client = loadClientIdentity(task, work);
    if (std::filesystem::exists(value.folder / "hashset.bin")) {
        if (!value.proofIdentity.empty() && fileIdentity(value.folder / "hashset.bin") != value.proofIdentity)
            throw std::runtime_error("ED2K proof file was replaced. Existing files were preserved.");
        value.proof = value.readProof();
        value.proofIdentity = fileIdentity(value.folder / "hashset.bin");
    } else if (!value.proofIdentity.empty()) {
        value.proofIdentity.clear(); // Recover missing proof from link or peer; do not trust the bitmap.
    }
    if (!value.link.partHashes.empty()) {
        std::vector<Hash> hashes;
        for (const auto& item : value.link.partHashes)
            hashes.push_back(parseHash(item));
        if (fileHash(value.link.size, hashes) != parseHash(value.link.hash) ||
            (!value.proof.empty() && hashes != value.proof))
            throw std::runtime_error("ED2K link part proofs disagree with the requested identity.");
        value.proof = std::move(hashes);
    }
    if (value.link.size < ed2kPartBytes) {
        value.proof = {parseHash(value.link.hash)};
        fileHash(value.link.size, value.proof);
    }
    value.save();
    const auto payload = value.folder / "payload.bin";
    if (!std::filesystem::exists(payload)) {
        if (!value.payloadIdentity.empty())
            throw std::runtime_error("ED2K recovery data is missing. Existing checkpoint was preserved.");
        QFile file(qpath(payload));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            throw std::runtime_error("Cannot create ED2K recovery payload.");
#ifdef _WIN32
        DWORD returned = 0;
        DeviceIoControl(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())), FSCTL_SET_SPARSE, nullptr, 0,
                        nullptr, 0, &returned, nullptr);
#endif
        sync(file);
        value.payloadIdentity = fileIdentity(file);
        file.close();
    } else if (value.payloadIdentity.empty()) {
        // An interrupted initial creation may leave only an empty payload in this owned namespace.
        regular(payload);
        if (std::filesystem::file_size(payload) != 0)
            throw std::runtime_error("Unrecognized ED2K payload was preserved.");
        value.payloadIdentity = fileIdentity(payload);
    }
    value.storeProof();
    value.save();
    // Recompute every complete candidate part, including data written just before a crash.
    value.rehash();
    value.save();
}
Checkpoint::~Checkpoint() = default;
const Ed2kFileLink& Checkpoint::link() const { return impl_->link; }
const Hash& Checkpoint::identity() const { return impl_->client; }
const std::vector<Hash>& Checkpoint::hashes() const { return impl_->proof; }
void Checkpoint::setHashes(std::vector<Hash> hashes) {
    if (fileHash(impl_->link.size, hashes) != parseHash(impl_->link.hash) ||
        (!impl_->proof.empty() && impl_->proof != hashes))
        throw std::runtime_error("ED2K peer hashset disagrees with the file identity.");
    if (impl_->proof == hashes)
        return;
    impl_->proof = std::move(hashes);
    impl_->storeProof();
    impl_->rehash();
    impl_->save();
}
std::size_t Checkpoint::partCount() const { return impl_->done.size(); }
std::size_t Checkpoint::partSize(std::size_t index) const { return impl_->size(index); }
bool Checkpoint::verified(std::size_t index) const { return impl_->done.at(index); }
std::uint64_t Checkpoint::verifiedBytes() const {
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < impl_->done.size(); ++i)
        if (impl_->done[i])
            bytes += impl_->size(i);
    return bytes;
}
void Checkpoint::commitPart(std::size_t index, std::span<const std::uint8_t> bytes) {
    auto& value = *impl_;
    if (value.proof.empty() || bytes.size() != value.size(index) || hashPart(bytes) != value.proof.at(index))
        throw std::invalid_argument("ED2K received part failed integrity verification.");
    if (value.done.at(index))
        return;
    QFile file;
    value.openPayload(file, QIODevice::ReadWrite);
    if (!file.seek(static_cast<qint64>(index * ed2kPartBytes)))
        throw std::runtime_error("Cannot seek ED2K recovery output.");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        checkStop(value.control);
        const auto count =
            file.write(reinterpret_cast<const char*>(bytes.data() + offset),
                       static_cast<qint64>(std::min<std::size_t>(128 * 1024, bytes.size() - offset)));
        if (count <= 0)
            throw std::runtime_error("Cannot write ED2K recovery data. Check disk space.");
        offset += static_cast<std::size_t>(count);
    }
    sync(file);
    file.close();
    value.done[index] = true;
    value.save();
}
Bytes Checkpoint::readPart(std::size_t index) const {
    if (!verified(index))
        throw std::runtime_error("ED2K part has not been verified.");
    QFile file;
    impl_->openPayload(file, QIODevice::ReadOnly);
    if (!file.seek(static_cast<qint64>(index * ed2kPartBytes)))
        throw std::runtime_error("Cannot seek ED2K part.");
    Bytes data(impl_->size(index));
    std::size_t offset = 0;
    while (offset < data.size()) {
        checkStop(impl_->control);
        const auto count =
            file.read(reinterpret_cast<char*>(data.data() + offset),
                      static_cast<qint64>(std::min<std::size_t>(128 * 1024, data.size() - offset)));
        if (count <= 0)
            throw std::runtime_error("ED2K recovery data changed before assembly.");
        offset += static_cast<std::size_t>(count);
    }
    if (hashPart(data) != impl_->proof.at(index))
        throw std::runtime_error("ED2K verified recovery data changed.");
    return data;
}
std::filesystem::path Checkpoint::assembleStage() {
    auto& value = *impl_;
    if (!std::all_of(value.done.begin(), value.done.end(), [](bool done) { return done; }))
        throw std::runtime_error("ED2K cannot assemble unverified data.");
    const auto destination = value.task.spec.destination;
    for (auto parent = destination.parent_path(); !parent.empty(); parent = parent.parent_path()) {
        if (std::filesystem::exists(parent))
            regular(parent, true);
        if (parent == parent.root_path())
            break;
    }
    std::filesystem::create_directories(destination.parent_path());
    if (std::filesystem::exists(destination))
        throw std::runtime_error("ED2K destination already exists. Existing files were preserved.");
    const auto stage = stagePath(value.task);
    if (std::filesystem::exists(stage)) {
        if (value.stageIdentity.empty() || fileIdentity(stage) != value.stageIdentity)
            throw std::runtime_error(
                "ED2K stage is not owned by this checkpoint. Existing files were preserved.");
        if (!std::filesystem::remove(stage))
            throw std::runtime_error("Cannot reset owned ED2K stage.");
    }
    QFile output(qpath(stage));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot create ED2K stage.");
    try {
        value.stageIdentity = fileIdentity(output);
        value.save();
        QFile input;
        value.openPayload(input, QIODevice::ReadOnly);
        std::array<char, 128 * 1024> buffer;
        for (std::size_t part = 0; part < partCount(); ++part) {
            QCryptographicHash digest(QCryptographicHash::Md4);
            auto remaining = value.size(part);
            while (remaining) {
                checkStop(value.control);
                const auto count =
                    input.read(buffer.data(), static_cast<qint64>(std::min(remaining, buffer.size())));
                if (count <= 0)
                    throw std::runtime_error("ED2K recovery changed during assembly.");
                digest.addData(QByteArrayView(buffer.data(), count));
                if (output.write(buffer.data(), count) != count)
                    throw std::runtime_error("Cannot assemble ED2K file. Check disk space.");
                remaining -= static_cast<std::size_t>(count);
            }
            if (digest.result().toHex().toStdString() != hashText(value.proof.at(part)))
                throw std::runtime_error("ED2K recovery part changed during assembly.");
        }
        if (!input.atEnd())
            throw std::runtime_error("ED2K recovery data exceeds the file identity.");
        sync(output);
        output.close();
        return stage;
    } catch (...) {
        output.close();
        cleanupStage();
        throw;
    }
}
void Checkpoint::cleanupStage() noexcept {
    try {
        const auto path = stagePath(impl_->task);
        if (!impl_->stageIdentity.empty() && std::filesystem::exists(path) &&
            fileIdentity(path) == impl_->stageIdentity)
            std::filesystem::remove(path);
    } catch (...) {
    } // Ownership could not be proved: preserve the file.
}
void Checkpoint::discard(const Task& task, const std::filesystem::path& work) {
    const auto folder = work / "ed2k";
    if (!std::filesystem::exists(folder))
        return;
    regular(folder, true);
    const auto object = readManifest(folder / "identity.json");
    const auto link = parseEd2kFileLink(task.spec.url);
    if (object.value("version") != 2 || object.value("task").toString().toStdString() != task.id ||
        object.value("owner").toString().toStdString() != task.ownerId ||
        object.value("hash").toString().toStdString() != link.hash ||
        object.value("size").toString() != QString::number(static_cast<qulonglong>(link.size)))
        throw std::runtime_error("Foreign ED2K recovery data was preserved.");
    const auto removeOwned = [](const std::filesystem::path& path, const std::string& expected) {
        if (!std::filesystem::exists(path))
            return;
        if (expected.empty() || expected.size() > 128 || fileIdentity(path) != expected)
            throw std::runtime_error("Changed ED2K recovery data was preserved.");
        if (!std::filesystem::remove(path))
            throw std::runtime_error("Cannot remove owned ED2K recovery data.");
    };
    const auto stage = stagePath(task);
    if (std::filesystem::exists(stage)) {
        if (object.value("destination").toString().toStdString() !=
            destinationIdentity(task.spec.destination))
            throw std::runtime_error("Unrecognized ED2K stage was preserved.");
        removeOwned(stage, object.value("stage").toString().toStdString());
    }
    // Native file identities make cleanup constant-sized, without rereading a large download on the service
    // actor.
    removeOwned(folder / "payload.bin", object.value("payload").toString().toStdString());
    removeOwned(folder / "hashset.bin", object.value("proof").toString().toStdString());
    const auto entries =
        QDir(qpath(folder)).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    if (entries == QStringList{"identity.json"}) {
        std::filesystem::remove(folder / "identity.json");
        std::filesystem::remove(folder);
    }
}
} // namespace maskpilot::ed2k
