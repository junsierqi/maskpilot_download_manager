#pragma once
#include <string>

namespace maskpilot {
struct DownloadSpec;
enum class ProxyType { Direct, Http, Https, Socks5 };
struct ProxyEndpoint {
    ProxyType type = ProxyType::Direct;
    std::string host;
    int port = 0;
};
// Empty means explicit direct access. Parsed values never contain credentials.
ProxyEndpoint parseProxyEndpoint(const std::string& value);
std::string validateProxy(const DownloadSpec& spec, bool requireCredentials = false);
} // namespace maskpilot
