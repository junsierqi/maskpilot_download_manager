#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace maskpilot {
enum class SftpAuthentication { Password, PrivateKey };
struct SftpOptions {
    SftpAuthentication authentication = SftpAuthentication::Password;
    std::string hostKeySha256;
    std::filesystem::path privateKeyFile;
    // Process-local credentials; never persist, export or pass to helper arguments.
    std::string username, password, keyPassphrase;
    bool operator==(const SftpOptions&) const = default;
};
bool isSftpUrl(std::string_view url);
std::string canonicalSftpHostKey(std::string_view value);
std::string validateSftpOptions(const SftpOptions&, bool sftpSource, bool requireCredentials = false);
} // namespace maskpilot
