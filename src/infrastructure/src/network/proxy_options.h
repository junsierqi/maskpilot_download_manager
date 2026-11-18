#pragma once
#include <curl/curl.h>
#include <maskpilot/domain/proxy.h>
#include <maskpilot/domain/task.h>
#include <stdexcept>

namespace maskpilot::network {
inline void configureProxy(CURL* handle, const DownloadSpec& spec) {
    if (const auto error = validateProxy(spec, true); !error.empty())
        throw std::invalid_argument(error);
    const auto endpoint = parseProxyEndpoint(spec.proxy);
    const auto set = [handle](CURLoption option, auto value) {
        if (curl_easy_setopt(handle, option, value) != CURLE_OK)
            throw std::runtime_error("Cannot configure the selected proxy route.");
    };
    // Both policies are explicit: never inherit ambient proxy or bypass settings.
    set(CURLOPT_NOPROXY, "");
    if (endpoint.type == ProxyType::Direct) {
        set(CURLOPT_PROXY, "");
        return;
    }
    const auto host =
        endpoint.host.find(':') == std::string::npos ? endpoint.host : "[" + endpoint.host + "]";
    const auto scheme = endpoint.type == ProxyType::Https    ? "https://"
                        : endpoint.type == ProxyType::Socks5 ? "socks5h://"
                                                             : "http://";
    const auto url = scheme + host + ":" + std::to_string(endpoint.port);
    set(CURLOPT_PROXY, url.c_str());
    set(CURLOPT_PROXYTYPE, endpoint.type == ProxyType::Socks5  ? CURLPROXY_SOCKS5_HOSTNAME
                           : endpoint.type == ProxyType::Https ? CURLPROXY_HTTPS
                                                               : CURLPROXY_HTTP);
    set(CURLOPT_PROXY_SSL_VERIFYPEER, 1L);
    set(CURLOPT_PROXY_SSL_VERIFYHOST, 2L);
    if (!spec.proxyUsername.empty()) {
        set(CURLOPT_PROXYUSERNAME, spec.proxyUsername.c_str());
        set(CURLOPT_PROXYPASSWORD, spec.proxyPassword.c_str());
        set(CURLOPT_PROXYAUTH, static_cast<long>(CURLAUTH_ANY));
    }
}
} // namespace maskpilot::network
