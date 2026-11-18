#include "css_content.h"
#include <QChar>
#include <QString>
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <vector>

namespace maskpilot::site {
namespace {
bool space(unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f'; }
bool ident(unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch == '-' || ch >= 0x80; }
void whitespace(std::string_view source, std::size_t& at) {
    while (at < source.size()) {
        if (space(source[at])) {
            ++at;
        } else if (source.substr(at, 2) == "/*") {
            const auto end = source.find("*/", at + 2);
            if (end == std::string_view::npos)
                throw CssSyntaxError("Unterminated CSS comment.");
            at = end + 2;
        } else {
            break;
        }
    }
}
std::string escape(std::string_view source, std::size_t& at) {
    if (++at >= source.size())
        throw CssSyntaxError("Incomplete CSS escape.");
    if (space(source[at])) {
        if (source[at] == '\r' && at + 1 < source.size() && source[at + 1] == '\n')
            ++at;
        ++at;
        return {};
    }
    if (!std::isxdigit(static_cast<unsigned char>(source[at])))
        return std::string(1, source[at++]);
    char32_t value = 0;
    for (int count = 0;
         count < 6 && at < source.size() && std::isxdigit(static_cast<unsigned char>(source[at])); ++count) {
        const auto ch = static_cast<unsigned char>(source[at++]);
        value = value * 16 + (ch <= '9' ? ch - '0' : std::tolower(ch) - 'a' + 10);
    }
    if (at < source.size() && space(source[at])) {
        if (source[at] == '\r' && at + 1 < source.size() && source[at + 1] == '\n')
            ++at;
        ++at;
    }
    if (value == 0 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        value = 0xfffd;
    return QString::fromUcs4(&value, 1).toUtf8().toStdString();
}
std::string identifier(std::string_view source, std::size_t& at) {
    std::string result;
    while (at < source.size()) {
        if (source[at] == '\\')
            result += escape(source, at);
        else if (ident(source[at]))
            result += source[at++];
        else
            break;
    }
    return result;
}
std::string quoted(std::string_view source, std::size_t& at) {
    const auto quote = source[at++];
    std::string value;
    while (at < source.size() && source[at] != quote) {
        if (source[at] == '\\')
            value += escape(source, at);
        else {
            if (source[at] == '\n' || source[at] == '\r' || source[at] == '\0')
                throw CssSyntaxError("Invalid CSS string.");
            value += source[at++];
        }
    }
    if (at == source.size())
        throw CssSyntaxError("Unterminated CSS string.");
    ++at;
    return value;
}
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
std::string cssString(std::string_view value) {
    std::string result = "\"";
    for (unsigned char ch : value) {
        if (ch == '<' || ch == '>') {
            result += ch == '<' ? "\\3c " : "\\3e ";
            continue;
        }
        if (ch == '"' || ch == '\\')
            result += '\\';
        if (ch < 0x20 || ch == 0x7f)
            throw CssSyntaxError("Invalid rewritten CSS reference.");
        result += static_cast<char>(ch);
    }
    return result + '"';
}
std::string cssIdentifier(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char ch : value) {
        if (ident(ch)) {
            result += static_cast<char>(ch);
        } else {
            result += '\\';
            result += digits[ch >> 4];
            result += digits[ch & 15];
            result += ' ';
        }
    }
    return result;
}
struct BoundedCss {
    std::string text;
    std::size_t maximum;
    BoundedCss& operator+=(std::string_view value) {
        if (value.size() > maximum - text.size())
            throw std::length_error("Offline CSS output exceeds the limit.");
        text.append(value);
        return *this;
    }
    BoundedCss& operator+=(char value) { return *this += std::string_view(&value, 1); }
};
} // namespace

std::string sanitizeCss(std::string_view source, const RewriteReference& rewrite,
                        std::size_t maxOutputBytes) {
    static const std::set<std::string> functions = {"rgb",
                                                    "rgba",
                                                    "hsl",
                                                    "hsla",
                                                    "hwb",
                                                    "lab",
                                                    "lch",
                                                    "oklab",
                                                    "oklch",
                                                    "color",
                                                    "calc",
                                                    "min",
                                                    "max",
                                                    "clamp",
                                                    "var",
                                                    "env",
                                                    "linear-gradient",
                                                    "radial-gradient",
                                                    "conic-gradient",
                                                    "repeating-linear-gradient",
                                                    "repeating-radial-gradient",
                                                    "translate",
                                                    "translatex",
                                                    "translatey",
                                                    "translatez",
                                                    "translate3d",
                                                    "scale",
                                                    "scalex",
                                                    "scaley",
                                                    "scale3d",
                                                    "rotate",
                                                    "rotatex",
                                                    "rotatey",
                                                    "rotatez",
                                                    "rotate3d",
                                                    "matrix",
                                                    "matrix3d",
                                                    "skew",
                                                    "skewx",
                                                    "skewy",
                                                    "perspective",
                                                    "cubic-bezier",
                                                    "steps",
                                                    "format",
                                                    "tech",
                                                    "local",
                                                    "not",
                                                    "is",
                                                    "where",
                                                    "has",
                                                    "nth-child",
                                                    "nth-of-type",
                                                    "attr",
                                                    "counter",
                                                    "counters",
                                                    "blur",
                                                    "brightness",
                                                    "contrast",
                                                    "drop-shadow",
                                                    "grayscale",
                                                    "hue-rotate",
                                                    "invert",
                                                    "opacity",
                                                    "saturate",
                                                    "sepia"};
    static const std::set<std::string> rules = {"import",    "charset", "font-face", "media",
                                                "supports",  "layer",   "keyframes", "-webkit-keyframes",
                                                "container", "page"};
    BoundedCss output{{}, maxOutputBytes};
    std::size_t at = 0;
    std::vector<bool> fontBlocks;
    int parentheses = 0;
    bool import = false, nextFontBlock = false;
    while (at < source.size()) {
        if (space(source[at]) || source.substr(at, 2) == "/*") {
            whitespace(source, at);
            output += ' ';
            continue;
        }
        auto ch = source[at];
        if (ch == '"' || ch == '\'') {
            auto text = quoted(source, at);
            if (import) {
                auto local = rewrite(text, SiteContentKind::Css);
                output += "url(" + cssString(local.empty() ? "data:," : local) + ')';
                import = false;
            } else {
                output += cssString(text);
            }
        } else if (ch == '@') {
            ++at;
            const auto name = lower(identifier(source, at));
            if (!rules.contains(name))
                throw CssSyntaxError("Unsupported CSS rule in offline content.");
            import = name == "import";
            nextFontBlock = name == "font-face";
            if (name == "charset") {
                whitespace(source, at);
                if (at == source.size() || (source[at] != '"' && source[at] != '\''))
                    throw CssSyntaxError("Invalid CSS charset directive.");
                const auto encoding = lower(quoted(source, at));
                if (encoding != "utf-8" && encoding != "utf8" && encoding != "us-ascii")
                    throw CssSyntaxError("Unsupported CSS charset directive.");
                whitespace(source, at);
                if (at == source.size() || source[at++] != ';')
                    throw CssSyntaxError("Invalid CSS charset directive.");
            } else {
                output += '@' + name;
            }
        } else if (ident(ch) || ch == '\\') {
            const auto name = identifier(source, at);
            const auto folded = lower(name);
            if (name.find_first_of("<>") != std::string::npos || folded == "expression" ||
                folded == "behavior" || folded == "-moz-binding")
                throw CssSyntaxError("Active CSS syntax is unsupported offline.");
            auto look = at;
            if (folded == "url")
                whitespace(source, look);
            if (look < source.size() && source[look] == '(') {
                at = look + 1;
                if (folded == "url") {
                    whitespace(source, at);
                    std::string text;
                    if (at < source.size() && (source[at] == '"' || source[at] == '\'')) {
                        text = quoted(source, at);
                        whitespace(source, at);
                    } else {
                        while (at < source.size() && source[at] != ')' && !space(source[at])) {
                            if (source[at] == '\\')
                                text += escape(source, at);
                            else if (source[at] == '(' || source[at] == '"' || source[at] == '\'' ||
                                     static_cast<unsigned char>(source[at]) < 0x20)
                                throw CssSyntaxError("Invalid CSS URL.");
                            else
                                text += source[at++];
                        }
                        whitespace(source, at);
                    }
                    if (at == source.size() || source[at++] != ')')
                        throw CssSyntaxError("Unterminated CSS URL.");
                    const auto kind =
                        import ? SiteContentKind::Css
                               : (!fontBlocks.empty() && fontBlocks.back() ? SiteContentKind::Font
                                                                           : SiteContentKind::Image);
                    auto local = rewrite(text, kind);
                    output += "url(" + cssString(local.empty() ? "data:," : local) + ')';
                    import = false;
                } else {
                    if (!functions.contains(folded))
                        throw CssSyntaxError("Unsupported CSS function in offline content.");
                    output += cssIdentifier(name) + '(';
                    if (++parentheses > 128)
                        throw CssSyntaxError("CSS nesting exceeds the limit.");
                }
            } else {
                output += cssIdentifier(name);
            }
        } else {
            ++at;
            if (static_cast<unsigned char>(ch) < 0x20 || ch == '<')
                throw CssSyntaxError("Unsupported CSS token in offline content.");
            if (ch == '{') {
                if (fontBlocks.size() >= 128)
                    throw CssSyntaxError("CSS nesting exceeds the limit.");
                fontBlocks.push_back(nextFontBlock || (!fontBlocks.empty() && fontBlocks.back()));
                nextFontBlock = false;
            } else if (ch == '}') {
                if (fontBlocks.empty())
                    throw CssSyntaxError("Unbalanced CSS block.");
                fontBlocks.pop_back();
            } else if (ch == '(') {
                if (++parentheses > 128)
                    throw std::length_error("CSS nesting exceeds the limit.");
            } else if (ch == ')') {
                if (--parentheses < 0)
                    throw CssSyntaxError("Unbalanced CSS function.");
            } else if (ch == ';') {
                import = false;
            }
            output += ch;
        }
    }
    if (!fontBlocks.empty() || parentheses || import)
        throw CssSyntaxError("Incomplete CSS content.");
    return std::move(output.text);
}
} // namespace maskpilot::site
