#include "curl_runtime.h"
#include <curl/curl.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace maskpilot::network {
void initializeCurl() {
    static const std::string issue = []() -> std::string {
#ifdef _WIN32
        // Adding SSH crypto must not change the established Windows TLS trust policy.
        if (curl_global_sslset(CURLSSLBACKEND_SCHANNEL, nullptr, nullptr) != CURLSSLSET_OK)
            return "The network component must provide the Windows Schannel TLS backend.";
#endif
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            return "Cannot initialize native networking.";
        const auto* version = curl_version_info(CURLVERSION_NOW);
#ifdef __APPLE__
        if (version->version_num < 0x081600)
            return "The macOS network runtime requires version 8.22 or newer for IPv6 proxy routing.";
#endif
        for (const auto* protocol = version->protocols; protocol && *protocol; ++protocol)
            if (std::string_view(*protocol) == "sftp")
                return {};
        return "The network runtime lacks SFTP support. Deploy the matching network libraries.";
    }();
    if (!issue.empty())
        throw std::runtime_error(issue);
    // All owned connections are released by their workers; the global reference lives until process exit.
}
} // namespace maskpilot::network
