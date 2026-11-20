#include <algorithm>
#include <maskpilot/domain/http_request.h>
#include <string_view>

namespace maskpilot {
std::string validateHttpRequest(const HttpRequest& request, bool requireBody) {
    if (request.method != HttpMethod::Get && request.method != HttpMethod::Post)
        return "Invalid HTTP request method.";
    if (request.method == HttpMethod::Get) {
        if (!request.body.empty() || request.bodyProvided ||
            request.contentType != "application/x-www-form-urlencoded")
            return "Request body and content type require POST.";
        return {};
    }
    if (request.body.size() > 1024 * 1024)
        return "POST body exceeds 1 MiB.";
    const auto& type = request.contentType;
    const auto end = type.find(';');
    const std::string_view mime(type.data(), end == std::string::npos ? type.size() : end);
    const auto slash = mime.find('/');
    const auto token = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    };
    if (type.empty() || type.size() > 1024 || slash == std::string::npos || slash == 0 ||
        slash + 1 == mime.size() ||
        !std::all_of(mime.begin(), mime.end(), [&](unsigned char c) { return c == '/' || token(c); }) ||
        mime.find('/', slash + 1) != std::string_view::npos ||
        !std::all_of(type.begin(), type.end(), [](unsigned char c) { return c >= 0x20 && c <= 0x7e; }))
        return "POST content type must be a valid printable MIME type.";
    if (requireBody && !request.bodyProvided)
        return "POST requires a new request body and explicit submission confirmation, including an empty "
               "body. A previous submission may already have reached the server.";
    return {};
}
} // namespace maskpilot
