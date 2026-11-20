#include <algorithm>
#include <chrono>
#include <iomanip>
#include <maskpilot/domain/proxy.h>
#include <maskpilot/domain/task.h>
#include <random>
#include <sstream>
#include <string_view>
#include <system_error>

namespace maskpilot {
bool validHttpResponseContentType(std::string_view value) {
    return value.size() <= 1024 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= 0x20 && ch <= 0x7e; });
}
bool hasOriginMaterial(const DownloadSpec& spec) {
    return !spec.cookie.empty() || !spec.authorization.empty() || !spec.headers.empty() ||
           !spec.ftpUsername.empty() || !spec.ftpPassword.empty() || !spec.sftp.username.empty() ||
           !spec.sftp.password.empty() || !spec.sftp.keyPassphrase.empty() ||
           !spec.sftp.privateKeyFile.empty();
}
bool requiresOriginAuthentication(const DownloadSpec& spec) {
    return isSftpUrl(spec.url) || spec.originAuthenticationRequired || hasOriginMaterial(spec);
}
std::string validateOriginAuthentication(const DownloadSpec& spec, bool requireMaterial) {
    if (!requireMaterial || !requiresOriginAuthentication(spec))
        return {};
    if (spec.kind == TransferKind::BitTorrent || isTorrentUrl(spec.url))
        return "Origin authentication is unsupported for torrent tasks. Choose a public source "
               "explicitly.";
    if (isSftpUrl(spec.url))
        return validateSftpOptions(spec.sftp, true, true);
    if (isFtpUrl(spec.url)) {
        if (spec.ftpUsername.empty())
            return "FTP authentication is required. Reenter the FTP username and password before resuming.";
    } else if (spec.cookie.empty() && spec.authorization.empty() && spec.headers.empty()) {
        if (spec.kind == TransferKind::Media)
            return "Origin authentication is required for media. Reenter its source credentials.";
        return "Source authentication is required. Reenter the URL-bound request credentials or explicitly "
               "allow anonymous access.";
    }
    return {};
}
std::vector<Segment> splitSegments(std::int64_t length, int count) {
    if (length < 0 || count < 1 || count > 32)
        return {};
    if (length == 0)
        return {{0, 0}};
    count = static_cast<int>(std::min<std::int64_t>(count, length));
    std::vector<Segment> result;
    auto base = length / count, remainder = length % count;
    std::int64_t offset = 0;
    for (int i = 0; i < count; ++i) {
        auto end = offset + base + (i < remainder ? 1 : 0);
        result.push_back({offset, end});
        offset = end;
    }
    return result;
}
bool isFtpUrl(const std::string& url) { return url.starts_with("ftp://") || url.starts_with("ftps://"); }
bool isTorrentUrl(const std::string& url) {
    return url.starts_with("magnet:?") || url.starts_with("torrent:");
}
TransferKind sourceKind(const DownloadSpec& spec) {
    if (spec.kind == TransferKind::Media)
        return TransferKind::Media;
    if (isTorrentUrl(spec.url))
        return TransferKind::BitTorrent;
    if (isSftpUrl(spec.url))
        return TransferKind::Sftp;
    return isFtpUrl(spec.url) ? TransferKind::Ftp : TransferKind::Http;
}
std::string validateSpec(const DownloadSpec& spec) {
    if (auto error = validateHttpRequest(spec.httpRequest); !error.empty())
        return error;
    if (spec.httpRequest.method == HttpMethod::Post &&
        (sourceKind(spec) != TransferKind::Http ||
         !(spec.url.starts_with("http://") || spec.url.starts_with("https://"))))
        return "POST requires a native HTTP or HTTPS file source.";
    if (spec.maxStoredBytes < 0 || spec.maxStoredBytes > 9007199254740991LL)
        return "Stored file size limit is outside the supported range.";
    if ((spec.maxStoredBytes != 0 || spec.sameOriginRedirectOnly) &&
        (sourceKind(spec) != TransferKind::Http ||
         !(spec.url.starts_with("http://") || spec.url.starts_with("https://"))))
        return "Stored size and redirect constraints require an HTTP source.";
    if (auto error = validateMediaSelection(spec.mediaSelection, spec.audioOnly); !error.empty())
        return error;
    if (spec.kind != TransferKind::Media && (spec.mediaSelection.mode != MediaSelectionMode::Automatic ||
                                             spec.mediaSelection.container != MediaContainer::Automatic))
        return "Explicit media selection requires media mode.";
    const bool ftp = isFtpUrl(spec.url);
    const bool sftp = isSftpUrl(spec.url);
    if (auto error = validateSftpOptions(spec.sftp, sftp); !error.empty())
        return error;
    if (spec.kind == TransferKind::Sftp && !sftp)
        return "SFTP tasks require an sftp:// source.";
    if (sftp && (spec.kind == TransferKind::Media || !spec.cookie.empty() || !spec.authorization.empty() ||
                 !spec.headers.empty() || !spec.referrer.empty()))
        return "SFTP cannot use media mode or HTTP request credentials and headers.";
    const bool torrent = isTorrentUrl(spec.url);
    if (static_cast<int>(spec.kind) < 0 || static_cast<int>(spec.kind) > 4)
        return "Invalid transfer kind.";
    if (spec.kind == TransferKind::BitTorrent && !torrent)
        return "BitTorrent tasks require a magnet link or local torrent source.";
    if (torrent) {
        if (spec.kind == TransferKind::Media || !spec.cookie.empty() || !spec.authorization.empty() ||
            !spec.headers.empty() || !spec.referrer.empty() || !spec.expectedSha256.empty())
            return "BitTorrent uses verified pieces, not HTTP credentials, media mode or a single file "
                   "digest.";
        if (spec.torrent.uploadLimit < 0 || spec.torrent.uploadLimit > 2147483647LL ||
            spec.torrent.selectedFiles.size() > maxTorrentFiles)
            return "Invalid torrent limits or file selection.";
        auto selection = spec.torrent.selectedFiles;
        std::sort(selection.begin(), selection.end());
        if ((!selection.empty() &&
             (selection.front() < 0 || selection.back() >= static_cast<int>(maxTorrentFiles))) ||
            std::adjacent_find(selection.begin(), selection.end()) != selection.end() ||
            (spec.torrent.allFiles && !selection.empty()))
            return "Torrent file selection must contain unique valid file indices.";
        if (spec.torrent.metadata &&
            (spec.torrent.metadata->empty() || spec.torrent.metadata->size() > maxTorrentMetadataBytes))
            return "Torrent metadata is empty or exceeds 16 MiB.";
        if (spec.url.starts_with("torrent:") && !spec.torrent.metadata) {
            try {
                const auto text = spec.url.substr(8);
                const std::filesystem::path path(std::u8string(text.begin(), text.end()));
                if (!path.is_absolute() || path.filename().empty() || text.starts_with("\\\\") ||
                    text.starts_with("//"))
                    return "Choose an absolute local torrent file path.";
#ifdef _WIN32
                const auto root = path.root_name().native();
                if (root.size() != 2 || root[1] != L':' ||
                    !((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')))
                    return "Copy the torrent metadata to a local drive before importing it.";
#endif
            } catch (const std::system_error&) {
                return "Invalid local torrent source path.";
            }
        }
    } else if (spec.torrent.metadata || !spec.torrent.allFiles || !spec.torrent.selectedFiles.empty() ||
               spec.torrent.seedAfterDownload || spec.torrent.uploadLimit != 0)
        return "Torrent options require a torrent source.";
    if (ftp && ((spec.kind == maskpilot::TransferKind::Media) || !spec.cookie.empty() ||
                !spec.authorization.empty() || !spec.headers.empty() || !spec.referrer.empty()))
        return "FTP tasks cannot use media mode or HTTP request credentials and headers.";
    if (!ftp && (!spec.ftpUsername.empty() || !spec.ftpPassword.empty() || spec.ftpExplicitTls ||
                 spec.ftpActiveMode || !spec.ftpCaBundle.empty()))
        return "FTP options require an FTP or FTPS address.";
    if (spec.ftpUsername.size() > 1024 || spec.ftpPassword.size() > 16384)
        return "FTP credentials are too long.";
    for (const auto& value : {spec.ftpUsername, spec.ftpPassword})
        if (value.find_first_of("\r\n\0", 0, 3) != std::string::npos)
            return "FTP credentials cannot contain control characters.";
    try {
        const auto ca = spec.ftpCaBundle.u8string();
        if (std::any_of(ca.begin(), ca.end(), [](char8_t c) { return c < 0x20 || c == 0x7f; }))
            return "Invalid FTP CA bundle path.";
    } catch (const std::system_error&) {
        return "FTP CA bundle path contains invalid Unicode.";
    }
    if (spec.mediaFormat.size() > 1000 || spec.mediaFormat.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        return "Invalid media format selection.";
    if (!(spec.url.starts_with("https://") || spec.url.starts_with("http://") || ftp || sftp || torrent))
        return "Use an HTTP, HTTPS, FTP, FTPS, SFTP, magnet or local torrent source.";
    if (spec.url.size() > 32768 || spec.url.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        return "Invalid URL.";
    if (!torrent) {
        auto hostBegin = spec.url.find("://") + 3;
        auto authorityEnd = spec.url.find_first_of("/?#", hostBegin);
        auto authority = spec.url.substr(
            hostBegin, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - hostBegin);
        if (authority.empty() || authority.find('@') != std::string::npos)
            return "URL host is missing or contains credentials.";
    }
    if (spec.destination.empty() || spec.destination.filename().empty())
        return "Choose a destination file.";
    std::u8string destinationBytes;
    try {
        destinationBytes = spec.destination.u8string();
    } catch (const std::system_error&) {
        return "Destination path contains invalid Unicode.";
    }
    if (std::any_of(destinationBytes.begin(), destinationBytes.end(),
                    [](char8_t value) { return value < 0x20 || value == 0x7f; }))
        return "Destination paths cannot contain control characters.";
    auto filenameBytes = spec.destination.filename().u8string();
    std::string filename(filenameBytes.begin(), filenameBytes.end());
    if (filename.find_first_of("<>:\"/\\|?*") != std::string::npos || filename.back() == '.' ||
        filename.back() == ' ')
        return "Destination filename contains unsupported characters.";
    auto device = filename.substr(0, filename.find('.'));
    std::transform(device.begin(), device.end(), device.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const bool numberedDevice =
        (device.starts_with("COM") || device.starts_with("LPT")) &&
        (device.substr(3) == "\xC2\xB9" || device.substr(3) == "\xC2\xB2" || device.substr(3) == "\xC2\xB3");
    if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" || device == "CONIN$" ||
        device == "CONOUT$" || numberedDevice ||
        (device.size() == 4 && (device.starts_with("COM") || device.starts_with("LPT")) && device[3] >= '1' &&
         device[3] <= '9'))
        return "Destination uses a reserved Windows filename.";
    if (spec.connections < 1 || spec.connections > 8)
        return "Connections must be between 1 and 8.";
    if (spec.speedLimit < 0 || spec.maxRetries < 0 || spec.maxRetries > 10)
        return "Invalid limits.";
    if (const auto error = validateProxy(spec); !error.empty())
        return error;
    if (!spec.expectedSha256.empty() && (spec.expectedSha256.size() != 64 ||
                                         !std::all_of(spec.expectedSha256.begin(), spec.expectedSha256.end(),
                                                      [](unsigned char c) { return std::isxdigit(c); })))
        return "SHA-256 must contain 64 hexadecimal characters.";
    for (const auto& value : {spec.referrer, spec.userAgent, spec.cookie, spec.authorization, spec.proxy}) {
        if (value.find_first_of("\r\n\0", 0, 3) != std::string::npos)
            return "Request values cannot contain control characters.";
    }
    for (const auto& [name, value] : spec.headers) {
        const auto validName = std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
        });
        if (name.empty() || !validName || value.find_first_of("\r\n\0", 0, 3) != std::string::npos)
            return "Invalid request header.";
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "range" || lower == "if-range" || lower == "host" || lower == "accept-encoding" ||
            lower == "content-length" || lower == "transfer-encoding" || lower == "proxy-authorization" ||
            lower == "proxy-connection" ||
            (spec.httpRequest.method == HttpMethod::Post && lower == "content-type"))
            return "This header is controlled by the download engine.";
    }
    return {};
}
bool isActive(TaskState state) {
    return state == TaskState::Probing || state == TaskState::Downloading || state == TaskState::Pausing ||
           state == TaskState::Verifying || state == TaskState::Finalizing ||
           state == TaskState::Cancelling || state == TaskState::FetchingMetadata ||
           state == TaskState::Seeding;
}
bool isTerminal(TaskState state) { return state == TaskState::Completed || state == TaskState::Cancelled; }
const char* stateName(TaskState state) {
    static constexpr const char* names[] = {
        "Queued",  "Scheduled",     "Probing",          "Downloading",     "Pausing",
        "Paused",  "Retry waiting", "Verifying",        "Finalizing",      "Completed",
        "Failed",  "Cancelling",    "Cancelled",        "Needs attention", "Fetching metadata",
        "Seeding", "Select files",  "Waiting for queue"};
    auto index = static_cast<int>(state);
    return index >= 0 && index < static_cast<int>(std::size(names)) ? names[index] : "Invalid";
}
std::int64_t unixTime() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string newTaskId() {
    std::random_device rd;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i)
        out << std::setw(8) << static_cast<std::uint32_t>(rd());
    return out.str();
}
} // namespace maskpilot
