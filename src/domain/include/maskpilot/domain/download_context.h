#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace maskpilot {
struct DownloadContext {
    std::string id, externalKey, label;
    std::filesystem::path downloadRoot;
    std::string proxyEndpoint;
    bool proxyAuthenticationRequired = false;
    std::string proxyCredentialRef;
    bool originAuthenticationRequired = false;
    std::int64_t revision = 1;
    bool enabled = true;
};
struct CredentialRecord {
    std::string reference, ownerId, proxyEndpoint;
    std::string ownershipNonce;
};
bool isOpaqueId(const std::string& value);
std::string validateDownloadContext(const DownloadContext& context);
} // namespace maskpilot
