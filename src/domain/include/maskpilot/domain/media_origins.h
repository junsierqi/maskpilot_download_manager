#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace maskpilot {
struct DownloadSpec;
struct MediaOriginAccess {
    std::string origin;
    std::string cookie, authorization;
    std::vector<std::pair<std::string, std::string>> headers;
    bool authenticationRequired = false;
};
// Exact HTTP origin including the effective port; empty for invalid input.
std::string mediaOrigin(std::string_view url, bool originOnly = false);
bool mediaOriginRequiresLogin(const MediaOriginAccess&);
std::string validateMediaOrigins(const DownloadSpec&, bool requireAccess = false);
void invalidateMediaOrigins(DownloadSpec&);
// Public, bounded representation: never includes credentials or process-local approval.
std::string encodeMediaOrigins(const std::vector<MediaOriginAccess>&);
std::vector<MediaOriginAccess> decodeMediaOrigins(std::string_view);
} // namespace maskpilot
