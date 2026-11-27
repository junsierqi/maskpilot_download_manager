#include <algorithm>
#include <charconv>
#include <limits>
#include <maskpilot/domain/ed2k.h>
#include <maskpilot/domain/torrent.h>
#include <stdexcept>

namespace maskpilot {
namespace {
[[noreturn]] void invalid(const char* message) { throw std::invalid_argument(message); }
std::vector<std::string_view> split(std::string_view text, char delimiter, std::size_t limit = 65536) {
    std::vector<std::string_view> fields;
    while (true) {
        if (fields.size() == limit)
            invalid("Too many ED2K fields.");
        const auto end = text.find(delimiter);
        fields.push_back(text.substr(0, end));
        if (end == text.npos)
            return fields;
        text.remove_prefix(end + 1);
    }
}
int hex(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}
std::string digest(std::string_view value) {
    if (value.size() != 32)
        invalid("ED2K requires a 32-character hexadecimal hash.");
    std::string result(value);
    for (auto& c : result) {
        const auto digit = hex(c);
        if (digit < 0)
            invalid("Invalid ED2K hexadecimal hash.");
        c = "0123456789abcdef"[digit];
    }
    return result;
}
std::uint64_t number(std::string_view value, std::uint64_t maximum) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() || result > maximum)
        invalid("Invalid or unsupported ED2K numeric value.");
    return result;
}
bool validUtf8(std::string_view text) {
    std::uint32_t value = 0, minimum = 0;
    int remaining = 0;
    for (unsigned char c : text) {
        if (remaining) {
            if ((c & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (c & 0x3f);
            if (--remaining == 0 &&
                (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)))
                return false;
        } else if (c < 0x80) {
            continue;
        } else if (c >= 0xc2 && c <= 0xdf) {
            remaining = 1;
            value = c & 0x1f;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            remaining = 2;
            value = c & 0xf;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            remaining = 3;
            value = c & 7;
            minimum = 0x10000;
        } else
            return false;
    }
    return remaining == 0;
}
std::string decode(std::string_view value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%')
            result += value[i];
        else {
            if (i + 2 >= value.size() || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0)
                invalid("Invalid ED2K percent escape.");
            result += static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2]));
            i += 2;
        }
    }
    if (!validUtf8(result))
        invalid("ED2K text must contain valid UTF-8.");
    return result;
}
bool validHost(std::string_view host) {
    if (host.empty() || host.size() > 253)
        return false;
    for (auto label : split(host, '.')) {
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            return false;
        for (auto c : label)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
                return false;
    }
    return true;
}
Ed2kEndpoint endpoint(std::string_view value) {
    const auto parts = split(value, ':');
    if (parts.size() < 2 || parts.size() > 4 || !validHost(parts[0]))
        invalid("Invalid ED2K source; use an IPv4 address or DNS name and port.");
    Ed2kEndpoint result;
    result.host = parts[0];
    result.port = static_cast<std::uint16_t>(number(parts[1], 65535));
    if (!result.port)
        invalid("ED2K ports must be nonzero.");
    if (parts.size() >= 3)
        result.cryptOptions = static_cast<std::uint8_t>(number(parts[2], 255));
    if (parts.size() == 4)
        result.clientHash = digest(parts[3]);
    if (bool(result.cryptOptions & 0x80) != !result.clientHash.empty())
        invalid("ED2K source hash and encryption flags disagree.");
    return result;
}
} // namespace
bool isEd2kUrl(std::string_view url) {
    constexpr std::string_view prefix = "ed2k://";
    if (url.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
        if (c != prefix[i])
            return false;
    }
    return true;
}
Ed2kEndpoint parseEd2kEndpoint(std::string_view text, bool server) {
    if (text.size() > 512)
        invalid("ED2K endpoint is too long.");
    std::string normalized;
    if (server && isEd2kUrl(text)) {
        const auto fields = split(text.substr(7), '|');
        if ((fields.size() != 5 && fields.size() != 6) || !fields[0].empty() || fields[1] != "server" ||
            fields[4] != "/" || (fields.size() == 6 && !fields[5].empty()))
            invalid("Invalid ED2K server link.");
        normalized = std::string(fields[2]) + ":" + std::string(fields[3]);
        text = normalized;
    }
    auto result = endpoint(text);
    if (server && (result.cryptOptions || !result.clientHash.empty()))
        invalid("ED2K servers require a host and port only.");
    std::transform(result.host.begin(), result.host.end(), result.host.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; });
    return result;
}
std::string formatEd2kEndpoint(const Ed2kEndpoint& value) {
    auto result = value.host + ":" + std::to_string(value.port);
    if (value.cryptOptions || !value.clientHash.empty()) {
        result += ":" + std::to_string(value.cryptOptions);
        if (!value.clientHash.empty())
            result += ":" + value.clientHash;
    }
    return result;
}
std::vector<Ed2kEndpoint> parseEd2kEndpointList(std::string_view text, bool servers) {
    if (text.size() > 64 * 1024)
        invalid("ED2K endpoint list is too large.");
    std::vector<Ed2kEndpoint> result;
    std::size_t lines = 0;
    while (!text.empty()) {
        if (++lines > 256)
            invalid("Too many ED2K endpoint lines.");
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        const auto first = line.find_first_not_of(" \t\r");
        if (first != line.npos) {
            line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
            auto item = parseEd2kEndpoint(line, servers);
            if (std::find(result.begin(), result.end(), item) == result.end()) {
                if (result.size() == maxEd2kSources)
                    invalid("Too many ED2K endpoints.");
                result.push_back(std::move(item));
            }
        }
        if (end == text.npos)
            break;
        text.remove_prefix(end + 1);
    }
    return result;
}
std::string formatEd2kEndpointList(const std::vector<Ed2kEndpoint>& endpoints) {
    if (endpoints.size() > maxEd2kSources)
        invalid("Too many ED2K endpoints.");
    std::string result;
    for (const auto& item : endpoints) {
        const auto value = formatEd2kEndpoint(item);
        parseEd2kEndpoint(value);
        if (!result.empty())
            result += '\n';
        result += value;
    }
    return result;
}
std::uint16_t ed2kHashCount(std::uint64_t size) {
    if (size > maxEd2kFileBytes)
        invalid("ED2K file exceeds the supported hashset range.");
    return static_cast<std::uint16_t>(size < ed2kPartBytes ? 1 : size / ed2kPartBytes + 1);
}
Ed2kFileLink parseEd2kFileLink(std::string_view url) {
    if (url.size() > maxEd2kLinkBytes || !isEd2kUrl(url))
        invalid("Invalid or oversized ED2K link.");
    if (std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
        invalid("ED2K links cannot contain raw spaces or control characters.");
    const auto fields = split(url.substr(7), '|', 128);
    if (fields.size() < 6 || !fields[0].empty() || fields[1] != "file")
        invalid("Only ED2K file links are supported.");
    Ed2kFileLink result;
    result.filename = decode(fields[2]);
    if (result.filename.find_first_of("/\\") != result.filename.npos ||
        !validateTorrentPath(result.filename).empty())
        invalid("Unsafe ED2K filename.");
    result.size = number(fields[3], maxEd2kFileBytes);
    result.hash = digest(fields[4]);
    std::size_t index = 5;
    for (; index < fields.size() && fields[index] != "/"; ++index) {
        const auto field = fields[index];
        if (field.starts_with("p=") && result.partHashes.empty()) {
            const auto hashes = split(field.substr(2), ':');
            if (hashes.size() != ed2kHashCount(result.size))
                invalid("ED2K part hash count does not match size.");
            for (auto item : hashes)
                result.partHashes.push_back(digest(item));
        } else if (field.starts_with("h=") && result.aichRoot.empty()) {
            auto hash = field.substr(2);
            if (hash.size() != 32)
                invalid("Invalid ED2K auxiliary hash length.");
            for (char c : hash)
                if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7')))
                    invalid("Invalid ED2K auxiliary Base32 hash.");
            result.aichRoot = hash;
        } else if (field.starts_with("s=") && result.webSeeds.size() < maxEd2kSources) {
            const auto raw = field.substr(2);
            auto seed =
                raw.starts_with("http://") || raw.starts_with("https://") ? std::string(raw) : decode(raw);
            const auto authorityBegin = seed.find("://");
            const auto authority =
                authorityBegin == seed.npos
                    ? std::string{}
                    : seed.substr(authorityBegin + 3,
                                  seed.find_first_of("/?#", authorityBegin + 3) - authorityBegin - 3);
            if (authority.empty() || decode(authority).find('@') != std::string::npos ||
                std::any_of(seed.begin(), seed.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
                invalid("ED2K web sources cannot contain credentials or unsafe characters.");
            if (!(seed.starts_with("http://") || seed.starts_with("https://")) ||
                seed.find_first_of("\r\n\0", 0, 3) != seed.npos || seed.size() > 32768)
                invalid("Invalid ED2K web source.");
            result.webSeeds.push_back(std::move(seed));
        } else
            invalid("Unsupported or duplicate ED2K link option.");
    }
    if (index == fields.size())
        invalid("ED2K link terminator is missing.");
    ++index;
    if (index < fields.size() && !fields[index].empty()) {
        if (!fields[index].starts_with("sources,"))
            invalid("Unsupported ED2K source suffix.");
        for (auto item : split(fields[index].substr(8), ',')) {
            if (result.sources.size() == maxEd2kSources)
                invalid("Too many ED2K sources.");
            const auto source = endpoint(item);
            if (std::find(result.sources.begin(), result.sources.end(), source) == result.sources.end())
                result.sources.push_back(source);
        }
        if (++index >= fields.size() || fields[index] != "/")
            invalid("ED2K source terminator is missing.");
        ++index;
    }
    // Both common final forms (|/ and |/|) are accepted; trailing data is not.
    if (index < fields.size() && fields[index].empty())
        ++index;
    if (index != fields.size())
        invalid("Unexpected data after ED2K link.");
    return result;
}
std::string validateEd2kOptions(const Ed2kOptions& options) {
    if (options.servers.size() > maxEd2kSources || options.sources.size() > maxEd2kSources)
        return "Too many ED2K endpoints.";
    for (const auto* list : {&options.servers, &options.sources})
        for (const auto& item : *list) {
            if (!validHost(item.host) || !item.port)
                return "Invalid ED2K endpoint.";
            if (list == &options.servers && (item.cryptOptions || !item.clientHash.empty()))
                return "ED2K index servers do not accept peer encryption options.";
            try {
                if (!item.clientHash.empty())
                    digest(item.clientHash);
            } catch (const std::invalid_argument&) {
                return "Invalid ED2K source hash.";
            }
            if (bool(item.cryptOptions & 0x80) != !item.clientHash.empty())
                return "ED2K source hash and encryption flags disagree.";
        }
    return {};
}
} // namespace maskpilot
