#include <algorithm>
#include <array>
#include <charconv>
#include <maskpilot/domain/media_origins.h>
#include <maskpilot/domain/proxy.h>
#include <maskpilot/domain/task.h>
#include <set>
#include <stdexcept>

namespace maskpilot {
namespace {
bool clean(std::string_view text) {
    return std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : static_cast<char>(c);
    });
    return text;
}
// parseProxyEndpoint has already validated the IPv6 syntax, including an IPv4 tail.
std::string ipv6(std::string text) {
    if (text.find('.') != std::string::npos) {
        const auto colon = text.rfind(':');
        const auto tail = text.substr(colon + 1);
        std::array<unsigned, 4> bytes{};
        std::size_t begin = 0;
        for (auto& value : bytes) {
            const auto end = tail.find('.', begin);
            std::from_chars(tail.data() + begin, tail.data() + (end == std::string::npos ? tail.size() : end),
                            value);
            begin = end + 1;
        }
        const auto hex = [](unsigned value) {
            std::array<char, 8> out{};
            auto result = std::to_chars(out.data(), out.data() + out.size(), value, 16);
            return std::string(out.data(), result.ptr);
        };
        text =
            text.substr(0, colon + 1) + hex(bytes[0] * 256 + bytes[1]) + ":" + hex(bytes[2] * 256 + bytes[3]);
    }
    const auto groups = [](std::string_view part) {
        std::vector<std::string> out;
        while (!part.empty()) {
            const auto end = part.find(':');
            const auto group = part.substr(0, end);
            out.push_back(std::string(4 - group.size(), '0') + std::string(group));
            if (end == std::string_view::npos)
                break;
            part.remove_prefix(end + 1);
        }
        return out;
    };
    const auto gap = text.find("::");
    auto result = groups(std::string_view(text).substr(0, gap));
    if (gap != std::string::npos) {
        const auto right = groups(std::string_view(text).substr(gap + 2));
        result.insert(result.end(), 8 - result.size() - right.size(), "0000");
        result.insert(result.end(), right.begin(), right.end());
    }
    std::string out;
    for (const auto& group : result)
        out += (out.empty() ? "" : ":") + group;
    return out;
}
} // namespace
std::string mediaOrigin(std::string_view url, bool originOnly) {
    if (url.size() > 32768 || !clean(url))
        return {};
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos)
        return {};
    const auto scheme = lower(std::string(url.substr(0, schemeEnd)));
    if (scheme != "http" && scheme != "https")
        return {};
    const auto end = url.find_first_of("/?#", schemeEnd + 3);
    if (originOnly && end != std::string_view::npos && url.substr(end) != "/")
        return {};
    try {
        const auto endpoint = parseProxyEndpoint(
            scheme + "://" +
            std::string(
                url.substr(schemeEnd + 3, end == std::string_view::npos ? end : end - schemeEnd - 3)));
        const auto host =
            endpoint.host.find(':') == std::string::npos ? endpoint.host : "[" + ipv6(endpoint.host) + "]";
        return scheme + "://" + host + ":" + std::to_string(endpoint.port);
    } catch (const std::exception&) {
        return {};
    }
}
bool mediaOriginRequiresLogin(const MediaOriginAccess& rule) {
    return rule.authenticationRequired || !rule.cookie.empty() || !rule.authorization.empty() ||
           !rule.headers.empty();
}
std::string validateMediaOrigins(const DownloadSpec& spec, bool requireAccess) {
    if (spec.mediaOrigins.empty())
        return {};
    if (spec.kind != TransferKind::Media && spec.kind != TransferKind::Live)
        return "Additional media origins require media or live recording mode.";
    if (spec.mediaOrigins.size() > 16)
        return "At most 16 additional media origins are supported.";
    const auto primary = mediaOrigin(spec.url);
    if (primary.empty())
        return "Media source requires a valid HTTP or HTTPS origin.";
    std::set<std::string> origins{primary};
    std::size_t bytes = spec.cookie.size() + spec.authorization.size();
    for (const auto& header : spec.headers)
        bytes += header.first.size() + header.second.size();
    for (const auto& rule : spec.mediaOrigins) {
        const auto origin = mediaOrigin(rule.origin, true);
        if (origin.empty() || !origins.insert(origin).second)
            return "Additional media origins must be unique exact HTTP origins without paths or credentials.";
        if (!clean(rule.cookie) || !clean(rule.authorization) || rule.headers.size() > 64)
            return "Invalid additional media origin credentials.";
        bytes += origin.size() + rule.cookie.size() + rule.authorization.size();
        std::set<std::string> names;
        for (const auto& [name, value] : rule.headers) {
            const auto key = lower(name);
            const bool token = !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
            });
            if (!token || !clean(value) || !names.insert(key).second || key == "cookie" ||
                key == "authorization" || key == "host" || key == "range" || key == "if-range" ||
                key == "accept-encoding" || key == "content-length" || key == "transfer-encoding" ||
                key == "connection" || key == "upgrade" || key == "te" || key == "trailer" ||
                key.starts_with("proxy-"))
                return "Invalid or engine-controlled additional media origin header.";
            bytes += name.size() + value.size();
        }
        if (requireAccess && mediaOriginRequiresLogin(rule) && rule.cookie.empty() &&
            rule.authorization.empty() && rule.headers.empty())
            return "Reenter the required additional media origin credentials before continuing.";
    }
    if (bytes > 128 * 1024)
        return "Media origin request material exceeds 128 KiB.";
    if (requireAccess && !spec.mediaOriginsConfirmed)
        return "Review and approve the additional media origins before continuing.";
    return {};
}
void invalidateMediaOrigins(DownloadSpec& spec) {
    for (auto& rule : spec.mediaOrigins) {
        rule.authenticationRequired = mediaOriginRequiresLogin(rule);
        rule.cookie.clear();
        rule.authorization.clear();
        rule.headers.clear();
    }
    spec.mediaOriginsConfirmed = false;
}
std::string encodeMediaOrigins(const std::vector<MediaOriginAccess>& rules) {
    if (rules.size() > 16)
        throw std::invalid_argument("Too many media origins.");
    std::string out;
    std::set<std::string> origins;
    for (const auto& rule : rules) {
        const auto origin = mediaOrigin(rule.origin, true);
        if (origin.empty() || !origins.insert(origin).second)
            throw std::invalid_argument("Invalid public media origin policy.");
        out += (mediaOriginRequiresLogin(rule) ? "1 " : "0 ") + origin + "\n";
    }
    return out;
}
std::vector<MediaOriginAccess> decodeMediaOrigins(std::string_view text) {
    if (text.size() > 32768)
        throw std::invalid_argument("Public media origin policy exceeds its bound.");
    std::vector<MediaOriginAccess> out;
    while (!text.empty()) {
        const auto end = text.find('\n');
        if (end == std::string_view::npos || end < 3 || text[1] != ' ' || (text[0] != '0' && text[0] != '1'))
            throw std::invalid_argument("Invalid public media origin policy.");
        MediaOriginAccess rule;
        rule.authenticationRequired = text[0] == '1';
        rule.origin = text.substr(2, end - 2);
        out.push_back(std::move(rule));
        text.remove_prefix(end + 1);
    }
    (void)encodeMediaOrigins(out);
    return out;
}
} // namespace maskpilot
