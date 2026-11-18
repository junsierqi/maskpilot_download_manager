#include <maskpilot/domain/proxy.h>
#include <maskpilot/domain/task.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string_view>

namespace maskpilot {
namespace {
bool ipv4(std::string_view host) {
    int components = 0;
    while (!host.empty()) {
        const auto dot = host.find('.');
        const auto part = host.substr(0, dot);
        unsigned number = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), number);
        if (part.empty() || part.size() > 3 || error != std::errc{} || end != part.data() + part.size() ||
            number > 255)
            return false;
        ++components;
        if (dot == std::string_view::npos)
            return components == 4;
        host.remove_prefix(dot + 1);
    }
    return false;
}
bool ipv6(std::string_view host) {
    const auto compressed = host.find("::");
    if (host.empty() ||
        (compressed != std::string_view::npos && host.find("::", compressed + 2) != std::string_view::npos))
        return false;
    int groups = 0;
    std::size_t cursor = 0;
    while (cursor < host.size()) {
        if (host[cursor] == ':') {
            if (cursor != compressed && cursor != compressed + 1)
                return false;
            ++cursor;
            continue;
        }
        const auto end = host.find(':', cursor);
        const auto group = host.substr(cursor, end == std::string_view::npos ? end : end - cursor);
        if (group.find('.') != std::string_view::npos) {
            if (end != std::string_view::npos || !ipv4(group))
                return false;
            groups += 2;
        } else {
            if (group.size() > 4 || !std::all_of(group.begin(), group.end(),
                                                 [](unsigned char c) { return std::isxdigit(c) != 0; }))
                return false;
            ++groups;
        }
        if (end == std::string_view::npos)
            break;
        cursor = end;
        if (cursor != compressed)
            ++cursor;
        if (cursor == host.size())
            return false;
    }
    return compressed == std::string_view::npos ? groups == 8 : groups < 8;
}
bool dnsHost(std::string_view host) {
    if (host.empty() || host.size() > 253)
        return false;
    if (host.back() == '.')
        host.remove_suffix(1);
    if (host.empty())
        return false;
    if (std::all_of(host.begin(), host.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; }))
        return ipv4(host);
    while (!host.empty()) {
        const auto dot = host.find('.');
        const auto label = host.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            !std::all_of(label.begin(), label.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
            }))
            return false;
        if (dot == std::string_view::npos)
            return true;
        host.remove_prefix(dot + 1);
    }
    return false;
}
} // namespace
ProxyEndpoint parseProxyEndpoint(const std::string& value) {
    ProxyEndpoint result;
    if (value.empty())
        return result;
    if (value.size() > 2048 ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
        throw std::invalid_argument("Proxy endpoint contains invalid characters.");
    const auto separator = value.find("://");
    auto scheme = value.substr(0, separator);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (separator == std::string::npos)
        throw std::invalid_argument("Proxy requires an HTTP, HTTPS or SOCKS5 scheme.");
    if (scheme == "http") {
        result.type = ProxyType::Http;
        result.port = 80;
    } else if (scheme == "https") {
        result.type = ProxyType::Https;
        result.port = 443;
    } else if (scheme == "socks5" || scheme == "socks5h") {
        result.type = ProxyType::Socks5;
        result.port = 1080;
    } else
        throw std::invalid_argument("Proxy requires an HTTP, HTTPS or SOCKS5 scheme.");
    std::string_view authority(value.data() + separator + 3, value.size() - separator - 3);
    if (authority.empty() || authority.find_first_of("@/?#\\") != std::string_view::npos)
        throw std::invalid_argument(
            "Proxy endpoint must contain only a host and optional port; use separate credentials.");
    std::string_view host, port;
    bool hasPort = false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos)
            throw std::invalid_argument("Proxy IPv6 host must be bracketed.");
        host = authority.substr(1, close - 1);
        if (!ipv6(host))
            throw std::invalid_argument("Proxy IPv6 host is invalid.");
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':')
                throw std::invalid_argument("Proxy port is invalid.");
            hasPort = true;
            port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (!dnsHost(host))
            throw std::invalid_argument("Proxy host is invalid; IPv6 addresses require brackets.");
        if (colon != std::string_view::npos) {
            hasPort = true;
            port = authority.substr(colon + 1);
        }
    }
    if (hasPort) {
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), result.port);
        if (port.empty() || error != std::errc{} || end != port.data() + port.size() || result.port < 1 ||
            result.port > 65535)
            throw std::invalid_argument("Proxy port must be between 1 and 65535.");
    }
    result.host = host;
    std::transform(result.host.begin(), result.host.end(), result.host.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}
std::string validateProxy(const DownloadSpec& spec, bool requireCredentials) {
    try {
        const auto endpoint = parseProxyEndpoint(spec.proxy);
        const bool credentials = !spec.proxyUsername.empty() || !spec.proxyPassword.empty();
        if (endpoint.type == ProxyType::Direct) {
            if (credentials || spec.proxyAuthenticationRequired)
                return "Proxy credentials require a configured proxy endpoint.";
            return {};
        }
        if (((requireCredentials && spec.proxyAuthenticationRequired) || credentials) &&
            (spec.proxyUsername.empty() || spec.proxyPassword.empty()))
            return "Proxy authentication requires a username and password. Reenter process-local "
                   "credentials.";
        const auto maximum = endpoint.type == ProxyType::Socks5 ? 255u : 4096u;
        for (const auto* secret : {&spec.proxyUsername, &spec.proxyPassword})
            if (secret->size() > maximum || std::any_of(secret->begin(), secret->end(), [](unsigned char c) {
                    return c < 0x20 || c == 0x7f;
                }))
                return "Proxy credentials contain unsupported characters or exceed the supported length.";
        if (spec.proxyUsername.find(':') != std::string::npos)
            return "Proxy username cannot contain a colon.";
        if (isFtpUrl(spec.url) && spec.ftpActiveMode)
            return "Active FTP cannot be used through a proxy. Select passive mode.";
        return {};
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
}
} // namespace maskpilot
