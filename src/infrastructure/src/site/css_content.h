#pragma once
#include <functional>
#include <maskpilot/application/site_content.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace maskpilot::site {
using RewriteReference = std::function<std::string(std::string_view, SiteContentKind)>;
class CssSyntaxError : public std::invalid_argument {
  public:
    using std::invalid_argument::invalid_argument;
};
std::string sanitizeCss(std::string_view source, const RewriteReference& rewrite, std::size_t maxOutputBytes);
} // namespace maskpilot::site
