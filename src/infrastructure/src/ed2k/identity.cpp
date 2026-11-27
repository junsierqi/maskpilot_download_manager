#include "identity.h"
#include "../filesystem/transfer_files.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <algorithm>
#include <maskpilot/domain/proxy.h>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace maskpilot::ed2k {
namespace {
QString qpath(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qsizetype>(bytes.size()));
}
void regular(const std::filesystem::path& path, bool directory) {
    const auto status = std::filesystem::symlink_status(path);
    if (directory ? !std::filesystem::is_directory(status) : !std::filesystem::is_regular_file(status))
        throw std::runtime_error("ED2K identity path is not a regular local file or directory.");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("ED2K identity does not follow reparse points.");
#endif
}
std::string digest(const QByteArray& value) {
    return QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex().toStdString();
}
} // namespace
std::string publicRouteKey(const Task& task) {
    auto endpoint = parseProxyEndpoint(task.spec.proxy);
    std::transform(endpoint.host.begin(), endpoint.host.end(), endpoint.host.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    const auto route = std::to_string(static_cast<int>(endpoint.type)) + ":" + endpoint.host + ":" +
                       std::to_string(endpoint.port);
    auto value = QByteArray::fromStdString(task.ownerId);
    value.append(char(0));
    value += QByteArray::fromStdString(route);
    // Only trusted owner and public route configuration enter this key. Never credentials.
    return digest(value);
}
std::string sessionKey(const Task& task, const std::filesystem::path& work) {
    return digest(qpath(work.parent_path().lexically_normal()).toUtf8() + char(0) +
                  QByteArray::fromStdString(publicRouteKey(task)));
}
Hash loadClientIdentity(const Task& task, const std::filesystem::path& work) {
    const auto folder = work.parent_path() / "ed2k-identities";
    for (auto parent = folder.parent_path(); !parent.empty(); parent = parent.parent_path()) {
        if (std::filesystem::exists(parent))
            regular(parent, true);
        if (parent == parent.root_path())
            break;
    }
    if (std::filesystem::exists(folder))
        regular(folder, true);
    else
        std::filesystem::create_directories(folder);
    const auto key = publicRouteKey(task);
    const auto target = folder / (key + ".json");
    const auto readExisting = [&] {
        regular(target, false);
        QFile file(qpath(target));
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 4096)
            throw std::runtime_error("Invalid ED2K identity record. Existing files were preserved.");
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        const auto object = document.object();
        if (error.error != QJsonParseError::NoError || !document.isObject() || object.size() != 4 ||
            object.value("version") != 1 || object.value("owner").toString().toStdString() != task.ownerId ||
            object.value("route").toString().toStdString() != key)
            throw std::runtime_error(
                "ED2K identity belongs to another owner or route. Existing files were preserved.");
        const auto hash = parseHash(object.value("client").toString().toStdString());
        if (std::all_of(hash.begin(), hash.end(), [](auto byte) { return byte == 0; }))
            throw std::runtime_error("Invalid empty ED2K client identity.");
        return hash;
    };
    if (std::filesystem::exists(target))
        return readExisting();
    Hash identity;
    for (auto& byte : identity)
        byte = static_cast<std::uint8_t>(QRandomGenerator::system()->generate());
    identity[0] |= 1;
    const auto payload = QJsonDocument(QJsonObject{{"version", 1},
                                                   {"owner", QString::fromStdString(task.ownerId)},
                                                   {"route", QString::fromStdString(key)},
                                                   {"client", QString::fromStdString(hashText(identity))}})
                             .toJson(QJsonDocument::Compact);
    const auto temporary = folder / ("identity-" + newTaskId() + ".new");
    QFile file(qpath(temporary));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot exclusively create ED2K identity.");
    try {
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (file.write(payload) != payload.size() || !file.flush())
            throw std::runtime_error("Cannot persist ED2K identity.");
#ifdef _WIN32
        if (!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()))))
#else
        if (fsync(file.handle()) != 0)
#endif
            throw std::runtime_error("Cannot synchronize ED2K identity.");
        file.close();
        try {
            transfer_files::publishFile(temporary, target);
        } catch (...) {
            // Another process may have completed the same exclusive first creation.
            if (!std::filesystem::exists(target))
                throw;
            QFile::remove(qpath(temporary));
            return readExisting();
        }
#ifndef _WIN32
        const auto descriptor = open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (descriptor < 0)
            throw std::runtime_error("Cannot synchronize ED2K identity directory.");
        const auto result = fsync(descriptor);
        close(descriptor);
        if (result != 0)
            throw std::runtime_error("Cannot synchronize ED2K identity directory.");
#endif
        return identity;
    } catch (...) {
        file.close();
        QFile::remove(qpath(temporary));
        throw;
    }
}
} // namespace maskpilot::ed2k
