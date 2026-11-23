#include <maskpilot/domain/proxy.h>
#include <maskpilot/domain/task.h>
#include <maskpilot/infrastructure/credential_vault.h>

#ifdef __linux__
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <libsecret/secret.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#endif
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <memory>
#ifdef __APPLE__
#include "macos_keychain.h"
#endif
#ifdef _WIN32
#include <Windows.h>
#include <wincred.h>
#endif

namespace maskpilot {
namespace {
bool opaqueId(const std::string& value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}
[[noreturn]] void invalid() {
    throw CredentialVaultError(CredentialVaultFailure::InvalidInput, "Invalid credential vault input");
}
QString text(const std::string& value) {
    const auto result = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    if (result.toUtf8().toStdString() != value)
        invalid();
    return result;
}
std::string route(const ProxyCredentialBinding& binding) {
    if (binding.ownerId != "standalone" && !opaqueId(binding.ownerId))
        invalid();
    if (!binding.ownershipNonce.empty() && !opaqueId(binding.ownershipNonce))
        invalid();
    ProxyEndpoint endpoint;
    try {
        endpoint = parseProxyEndpoint(binding.proxyEndpoint);
    } catch (const std::invalid_argument&) {
        invalid();
    }
    if (endpoint.type == ProxyType::Direct)
        invalid();
    // parseProxyEndpoint validates ASCII hosts; use an unambiguous structured binding.
    std::transform(endpoint.host.begin(), endpoint.host.end(), endpoint.host.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    return std::to_string(static_cast<int>(endpoint.type)) + "/" + endpoint.host + "/" +
           std::to_string(endpoint.port);
}
#ifdef _WIN32
[[noreturn]] void nativeFailure(DWORD error) {
    if (error == ERROR_ACCESS_DENIED)
        throw CredentialVaultError(CredentialVaultFailure::AccessDenied, "Credential vault access denied");
    if (error == ERROR_NO_SUCH_LOGON_SESSION || error == ERROR_NOT_SUPPORTED)
        throw CredentialVaultError(CredentialVaultFailure::Unavailable,
                                   "Credential vault is unavailable for this logon session");
    throw CredentialVaultError(CredentialVaultFailure::Storage, "Credential vault operation failed");
}
struct NativeCredentialDeleter {
    void operator()(CREDENTIALW* value) const {
        if (value) {
            if (value->CredentialBlob)
                SecureZeroMemory(value->CredentialBlob, value->CredentialBlobSize);
            CredFree(value);
        }
    }
};
using NativeCredential = std::unique_ptr<CREDENTIALW, NativeCredentialDeleter>;
class NativeWriteLock {
    HANDLE handle_ = nullptr;

  public:
    explicit NativeWriteLock(const std::wstring& name) {
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle_)
            nativeFailure(GetLastError());
        const auto result = WaitForSingleObject(handle_, 5000);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
            CloseHandle(handle_);
            handle_ = nullptr;
            throw CredentialVaultError(CredentialVaultFailure::Storage,
                                       "Credential vault write lock is unavailable");
        }
    }
    ~NativeWriteLock() {
        if (handle_) {
            ReleaseMutex(handle_);
            CloseHandle(handle_);
        }
    }
    NativeWriteLock(const NativeWriteLock&) = delete;
    NativeWriteLock& operator=(const NativeWriteLock&) = delete;
};

class NativeVault final : public ICredentialVault {
  public:
    explicit NativeVault(std::string installation) : installation_(std::move(installation)) {}

    std::string remember(const ProxyCredentialBinding& binding,
                         const ProxyCredentials& credentials) override {
        const auto reference = QUuid::createUuid().toString(QUuid::Id128).toStdString();
        store(reference, binding, credentials);
        return reference;
    }
    void store(const std::string& reference, const ProxyCredentialBinding& binding,
               const ProxyCredentials& credentials) override {
        const auto canonical = route(binding);
        auto targetName = target(reference);
        DownloadSpec spec;
        spec.proxy = binding.proxyEndpoint;
        spec.proxyUsername = credentials.username;
        spec.proxyPassword = credentials.password;
        spec.proxyAuthenticationRequired = true;
        if (!validateProxy(spec, true).empty())
            invalid();
        const NativeWriteLock writeLock(
            text("Local\\MaskPilot.DownloadManager.Write/" + installation_ + "/" + reference).toStdWString());
        PCREDENTIALW existing = nullptr;
        if (CredReadW(targetName.c_str(), CRED_TYPE_GENERIC, 0, &existing)) {
            const NativeCredential owned(existing);
            throw CredentialVaultError(CredentialVaultFailure::AlreadyExists,
                                       "Credential reference already exists");
        }
        if (const auto error = GetLastError(); error != ERROR_NOT_FOUND)
            nativeFailure(error);
        QJsonObject object{{"version", 1},
                           {"owner", text(binding.ownerId)},
                           {"route", text(canonical)},
                           {"nonce", text(binding.ownershipNonce)},
                           {"username", text(credentials.username)},
                           {"password", text(credentials.password)}};
        auto blob = QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (blob.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
            SecureZeroMemory(blob.data(), static_cast<SIZE_T>(blob.size()));
            invalid();
        }
        CREDENTIALW native{};
        native.Type = CRED_TYPE_GENERIC;
        native.TargetName = targetName.data();
        native.CredentialBlobSize = static_cast<DWORD>(blob.size());
        native.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
        native.Persist = CRED_PERSIST_LOCAL_MACHINE;
        const bool written = CredWriteW(&native, 0) != FALSE;
        const auto error = written ? ERROR_SUCCESS : GetLastError();
        SecureZeroMemory(blob.data(), static_cast<SIZE_T>(blob.size()));
        if (!written)
            nativeFailure(error);
    }
    std::optional<ProxyCredentials> recall(const ProxyCredentialBinding& binding,
                                           const std::string& reference) override {
        auto native = read(binding, reference);
        if (!native)
            return std::nullopt;
        const auto object = decode(*native);
        return ProxyCredentials{object["username"].toString().toUtf8().toStdString(),
                                object["password"].toString().toUtf8().toStdString()};
    }
    bool forget(const ProxyCredentialBinding& binding, const std::string& reference) override {
        auto native = read(binding, reference);
        if (!native)
            return false;
        auto targetName = target(reference);
        if (!CredDeleteW(targetName.c_str(), CRED_TYPE_GENERIC, 0)) {
            const auto error = GetLastError();
            if (error == ERROR_NOT_FOUND)
                return false;
            nativeFailure(error);
        }
        return true;
    }

  private:
    std::string installation_;
    std::wstring target(const std::string& reference) const {
        if (!opaqueId(reference))
            invalid();
        return text("MaskPilot.DownloadManager/v1/" + installation_ + "/" + reference).toStdWString();
    }
    static QJsonObject decode(const CREDENTIALW& credential) {
        if (credential.CredentialBlobSize == 0 ||
            credential.CredentialBlobSize > CRED_MAX_CREDENTIAL_BLOB_SIZE || !credential.CredentialBlob)
            throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid credential vault entry");
        const auto document = QJsonDocument::fromJson(
            QByteArray::fromRawData(reinterpret_cast<const char*>(credential.CredentialBlob),
                                    static_cast<qsizetype>(credential.CredentialBlobSize)));
        const auto object = document.object();
        if (!document.isObject() || object["version"].toInt() != 1 || !object["owner"].isString() ||
            !object["route"].isString() || !object["username"].isString() || !object["password"].isString())
            throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid credential vault entry");
        if (object.contains("nonce") &&
            (!object["nonce"].isString() ||
             (!object["nonce"].toString().isEmpty() && !opaqueId(object["nonce"].toString().toStdString()))))
            throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid credential vault entry");
        return object;
    }
    NativeCredential read(const ProxyCredentialBinding& binding, const std::string& reference) const {
        const auto canonical = route(binding);
        const auto targetName = target(reference);
        PCREDENTIALW result = nullptr;
        if (!CredReadW(targetName.c_str(), CRED_TYPE_GENERIC, 0, &result)) {
            const auto error = GetLastError();
            if (error == ERROR_NOT_FOUND)
                return {};
            nativeFailure(error);
        }
        NativeCredential owned(result);
        const auto object = decode(*owned);
        if (object["owner"].toString() != text(binding.ownerId) ||
            object["route"].toString() != text(canonical) ||
            object["nonce"].toString() != text(binding.ownershipNonce))
            throw CredentialVaultError(CredentialVaultFailure::Binding,
                                       "Credential vault binding does not match");
        DownloadSpec spec;
        spec.proxy = binding.proxyEndpoint;
        spec.proxyUsername = object["username"].toString().toUtf8().toStdString();
        spec.proxyPassword = object["password"].toString().toUtf8().toStdString();
        spec.proxyAuthenticationRequired = true;
        if (!validateProxy(spec, true).empty())
            throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid credential vault entry");
        return owned;
    }
};
#elif defined(__linux__)
constexpr qsizetype secretLimit = 2560;
const SecretSchema schema = [] {
    SecretSchema value{};
    value.name = "org.maskpilot.DownloadManager.Credentials.v1";
    value.flags = SECRET_SCHEMA_NONE;
    value.attributes[0] = {"installation", SECRET_SCHEMA_ATTRIBUTE_STRING};
    value.attributes[1] = {"reference", SECRET_SCHEMA_ATTRIBUTE_STRING};
    return value;
}();
struct SecretRequest {
    GCancellable* cancel = g_cancellable_new();
    GError* error = nullptr;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread deadline{[this](std::stop_token stop) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, stop, std::chrono::seconds(5), [] { return false; });
        if (!stop.stop_requested())
            g_cancellable_cancel(cancel);
    }};
    ~SecretRequest() {
        deadline.request_stop();
        wake.notify_all();
        deadline.join();
        if (error)
            g_error_free(error);
        g_object_unref(cancel);
    }
    void check() const {
        if (error) {
            if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ||
                g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED))
                throw CredentialVaultError(CredentialVaultFailure::AccessDenied, "System secret service access denied");
            throw CredentialVaultError(CredentialVaultFailure::Unavailable,
                                       "System secret service is unavailable, locked, or timed out");
        }
    }
};
struct ItemRelease {
    void operator()(SecretItem* item) const {
        if (item)
            g_object_unref(item);
    }
};
using SecretItemHandle = std::unique_ptr<SecretItem, ItemRelease>;
class LinuxWriteLock {
    QLockFile lock_;
    static QString lockPath(const std::string& installation, const std::string& reference) {
        const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (runtime.isEmpty())
            throw CredentialVaultError(CredentialVaultFailure::Unavailable,
                                       "Private runtime directory is unavailable");
        const auto directory = QDir(runtime).filePath("maskpilot-vault");
        const auto bytes = directory.toUtf8();
        if (mkdir(bytes.constData(), 0700) != 0 && errno != EEXIST)
            throw CredentialVaultError(CredentialVaultFailure::Storage,
                                       "Cannot create private vault lock directory");
        struct stat status{};
        if (lstat(bytes.constData(), &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != getuid() ||
            (status.st_mode & 077) != 0)
            throw CredentialVaultError(CredentialVaultFailure::Storage,
                                       "Vault lock directory has unsafe ownership");
        return QDir(directory).filePath(text(installation + "-" + reference + ".lock"));
    }

  public:
    LinuxWriteLock(const std::string& installation, const std::string& reference)
        : lock_(lockPath(installation, reference)) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!lock_.tryLock(0)) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw CredentialVaultError(CredentialVaultFailure::Storage, "Credential reference is busy");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};
class NativeVault final : public ICredentialVault {
    std::string installation_;
    void validReference(const std::string& reference) const {
        if (!opaqueId(reference))
            invalid();
    }
    SecretItemHandle find(const std::string& reference, SecretRequest& request, bool load) const {
        validReference(reference);
        auto* service = secret_service_get_sync(SECRET_SERVICE_NONE, request.cancel, &request.error);
        request.check();
        if (!service)
            throw CredentialVaultError(CredentialVaultFailure::Unavailable,
                                       "System secret service is unavailable");
        auto* attributes = secret_attributes_build(&schema, "installation", installation_.c_str(),
                                                   "reference", reference.c_str(), nullptr);
        const auto flags = static_cast<SecretSearchFlags>(
            SECRET_SEARCH_ALL | (load ? SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS : 0));
        auto* items =
            secret_service_search_sync(service, &schema, attributes, flags, request.cancel, &request.error);
        g_hash_table_unref(attributes);
        g_object_unref(service);
        if (request.error) {
            g_list_free_full(items, g_object_unref);
            request.check();
        }
        if (items && items->next) {
            g_list_free_full(items, g_object_unref);
            throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Credential reference is ambiguous");
        }
        SecretItemHandle result(items ? SECRET_ITEM(g_object_ref(items->data)) : nullptr);
        g_list_free_full(items, g_object_unref);
        return result;
    }
    QJsonObject decode(SecretItem* item, const ProxyCredentialBinding& binding) const {
        const auto canonical = route(binding);
        auto* value = secret_item_get_secret(item);
        if (!value)
            throw CredentialVaultError(CredentialVaultFailure::Unavailable, "Credential item is locked");
        gsize length = 0;
        const auto* bytes = secret_value_get(value, &length);
        if (!bytes || length == 0 || length > static_cast<gsize>(secretLimit)) {
            secret_value_unref(value);
            throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid credential vault entry");
        }
        const auto document =
            QJsonDocument::fromJson(QByteArray::fromRawData(bytes, static_cast<qsizetype>(length)));
        secret_value_unref(value);
        const auto object = document.object();
        if (!document.isObject() || object["version"].toInt() != 1 || !object["owner"].isString() ||
            !object["route"].isString() || !object["nonce"].isString() || !object["username"].isString() ||
            !object["password"].isString() ||
            (!object["nonce"].toString().isEmpty() && !opaqueId(object["nonce"].toString().toStdString())))
            throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid credential vault entry");
        if (object["owner"].toString() != text(binding.ownerId) ||
            object["route"].toString() != text(canonical) ||
            object["nonce"].toString() != text(binding.ownershipNonce))
            throw CredentialVaultError(CredentialVaultFailure::Binding,
                                       "Credential vault binding does not match");
        DownloadSpec spec;
        spec.proxy = binding.proxyEndpoint;
        spec.proxyUsername = object["username"].toString().toUtf8().toStdString();
        spec.proxyPassword = object["password"].toString().toUtf8().toStdString();
        spec.proxyAuthenticationRequired = true;
        if (!validateProxy(spec, true).empty())
            throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid credential vault entry");
        return object;
    }

  public:
    explicit NativeVault(std::string installation) : installation_(std::move(installation)) {}
    std::string remember(const ProxyCredentialBinding& binding,
                         const ProxyCredentials& credentials) override {
        const auto reference = QUuid::createUuid().toString(QUuid::Id128).toStdString();
        store(reference, binding, credentials);
        return reference;
    }
    void store(const std::string& reference, const ProxyCredentialBinding& binding,
               const ProxyCredentials& credentials) override {
        validReference(reference);
        const auto canonical = route(binding);
        DownloadSpec spec;
        spec.proxy = binding.proxyEndpoint;
        spec.proxyUsername = credentials.username;
        spec.proxyPassword = credentials.password;
        spec.proxyAuthenticationRequired = true;
        if (!validateProxy(spec, true).empty())
            invalid();
        auto blob = QJsonDocument(QJsonObject{{"version", 1},
                                              {"owner", text(binding.ownerId)},
                                              {"route", text(canonical)},
                                              {"nonce", text(binding.ownershipNonce)},
                                              {"username", text(credentials.username)},
                                              {"password", text(credentials.password)}})
                        .toJson(QJsonDocument::Compact);
        struct Wipe {
            QByteArray& bytes;
            ~Wipe() { bytes.fill('\0'); }
        } wipe{blob};
        if (blob.size() > secretLimit) {
            blob.fill('\0');
            invalid();
        }
        const LinuxWriteLock lock(installation_, reference);
        SecretRequest request;
        if (find(reference, request, false)) {
            blob.fill('\0');
            throw CredentialVaultError(CredentialVaultFailure::AlreadyExists,
                                       "Credential reference already exists");
        }
        const auto stored =
            secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, "MaskPilot proxy credentials",
                                       blob.constData(), request.cancel, &request.error, "installation",
                                       installation_.c_str(), "reference", reference.c_str(), nullptr);
        blob.fill('\0');
        request.check();
        if (!stored)
            throw CredentialVaultError(CredentialVaultFailure::Storage,
                                       "Cannot store credentials in system secret service");
    }
    std::optional<ProxyCredentials> recall(const ProxyCredentialBinding& binding,
                                           const std::string& reference) override {
        route(binding);
        validReference(reference);
        const LinuxWriteLock lock(installation_, reference);
        SecretRequest request;
        auto item = find(reference, request, true);
        if (!item)
            return {};
        const auto object = decode(item.get(), binding);
        return ProxyCredentials{object["username"].toString().toUtf8().toStdString(),
                                object["password"].toString().toUtf8().toStdString()};
    }
    bool forget(const ProxyCredentialBinding& binding, const std::string& reference) override {
        route(binding);
        validReference(reference);
        const LinuxWriteLock lock(installation_, reference);
        SecretRequest request;
        auto item = find(reference, request, true);
        if (!item)
            return false;
        decode(item.get(), binding);
        const auto deleted = secret_item_delete_sync(item.get(), request.cancel, &request.error);
        request.check();
        if (!deleted)
            throw CredentialVaultError(CredentialVaultFailure::Storage,
                                       "Cannot remove owned credential item");
        return true;
    }
};
#elif defined(__APPLE__)
class NativeVault final : public ICredentialVault {
    std::string installation_;
    QJsonObject decode(const QByteArray& bytes, const ProxyCredentialBinding& binding) const {
        const auto document = QJsonDocument::fromJson(bytes);
        const auto object = document.object();
        if (!document.isObject() || object["version"].toInt() != 1 || !object["owner"].isString() ||
            !object["route"].isString() || !object["nonce"].isString() || !object["username"].isString() ||
            !object["password"].isString() ||
            (!object["nonce"].toString().isEmpty() && !opaqueId(object["nonce"].toString().toStdString())))
            throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid keychain entry");
        if (object["owner"].toString() != text(binding.ownerId) ||
            object["route"].toString() != text(route(binding)) ||
            object["nonce"].toString() != text(binding.ownershipNonce))
            throw CredentialVaultError(CredentialVaultFailure::Binding,
                                       "Credential vault binding does not match");
        DownloadSpec spec;
        spec.proxy = binding.proxyEndpoint;
        spec.proxyUsername = object["username"].toString().toStdString();
        spec.proxyPassword = object["password"].toString().toStdString();
        spec.proxyAuthenticationRequired = true;
        if (!validateProxy(spec, true).empty())
            throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid keychain entry");
        return object;
    }

  public:
    explicit NativeVault(std::string installation) : installation_(std::move(installation)) {}
    std::string remember(const ProxyCredentialBinding& binding,
                         const ProxyCredentials& credentials) override {
        const auto reference = QUuid::createUuid().toString(QUuid::Id128).toStdString();
        store(reference, binding, credentials);
        return reference;
    }
    void store(const std::string& reference, const ProxyCredentialBinding& binding,
               const ProxyCredentials& credentials) override {
        const auto canonical = route(binding);
        DownloadSpec spec;
        spec.proxy = binding.proxyEndpoint;
        spec.proxyUsername = credentials.username;
        spec.proxyPassword = credentials.password;
        spec.proxyAuthenticationRequired = true;
        if (!validateProxy(spec, true).empty())
            invalid();
        auto bytes = QJsonDocument(QJsonObject{{"version", 1},
                                               {"owner", text(binding.ownerId)},
                                               {"route", text(canonical)},
                                               {"nonce", text(binding.ownershipNonce)},
                                               {"username", text(credentials.username)},
                                               {"password", text(credentials.password)}})
                         .toJson(QJsonDocument::Compact);
        struct Wipe {
            QByteArray& data;
            ~Wipe() { data.fill('\0'); }
        } wipe{bytes};
        const security::MacKeychainLock lock(installation_, reference);
        security::addKeychain(installation_, reference, bytes);
    }
    std::optional<ProxyCredentials> recall(const ProxyCredentialBinding& binding,
                                           const std::string& reference) override {
        route(binding);
        const security::MacKeychainLock lock(installation_, reference);
        auto entry = security::readKeychain(installation_, reference);
        if (!entry)
            return {};
        struct Wipe {
            QByteArray& data;
            ~Wipe() { data.fill('\0'); }
        } wipe{entry->bytes};
        const auto object = decode(entry->bytes, binding);
        return ProxyCredentials{object["username"].toString().toStdString(),
                                object["password"].toString().toStdString()};
    }
    bool forget(const ProxyCredentialBinding& binding, const std::string& reference) override {
        route(binding);
        const security::MacKeychainLock lock(installation_, reference);
        auto entry = security::readKeychain(installation_, reference);
        if (!entry)
            return false;
        struct Wipe {
            QByteArray& data;
            ~Wipe() { data.fill('\0'); }
        } wipe{entry->bytes};
        decode(entry->bytes, binding);
        return security::removeKeychain(*entry);
    }
};
#else
class NativeVault final : public ICredentialVault {
  public:
    explicit NativeVault(const std::string&) {}
    std::string remember(const ProxyCredentialBinding&, const ProxyCredentials&) override { unavailable(); }
    void store(const std::string&, const ProxyCredentialBinding&, const ProxyCredentials&) override {
        unavailable();
    }
    std::optional<ProxyCredentials> recall(const ProxyCredentialBinding&, const std::string&) override {
        unavailable();
    }
    bool forget(const ProxyCredentialBinding&, const std::string&) override { unavailable(); }

  private:
    [[noreturn]] static void unavailable() {
        throw CredentialVaultError(CredentialVaultFailure::Unavailable,
                                   "Native credential vault is not supported on this platform");
    }
};
#endif
} // namespace
std::unique_ptr<ICredentialVault> makeNativeCredentialVault(const std::string& installationId) {
    if (!opaqueId(installationId))
        invalid();
    return std::make_unique<NativeVault>(installationId);
}
} // namespace maskpilot
