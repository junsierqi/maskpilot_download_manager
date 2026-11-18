#include <maskpilot/domain/proxy.h>
#include <maskpilot/domain/task.h>
#include <maskpilot/infrastructure/credential_vault.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>
#include <memory>
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
