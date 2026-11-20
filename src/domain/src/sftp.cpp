#include <algorithm>
#include <maskpilot/domain/sftp.h>
#include <system_error>

namespace maskpilot {
bool isSftpUrl(std::string_view url) { return url.starts_with("sftp://"); }
std::string canonicalSftpHostKey(std::string_view value) {
    if (value.starts_with("SHA256:"))
        value.remove_prefix(7);
    if (value.size() == 44 && value.back() == '=')
        value.remove_suffix(1);
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (value.size() != 43 ||
        !std::all_of(value.begin(), value.end(), [&](char c) { return alphabet.find(c) != alphabet.npos; }) ||
        alphabet.find(value.back()) % 4 != 0)
        return {};
    return std::string(value) + '=';
}
std::string validateSftpOptions(const SftpOptions& options, bool source, bool requireCredentials) {
    if (!source) {
        if (options.authentication != SftpAuthentication::Password || !options.hostKeySha256.empty() ||
            !options.privateKeyFile.empty() || !options.username.empty() || !options.password.empty() ||
            !options.keyPassphrase.empty())
            return "SSH options require an SFTP source.";
        return {};
    }
    if (options.authentication != SftpAuthentication::Password &&
        options.authentication != SftpAuthentication::PrivateKey)
        return "Invalid SFTP authentication mode.";
    if (canonicalSftpHostKey(options.hostKeySha256).empty())
        return "SFTP requires the trusted server's SHA256 public-key fingerprint (Base64, optionally "
               "SHA256:).";
    if (options.username.size() > 1024 || options.password.size() > 16384 ||
        options.keyPassphrase.size() > 16384)
        return "SFTP credentials exceed the supported length.";
    for (const auto* text : {&options.username, &options.password, &options.keyPassphrase})
        if (text->find_first_of("\r\n\0", 0, 3) != text->npos)
            return "SFTP credentials cannot contain control characters.";
    if (options.authentication == SftpAuthentication::Password) {
        if (!options.privateKeyFile.empty() || !options.keyPassphrase.empty())
            return "Choose one SFTP authentication method; key options require private-key mode.";
    } else {
        if (!options.password.empty())
            return "Password and private-key authentication cannot be combined.";
        try {
            const auto bytes = options.privateKeyFile.u8string();
            if (!options.privateKeyFile.is_absolute() || options.privateKeyFile.filename().empty() ||
                bytes.size() > 32768 || bytes.starts_with(u8"//") || bytes.starts_with(u8"\\\\") ||
                std::any_of(bytes.begin(), bytes.end(), [](char8_t c) { return c < 0x20 || c == 0x7f; }))
                return "Choose an absolute local SFTP private-key file.";
        } catch (const std::system_error&) {
            return "SFTP key path contains invalid Unicode.";
        }
    }
    if (requireCredentials && options.username.empty())
        return "SFTP login is required. Reenter the username and password or key passphrase before resuming.";
    return {};
}
} // namespace maskpilot
