#include "css_content.h"
#include <QByteArray>
#include <QString>
#include <QStringDecoder>
#include <QUrl>
#include <algorithm>
#include <cctype>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlsave.h>
#include <maskpilot/domain/task.h>
#include <maskpilot/infrastructure/adapters.h>
#include <set>
#include <stdexcept>
#include <utility>

namespace maskpilot {
namespace {
std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}
std::string mediaType(std::string_view text) {
    if (!validHttpResponseContentType(text))
        return {};
    return lower(trim(text.substr(0, text.find(';'))));
}
std::string encoding(std::string_view contentType) {
    std::string result;
    auto at = contentType.find(';');
    while (at != std::string_view::npos) {
        const auto end = contentType.find(';', at + 1);
        const auto part = contentType.substr(at + 1, end == std::string_view::npos ? end : end - at - 1);
        const auto equal = part.find('=');
        if (equal != std::string_view::npos && lower(trim(part.substr(0, equal))) == "charset") {
            auto next = lower(trim(part.substr(equal + 1)));
            if (next.size() >= 2 && next.front() == '"' && next.back() == '"')
                next = next.substr(1, next.size() - 2);
            if (!result.empty() && result != next)
                throw std::invalid_argument("Conflicting offline content encodings.");
            result = std::move(next);
        }
        at = end;
    }
    return result;
}
const char* supportedEncoding(std::string value) {
    value = lower(trim(value));
    if (value.empty())
        return nullptr;
    if (value == "utf-8" || value == "utf8")
        return "UTF-8";
    if (value == "utf-16" || value == "utf16")
        return "UTF-16";
    if (value == "utf-16le")
        return "UTF-16LE";
    if (value == "utf-16be")
        return "UTF-16BE";
    if (value == "iso-8859-1" || value == "latin1" || value == "latin-1")
        return "ISO-8859-1";
    if (value == "ascii" || value == "us-ascii")
        return "US-ASCII";
    throw std::invalid_argument("Unsupported offline content encoding; use UTF-8, UTF-16 or Latin-1.");
}
QUrl parsedUrl(std::string_view value) {
    return QUrl::fromEncoded(QByteArray(value.data(), static_cast<qsizetype>(value.size())),
                             QUrl::StrictMode);
}
std::string text(const xmlChar* value) { return value ? reinterpret_cast<const char*>(value) : ""; }
using XmlText = std::unique_ptr<xmlChar, decltype(xmlFree)>;
std::string attribute(xmlNode* node, const char* name) {
    const XmlText raw(xmlGetProp(node, reinterpret_cast<const xmlChar*>(name)), xmlFree);
    return text(raw.get());
}
void setAttribute(xmlNode* node, const char* name, const std::string& value) {
    if (!xmlSetProp(node, reinterpret_cast<const xmlChar*>(name),
                    reinterpret_cast<const xmlChar*>(value.c_str())))
        throw std::bad_alloc();
}
bool safeRelativePath(std::string_view value) {
    if (value.empty() || value.size() > 512 || value.front() == '/' || value.back() == '/')
        return false;
    std::size_t at = 0;
    while (at < value.size()) {
        const auto end = value.find('/', at);
        const auto component = value.substr(at, end == std::string_view::npos ? end : end - at);
        if (component.empty() || component == "." || component == ".." || component.back() == '.')
            return false;
        if (!std::all_of(component.begin(), component.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
            }))
            return false;
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
    return true;
}
struct Rewrite {
    const SiteContentInput& input;
    const SiteContentLimits& limits;
    const SiteContentResolver& resolver;
    SiteContentResult result;
    std::string base;
    std::set<std::pair<std::string, SiteContentKind>> seen;
    std::size_t cssBytes = 0;
    std::string css(std::string_view value) {
        auto output = site::sanitizeCss(value, std::ref(*this), limits.maxOutputBytes - cssBytes);
        cssBytes += output.size();
        return output;
    }
    std::string operator()(std::string_view value, SiteContentKind kind) {
        const auto clean = trim(value);
        if (clean.empty())
            return {};
        const auto url = canonicalSiteUrl(clean, base);
        if (url.empty() || !sameSiteOrigin(input.url, url))
            return {};
        if (seen.emplace(url, kind).second) {
            if (result.references.size() >= limits.maxReferences)
                throw std::length_error("Offline content reference count exceeds the limit.");
            result.references.push_back({url, kind});
        }
        if (!resolver)
            return {};
        auto path = resolver({url, kind});
        if (!path)
            return {};
        if (!safeRelativePath(*path))
            throw std::runtime_error("Offline resolver returned an unsafe relative path.");
        const auto fragment = parsedUrl(clean).fragment(QUrl::FullyEncoded).toStdString();
        return *path + (fragment.empty() ? "" : '#' + fragment);
    }
};
void validateLimits(const SiteContentInput& input, const SiteContentLimits& limits) {
    if (!limits.maxInputBytes || limits.maxInputBytes > 8 * 1024 * 1024 || !limits.maxOutputBytes ||
        limits.maxOutputBytes > 64 * 1024 * 1024 || !limits.maxNodes || limits.maxNodes > 100000 ||
        !limits.maxDepth || limits.maxDepth > 128 || !limits.maxReferences || limits.maxReferences > 10000 ||
        input.body.size() > limits.maxInputBytes || canonicalSiteUrl(input.url).empty() ||
        !validHttpResponseContentType(input.contentType))
        throw std::invalid_argument("Invalid bounded offline content input or limits.");
}
void validateTree(xmlDoc* document, const SiteContentLimits& limits) {
    std::vector<std::pair<xmlNode*, std::size_t>> pending{{document->children, 1}};
    std::size_t nodes = 0;
    while (!pending.empty()) {
        auto [first, depth] = pending.back();
        pending.pop_back();
        for (auto* node = first; node; node = node->next) {
            if (++nodes > limits.maxNodes || depth > limits.maxDepth)
                throw std::invalid_argument("Offline HTML tree exceeds the node or depth limit.");
            std::size_t attributes = 0;
            for (auto* attr = node->type == XML_ELEMENT_NODE ? node->properties : nullptr; attr;
                 attr = attr->next) {
                if (++attributes > 128)
                    throw std::invalid_argument("Offline HTML attribute count exceeds the limit.");
                auto* value = xmlNodeListGetString(document, attr->children, 1);
                const auto size = value ? xmlStrlen(value) : 0;
                xmlFree(value);
                if (size > 32768)
                    throw std::invalid_argument("Offline HTML attribute exceeds the limit.");
            }
            if (node->children && node->type == XML_ELEMENT_NODE)
                pending.emplace_back(node->children, depth + 1);
        }
    }
}
std::string srcset(std::string_view source, Rewrite& rewrite) {
    if (lower(std::string(source)).find("data:") != std::string::npos)
        return {}; // Data URLs and comma-bearing URL syntax are deliberately unsupported.
    std::string result;
    std::size_t at = 0, count = 0;
    while (at < source.size()) {
        if (++count > 64)
            throw std::invalid_argument("Offline srcset exceeds the candidate limit.");
        const auto end = source.find(',', at);
        const auto item = trim(source.substr(at, end == std::string_view::npos ? end : end - at));
        const auto separator = item.find_first_of(" \t\r\n");
        const auto url = item.substr(0, separator);
        const auto descriptor = separator == std::string::npos ? "" : trim(item.substr(separator));
        const bool valid = descriptor.empty() ||
                           (descriptor.size() <= 16 && descriptor.size() > 1 &&
                            (descriptor.back() == 'w' || descriptor.back() == 'x') &&
                            std::all_of(descriptor.begin(), descriptor.end() - 1,
                                        [](unsigned char ch) { return std::isdigit(ch) || ch == '.'; }));
        if (valid) {
            auto path = rewrite(url, SiteContentKind::Image);
            if (!path.empty())
                result += (result.empty() ? "" : ", ") + path + (descriptor.empty() ? "" : " " + descriptor);
        }
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
    return result;
}
const std::set<std::string> tags = {
    "html",    "head",  "body",       "title",  "style",  "link",       "a",       "abbr",    "address",
    "article", "aside", "b",          "bdi",    "bdo",    "blockquote", "br",      "caption", "cite",
    "code",    "col",   "colgroup",   "dd",     "del",    "details",    "dfn",     "div",     "dl",
    "dt",      "em",    "figcaption", "figure", "footer", "h1",         "h2",      "h3",      "h4",
    "h5",      "h6",    "header",     "hgroup", "hr",     "i",          "img",     "ins",     "kbd",
    "li",      "main",  "mark",       "nav",    "ol",     "p",          "picture", "pre",     "q",
    "rp",      "rt",    "ruby",       "s",      "samp",   "section",    "small",   "source",  "span",
    "strong",  "sub",   "summary",    "sup",    "table",  "tbody",      "td",      "tfoot",   "th",
    "thead",   "time",  "tr",         "u",      "ul",     "var",        "video",   "audio",   "wbr"};
const std::set<std::string> plainAttributes = {
    "id",      "class", "title",   "lang",  "dir",      "alt",   "width",    "height",   "colspan",
    "rowspan", "scope", "headers", "start", "reversed", "value", "datetime", "open",     "controls",
    "muted",   "loop",  "preload", "media", "type",     "sizes", "loading",  "decoding", "role"};
void sanitizeTree(xmlNode* first, Rewrite& rewrite) {
    for (auto* node = first; node;) {
        auto* next = node->next;
        const auto name = lower(text(node->name));
        const bool element = node->type == XML_ELEMENT_NODE;
        if ((element && (node->ns || !tags.contains(name))) || (!element && node->type != XML_TEXT_NODE)) {
            xmlUnlinkNode(node);
            xmlFreeNode(node);
            node = next;
            continue;
        }
        if (element) {
            const auto rel = lower(attribute(node, "rel"));
            if (name == "link" && rel != "stylesheet") {
                xmlUnlinkNode(node);
                xmlFreeNode(node);
                node = next;
                continue;
            }
            for (auto* attr = node->properties; attr;) {
                auto* following = attr->next;
                const auto key = lower(text(attr->name));
                const auto value = attribute(node, key.c_str());
                std::string replacement;
                bool keep = false;
                if (!attr->ns && plainAttributes.contains(key)) {
                    keep = true;
                } else if (!attr->ns && key == "style") {
                    try {
                        replacement = rewrite.css(value);
                    } catch (const site::CssSyntaxError&) {
                        // Unsupported inline CSS is omitted; HTML remains an inert usable page.
                    }
                } else if (!attr->ns && key == "href" && (name == "a" || name == "link")) {
                    replacement = rewrite(value, name == "a" ? SiteContentKind::Html : SiteContentKind::Css);
                } else if (!attr->ns && key == "src" &&
                           (name == "img" || name == "video" || name == "audio" || name == "source")) {
                    const auto image = name == "img" || (name == "source" && node->parent &&
                                                         text(node->parent->name) == "picture");
                    replacement = rewrite(value, image ? SiteContentKind::Image : SiteContentKind::Media);
                } else if (!attr->ns && key == "poster" && name == "video") {
                    replacement = rewrite(value, SiteContentKind::Image);
                } else if (!attr->ns && key == "srcset" && (name == "img" || name == "source")) {
                    replacement = srcset(value, rewrite);
                } else if (!attr->ns && key == "rel" && name == "link") {
                    keep = true;
                }
                if (!replacement.empty())
                    setAttribute(node, key.c_str(), replacement);
                else if (!keep)
                    xmlRemoveProp(attr);
                attr = following;
            }
            if (name == "style") {
                const XmlText content(xmlNodeGetContent(node), xmlFree);
                std::string cleaned;
                try {
                    cleaned = rewrite.css(text(content.get()));
                } catch (const site::CssSyntaxError&) {
                }
                xmlNodeSetContent(node, reinterpret_cast<const xmlChar*>(cleaned.c_str()));
            } else {
                sanitizeTree(node->children, rewrite);
            }
        }
        node = next;
    }
}
struct Output {
    std::string bytes;
    std::size_t maximum;
    bool exceeded = false;
    static int write(void* context, const char* data, int size) {
        auto& output = *static_cast<Output*>(context);
        if (size < 0 || static_cast<std::size_t>(size) > output.maximum - output.bytes.size()) {
            output.exceeded = true;
            return -1;
        }
        try {
            output.bytes.append(data, static_cast<std::size_t>(size));
            return size;
        } catch (...) {
            return -1;
        }
    }
};
void validateAsset(const SiteContentInput& input) {
    const auto& bytes = input.body;
    const auto type = mediaType(input.contentType);
    const auto prefix = [&](std::string_view signature) { return bytes.starts_with(signature); };
    const auto at = [&](std::size_t offset, std::string_view signature) {
        return offset <= bytes.size() && std::string_view(bytes).substr(offset).starts_with(signature);
    };
    bool valid = false;
    if (type == "image/png")
        valid = prefix(std::string_view("\x89PNG\r\n\x1a\n", 8));
    else if (type == "image/jpeg")
        valid = prefix(std::string_view("\xff\xd8\xff", 3));
    else if (type == "image/gif")
        valid = prefix("GIF87a") || prefix("GIF89a");
    else if (type == "image/webp")
        valid = prefix("RIFF") && at(8, "WEBP");
    else if (type == "image/avif")
        valid = at(4, "ftyp") && (at(8, "avif") || at(8, "avis"));
    else if (type == "image/bmp")
        valid = prefix("BM");
    else if (type == "image/x-icon" || type == "image/vnd.microsoft.icon")
        valid = prefix(std::string_view("\0\0\1\0", 4));
    else if (type == "font/woff" || type == "application/font-woff")
        valid = prefix("wOFF");
    else if (type == "font/woff2")
        valid = prefix("wOF2");
    else if (type == "font/ttf")
        valid = prefix(std::string_view("\0\1\0\0", 4)) || prefix("true");
    else if (type == "font/otf")
        valid = prefix("OTTO");
    else if (type == "video/mp4" || type == "audio/mp4")
        valid = at(4, "ftyp") && (at(8, "isom") || at(8, "iso2") || at(8, "mp41") || at(8, "mp42") ||
                                  at(8, "avc1") || at(8, "M4V ") || at(8, "M4A ") || at(8, "dash"));
    else if (type == "video/webm" || type == "audio/webm")
        valid = prefix(std::string_view("\x1a\x45\xdf\xa3", 4));
    else if (type == "video/ogg" || type == "audio/ogg")
        valid = prefix("OggS");
    else if (type == "audio/wav")
        valid = prefix("RIFF") && at(8, "WAVE");
    else if (type == "audio/flac")
        valid = prefix("fLaC");
    else if (type == "audio/mpeg")
        valid = prefix("ID3") || (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
                                  (static_cast<unsigned char>(bytes[1]) & 0xe0) == 0xe0);
    if (!valid)
        throw std::invalid_argument("Offline resource type does not match a supported binary signature.");
}
class SiteContentProcessor final : public ISiteContentProcessor {
  public:
    SiteContentResult process(const SiteContentInput& input, const SiteContentLimits& limits,
                              const SiteContentResolver& resolver) const override {
        validateLimits(input, limits);
        Rewrite rewrite{input, limits, resolver, {}, canonicalSiteUrl(input.url), {}};
        const auto kind = classifySiteContent(input.contentType);
        const auto* charset = supportedEncoding(encoding(input.contentType));
        if (kind == SiteContentKind::Css) {
            const auto decoderName = charset ? charset : "UTF-8";
            QStringDecoder decoder(decoderName);
            if (!decoder.isValid())
                throw std::invalid_argument("Unsupported offline CSS encoding.");
            const QString decoded = decoder(QByteArrayView(input.body.data(), input.body.size()));
            const auto source = decoded.toUtf8();
            if (decoder.hasError())
                throw std::invalid_argument("Invalid offline CSS encoding.");
            rewrite.result.body = rewrite.css(source.toStdString());
        } else if (kind == SiteContentKind::Html) {
            auto context = std::unique_ptr<xmlParserCtxt, decltype(&xmlFreeParserCtxt)>(htmlNewParserCtxt(),
                                                                                        xmlFreeParserCtxt);
            if (!context)
                throw std::bad_alloc();
            xmlCtxtSetResourceLoader(
                context.get(),
                +[](void*, const char*, const char*, xmlResourceType, xmlParserInputFlags,
                    xmlParserInput** output) -> xmlParserErrors {
                    *output = nullptr;
                    return XML_IO_LOAD_ERROR;
                },
                nullptr);
            xmlCtxtSetErrorHandler(context.get(), +[](void*, const xmlError*) {}, nullptr);
            auto document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>(
                htmlCtxtReadMemory(context.get(), input.body.data(), static_cast<int>(input.body.size()),
                                   input.url.c_str(), charset, HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING),
                xmlFreeDoc);
            if (!document)
                throw std::invalid_argument("Cannot parse bounded offline HTML.");
            validateTree(document.get(), limits);
            if (document->encoding)
                supportedEncoding(text(document->encoding));
            bool haveBase = false;
            const std::function<void(xmlNode*)> inspect = [&](xmlNode* first) {
                for (auto* node = first; node; node = node->next) {
                    if (node->type == XML_ELEMENT_NODE) {
                        const auto name = lower(text(node->name));
                        if (node->ns || (!tags.contains(name) && name != "base" && name != "meta"))
                            continue;
                        if (name == "base" && !haveBase) {
                            const auto base = canonicalSiteUrl(attribute(node, "href"), input.url);
                            if (!base.empty() && sameSiteOrigin(input.url, base)) {
                                rewrite.base = base;
                                haveBase = true;
                            }
                        } else if (name == "meta") {
                            supportedEncoding(attribute(node, "charset"));
                            if (lower(attribute(node, "http-equiv")) == "content-type")
                                supportedEncoding(encoding(attribute(node, "content")));
                        }
                        if (node->children)
                            inspect(node->children);
                    }
                }
            };
            inspect(document->children);
            sanitizeTree(document->children, rewrite);
            auto* root = xmlDocGetRootElement(document.get());
            if (!root || text(root->name) != "html")
                throw std::invalid_argument("Offline HTML has no usable document root.");
            xmlNode* head = nullptr;
            for (auto* node = root->children; node; node = node->next)
                if (node->type == XML_ELEMENT_NODE && text(node->name) == "head")
                    head = node;
            if (!head) {
                head = xmlNewNode(nullptr, BAD_CAST "head");
                if (root->children)
                    xmlAddPrevSibling(root->children, head);
                else
                    xmlAddChild(root, head);
            }
            auto* policy = xmlNewNode(nullptr, BAD_CAST "meta");
            setAttribute(policy, "http-equiv", "Content-Security-Policy");
            setAttribute(
                policy, "content",
                "default-src 'none'; script-src 'none'; connect-src 'none'; "
                "img-src 'self'; style-src 'self' 'unsafe-inline'; font-src 'self'; media-src 'self'; "
                "frame-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'");
            if (head->children)
                xmlAddPrevSibling(head->children, policy);
            else
                xmlAddChild(head, policy);
            auto* charsetNode = xmlNewNode(nullptr, BAD_CAST "meta");
            setAttribute(charsetNode, "charset", "UTF-8");
            xmlAddPrevSibling(policy, charsetNode);
            Output output{{}, limits.maxOutputBytes};
            auto* save =
                xmlSaveToIO(Output::write, nullptr, &output, "UTF-8", XML_SAVE_AS_HTML | XML_SAVE_NO_DECL);
            if (!save)
                throw std::bad_alloc();
            const auto written = xmlSaveDoc(save, document.get());
            const auto closed = xmlSaveClose(save);
            if (written < 0 || closed < 0 || output.exceeded)
                throw std::invalid_argument("Offline HTML serialization exceeds its bounded output.");
            rewrite.result.body = std::move(output.bytes);
        } else {
            if (kind == SiteContentKind::Unsupported)
                throw std::invalid_argument("Unsupported observed offline content type.");
            validateAsset(input);
            rewrite.result.body = input.body;
        }
        if (rewrite.result.body.size() > limits.maxOutputBytes)
            throw std::invalid_argument("Offline content output exceeds the limit.");
        return std::move(rewrite.result);
    }
};
} // namespace

std::string canonicalSiteUrl(std::string_view value, std::string_view base) {
    if (value.empty() || value.size() > 32768 ||
        std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch <= 0x20 || ch == 0x7f; }))
        return {};
    auto url = parsedUrl(value);
    if (url.isRelative()) {
        if (base.empty())
            return {};
        url = parsedUrl(base).resolved(url);
    }
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty() ||
        url.authority(QUrl::FullyEncoded).contains('@'))
        return {};
    url = url.adjusted(QUrl::NormalizePathSegments | QUrl::RemoveFragment);
    if (url.port() == (url.scheme() == "https" ? 443 : 80))
        url.setPort(-1);
    if (url.path().isEmpty())
        url.setPath("/");
    const auto result = url.toEncoded(QUrl::FullyEncoded).toStdString();
    return result.size() <= 32768 ? result : std::string();
}
bool sameSiteOrigin(std::string_view left, std::string_view right) {
    const auto a = canonicalSiteUrl(left), b = canonicalSiteUrl(right);
    if (a.empty() || b.empty())
        return false;
    const auto first = parsedUrl(a), second = parsedUrl(b);
    return first.scheme() == second.scheme() && first.host() == second.host() &&
           first.port(first.scheme() == "https" ? 443 : 80) ==
               second.port(second.scheme() == "https" ? 443 : 80);
}
SiteContentKind classifySiteContent(std::string_view contentType) {
    const auto type = mediaType(contentType);
    if (type == "text/html")
        return SiteContentKind::Html;
    if (type == "text/css")
        return SiteContentKind::Css;
    static const std::set<std::string> images = {
        "image/png",  "image/jpeg", "image/gif",    "image/webp",
        "image/avif", "image/bmp",  "image/x-icon", "image/vnd.microsoft.icon"};
    static const std::set<std::string> fonts = {"font/woff", "font/woff2", "font/ttf", "font/otf",
                                                "application/font-woff"};
    static const std::set<std::string> media = {"video/mp4",  "video/webm", "video/ogg",
                                                "audio/mpeg", "audio/mp4",  "audio/ogg",
                                                "audio/webm", "audio/wav",  "audio/flac"};
    if (images.contains(type))
        return SiteContentKind::Image;
    if (fonts.contains(type))
        return SiteContentKind::Font;
    if (media.contains(type))
        return SiteContentKind::Media;
    return SiteContentKind::Unsupported;
}
std::unique_ptr<ISiteContentProcessor> makeSiteContentProcessor() {
    return std::make_unique<SiteContentProcessor>();
}
} // namespace maskpilot
