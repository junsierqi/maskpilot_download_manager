#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maskpilot {
enum class SiteContentKind { Html, Css, Image, Font, Media, Unsupported };
struct SiteContentLimits {
    std::size_t maxInputBytes = 8 * 1024 * 1024;
    std::size_t maxOutputBytes = 16 * 1024 * 1024;
    std::size_t maxNodes = 100000;
    std::size_t maxDepth = 128;
    std::size_t maxReferences = 2000;
};
struct SiteContentInput {
    std::string url, contentType, body;
};
struct SiteContentReference {
    std::string url;
    SiteContentKind kind = SiteContentKind::Unsupported;
};
struct SiteContentResult {
    std::string body;
    std::vector<SiteContentReference> references;
};
// Return a generated relative export path, or nullopt when a resource is unavailable.
using SiteContentResolver = std::function<std::optional<std::string>(const SiteContentReference&)>;
class ISiteContentProcessor {
  public:
    virtual ~ISiteContentProcessor() = default;
    virtual SiteContentResult process(const SiteContentInput&, const SiteContentLimits&,
                                      const SiteContentResolver& = {}) const = 0;
};
SiteContentKind classifySiteContent(std::string_view contentType);
std::string canonicalSiteUrl(std::string_view url, std::string_view base = {});
bool sameSiteOrigin(std::string_view left, std::string_view right);
} // namespace maskpilot
