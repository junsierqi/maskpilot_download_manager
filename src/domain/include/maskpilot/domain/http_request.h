#pragma once
#include <string>

namespace maskpilot {
enum class HttpMethod { Get, Post };
struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string contentType = "application/x-www-form-urlencoded";
    // Process-local submission authority, consumed before dispatch. Never persist or export.
    std::string body;
    bool bodyProvided = false;
    bool operator==(const HttpRequest&) const = default;
};
std::string validateHttpRequest(const HttpRequest& request, bool requireBody = false);
} // namespace maskpilot
