#include <algorithm>
#include <chrono>
#include <iomanip>
#include <maskpilot/domain/task.h>
#include <random>
#include <sstream>
#include <string_view>
#include <system_error>

namespace maskpilot {
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
std::string validateSpec(const DownloadSpec& spec) {
    const bool ftp = isFtpUrl(spec.url);
    if (ftp && (spec.media || !spec.cookie.empty() || !spec.authorization.empty() || !spec.headers.empty() ||
                !spec.referrer.empty()))
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
    if (spec.media && (!spec.cookie.empty() || !spec.authorization.empty() || !spec.headers.empty()))
        return "Origin-bound login headers are supported for direct file downloads only. For media, use a "
               "public URL or download the direct file without media mode.";
    if (spec.mediaFormat.size() > 1000 || spec.mediaFormat.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        return "Invalid media format selection.";
    if (!(spec.url.starts_with("https://") || spec.url.starts_with("http://") || ftp))
        return "Only HTTP, HTTPS, FTP and FTPS URLs are supported.";
    if (spec.url.size() > 32768 || spec.url.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        return "Invalid URL.";
    auto hostBegin = spec.url.find("://") + 3;
    auto authorityEnd = spec.url.find_first_of("/?#", hostBegin);
    auto authority = spec.url.substr(hostBegin, authorityEnd == std::string::npos ? std::string::npos
                                                                                  : authorityEnd - hostBegin);
    if (authority.empty() || authority.find('@') != std::string::npos)
        return "URL host is missing or contains credentials.";
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
    if (!spec.proxy.empty()) {
        auto scheme = spec.proxy.find("://");
        if (scheme == std::string::npos)
            return "Proxy must be an HTTP, HTTPS or SOCKS URL.";
        auto host = spec.proxy.substr(scheme + 3);
        host = host.substr(0, host.find_first_of("/?#"));
        if (host.empty() || host.find('@') != std::string::npos)
            return "Proxy credentials cannot be stored in the proxy URL.";
    }
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
            lower == "content-length" || lower == "transfer-encoding")
            return "This header is controlled by the download engine.";
    }
    return {};
}
bool isActive(TaskState state) {
    return state == TaskState::Probing || state == TaskState::Downloading || state == TaskState::Pausing ||
           state == TaskState::Verifying || state == TaskState::Finalizing || state == TaskState::Cancelling;
}
bool isTerminal(TaskState state) { return state == TaskState::Completed || state == TaskState::Cancelled; }
const char* stateName(TaskState state) {
    static constexpr const char* names[] = {
        "Queued",    "Scheduled",  "Probing",   "Downloading", "Pausing",    "Paused",    "Retry waiting",
        "Verifying", "Finalizing", "Completed", "Failed",      "Cancelling", "Cancelled", "Needs attention"};
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
