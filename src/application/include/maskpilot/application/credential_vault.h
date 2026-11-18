#pragma once
#include <optional>
#include <stdexcept>
#include <string>

namespace maskpilot {
struct ProxyCredentialBinding {
    // "standalone" or a server-issued 32-character lowercase hexadecimal owner ID.
    std::string ownerId;
    std::string proxyEndpoint;
};
struct ProxyCredentials {
    std::string username;
    std::string password;
};
enum class CredentialVaultFailure { InvalidInput, Unavailable, AccessDenied, Storage, Binding, Corrupt };
class CredentialVaultError : public std::runtime_error {
  public:
    CredentialVaultError(CredentialVaultFailure failure, const char* message)
        : std::runtime_error(message), failure_(failure) {}
    CredentialVaultFailure failure() const noexcept { return failure_; }

  private:
    CredentialVaultFailure failure_;
};
class ICredentialVault {
  public:
    virtual ~ICredentialVault() = default;
    // Caller commits the returned opaque reference only after successful native storage.
    virtual std::string remember(const ProxyCredentialBinding&, const ProxyCredentials&) = 0;
    virtual std::optional<ProxyCredentials> recall(const ProxyCredentialBinding&,
                                                   const std::string& reference) = 0;
    // Missing is false; a mismatching binding must never delete the entry.
    virtual bool forget(const ProxyCredentialBinding&, const std::string& reference) = 0;
};
} // namespace maskpilot
