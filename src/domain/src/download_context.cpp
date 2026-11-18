#include <maskpilot/domain/download_context.h>
#include <maskpilot/domain/proxy.h>

#include <algorithm>
#include <stdexcept>
#include <system_error>

namespace maskpilot {
bool isOpaqueId(const std::string& value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}
std::string validateDownloadContext(const DownloadContext& context) {
    if (!isOpaqueId(context.id) || context.revision < 1)
        return "Invalid download context identity or revision.";
    for (const auto* value : {&context.externalKey, &context.label})
        if (value->empty() || value->size() > 256 ||
            std::any_of(value->begin(), value->end(),
                        [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }))
            return "Context key and label must contain between 1 and 256 printable bytes.";
    if (context.downloadRoot.empty() || !context.downloadRoot.is_absolute())
        return "Context download root must be an absolute directory path.";
    try {
        const auto bytes = context.downloadRoot.u8string();
        if (bytes.size() > 32768 ||
            std::any_of(bytes.begin(), bytes.end(), [](char8_t ch) { return ch < 0x20 || ch == 0x7f; }))
            return "Context download root contains unsupported characters.";
        for (const auto& part : context.downloadRoot)
            if (part == "." || part == "..")
                return "Context download root must be normalized.";
    } catch (const std::system_error&) {
        return "Context download root contains invalid Unicode.";
    }
    try {
        const auto proxy = parseProxyEndpoint(context.proxyEndpoint);
        if (proxy.type == ProxyType::Direct &&
            (context.proxyAuthenticationRequired || !context.proxyCredentialRef.empty()))
            return "Context proxy authentication requires a proxy endpoint.";
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    if (!context.proxyCredentialRef.empty() &&
        (!isOpaqueId(context.proxyCredentialRef) || !context.proxyAuthenticationRequired))
        return "Invalid context credential reference.";
    return {};
}
} // namespace maskpilot
