#pragma once
#include <cstdint>
#include <string>

namespace maskpilot {
// Source refresh only. Paths, ownership, routing and policy are never browser inputs.
struct BrowserRecoveryMaterial {
    std::string url, cookie, referrer, userAgent;
    bool operator==(const BrowserRecoveryMaterial&) const = default;
};
struct BrowserRecoverySession {
    std::string token, taskId, name, pageUrl;
    std::int64_t expiresAt = 0;
};
} // namespace maskpilot
