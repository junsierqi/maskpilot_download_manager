#include <algorithm>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <set>

using namespace maskpilot;
namespace {
SiteContentInput page(std::string body) {
    return {"https://example.test/docs/index.html", "text/html; charset=UTF-8", std::move(body)};
}
bool hasReference(const SiteContentResult& result, std::string_view url, SiteContentKind kind) {
    return std::any_of(result.references.begin(), result.references.end(),
                       [&](const auto& ref) { return ref.url == url && ref.kind == kind; });
}
} // namespace

TEST(SiteContent, CanonicalUrlsKeepQueriesResolvePathsAndCompareEffectiveOrigins) {
    EXPECT_EQ(canonicalSiteUrl("HTTP://Example.Test:80/a/../docs?q=1#part"), "http://example.test/docs?q=1");
    EXPECT_EQ(canonicalSiteUrl("../image.png?q=2", "https://example.test/docs/index.html"),
              "https://example.test/image.png?q=2");
    EXPECT_TRUE(sameSiteOrigin("http://[0:0:0:0:0:0:0:1]/", "http://[::1]:80/a"));
    EXPECT_FALSE(sameSiteOrigin("https://example.test/", "http://example.test/"));
    EXPECT_FALSE(sameSiteOrigin("https://example.test/", "https://example.test:444/"));
    for (const auto* invalid : {"https://u:p@example.test/", "https://@example.test/", "file:///C:/x",
                                "javascript:alert(1)", "http:///", "https://example.test/a\n"})
        EXPECT_TRUE(canonicalSiteUrl(invalid).empty()) << invalid;
}

TEST(SiteContent, ClassifiesObservedMimeWithoutGuessingExtensionsOrAcceptingActiveSvg) {
    EXPECT_EQ(classifySiteContent("Text/HTML; charset=UTF-8"), SiteContentKind::Html);
    EXPECT_EQ(classifySiteContent("text/css"), SiteContentKind::Css);
    EXPECT_EQ(classifySiteContent("image/png"), SiteContentKind::Image);
    EXPECT_EQ(classifySiteContent("font/woff2"), SiteContentKind::Font);
    EXPECT_EQ(classifySiteContent("audio/ogg"), SiteContentKind::Media);
    for (const auto* invalid :
         {"", "image/svg+xml", "application/xhtml+xml", "application/octet-stream", "text/html\r\nmalicious"})
        EXPECT_EQ(classifySiteContent(invalid), SiteContentKind::Unsupported);
}

TEST(SiteContent, MaintainedTreeRewritesFinalBaseSrcsetCssAndAnchorsWhileRemovingActiveContent) {
    auto processor = makeSiteContentProcessor();
    const auto input = page(R"HTML(<html><head><base href="../assets/"><link rel="stylesheet" href="theme">
        <meta http-equiv="refresh" content="0;url=https://foreign.test/"></head><body onload="bad()">
        <h1 id="heading">Title &amp; text</h1><a href="../next?q=1#heading" ping="https://foreign.test/">Next</a>
        <img src="logo" srcset="small 1x, large 2x" onerror="bad()">
        <style>.photo { background: url(background); }</style>
        <script>window.activeSentinel=1</script><iframe src="frame"></iframe>
        <form action="send"><input name="secret"></form><object data="active"></object>
        <svg><image href="https://foreign.test/"></image></svg>
        <img src="https://foreign.test/pixel"><a href="javascript:bad()">bad</a></body></html>)HTML");
    auto result = processor->process(input, {}, [](const SiteContentReference& ref) {
        return std::optional<std::string>(ref.kind == SiteContentKind::Html ? "page.html" : "asset.bin");
    });
    EXPECT_TRUE(hasReference(result, "https://example.test/assets/theme", SiteContentKind::Css));
    EXPECT_TRUE(hasReference(result, "https://example.test/assets/logo", SiteContentKind::Image));
    EXPECT_TRUE(hasReference(result, "https://example.test/assets/background", SiteContentKind::Image));
    EXPECT_TRUE(hasReference(result, "https://example.test/next?q=1", SiteContentKind::Html));
    EXPECT_TRUE(hasReference(result, "https://example.test/assets/small", SiteContentKind::Image));
    EXPECT_TRUE(hasReference(result, "https://example.test/assets/large", SiteContentKind::Image));
    EXPECT_NE(result.body.find("page.html#heading"), std::string::npos);
    EXPECT_NE(result.body.find("asset.bin 1x, asset.bin 2x"), std::string::npos);
    EXPECT_NE(result.body.find("Content-Security-Policy"), std::string::npos);
    EXPECT_NE(result.body.find("Title &amp; text"), std::string::npos);
    for (const auto* forbidden : {"<script", "activeSentinel", "onload", "onerror", "<iframe", "<form",
                                  "<object", "<svg", "<base", "refresh", "javascript:", "foreign.test"})
        EXPECT_EQ(result.body.find(forbidden), std::string::npos) << forbidden;
}

TEST(SiteContent, DiscoveryDoesNotTrustRemotePathsAndResolverCannotEscapeExport) {
    const auto processor = makeSiteContentProcessor();
    const auto input = page("<img src='../logo.png'><img src='file:///C:/secret'><a href='/a'>Link</a>");
    auto discovered = processor->process(input, {});
    EXPECT_TRUE(hasReference(discovered, "https://example.test/logo.png", SiteContentKind::Image));
    EXPECT_EQ(discovered.references.size(), 2u);
    EXPECT_EQ(discovered.body.find("src="), std::string::npos);
    for (const auto* path : {"../escape.png", "/absolute.png", "C:/escape.png", "a/../../escape", "a\\x",
                             "a.png?remote=1", "a.png#fragment", "https://foreign.test/", "a//b"}) {
        EXPECT_THROW(
            processor->process(input, {}, [path](const auto&) { return std::optional<std::string>(path); }),
            std::exception)
            << path;
    }
}

TEST(SiteContent, EnforcesInputTreeDepthAttributesReferencesAndSerializedBounds) {
    auto processor = makeSiteContentProcessor();
    auto input = page("<div><p>text</p><img src='a'><img src='b'></div>");
    SiteContentLimits limits;
    limits.maxInputBytes = 2;
    EXPECT_THROW(processor->process(input, limits), std::exception);
    limits = {};
    limits.maxNodes = 2;
    EXPECT_THROW(processor->process(input, limits), std::exception);
    limits = {};
    limits.maxDepth = 2;
    EXPECT_THROW(processor->process(input, limits), std::exception);
    limits = {};
    limits.maxReferences = 1;
    EXPECT_THROW(processor->process(input, limits), std::exception);
    input = page("<style>a { background:url(a),url(b); }</style>");
    EXPECT_THROW(processor->process(input, limits), std::exception);
    limits = {};
    limits.maxOutputBytes = 32;
    EXPECT_THROW(processor->process(input, limits), std::exception);
    input = page("<p title='" + std::string(32769, 'a') + "'>x</p>");
    EXPECT_THROW(processor->process(input, {}), std::exception);
}

TEST(SiteContent, DoesNotLoadDtdEntitiesAndHandlesMalformedHtmlWithoutActiveFallback) {
    auto processor = makeSiteContentProcessor();
    auto input = page(R"(<!DOCTYPE html PUBLIC "owned" "file:///C:/owned-secret.txt">
        <html><head><title>Broken</title></head><body><p>one<p>two &amp; three
        <script src="http://127.0.0.1:9/active"></script><img src="ok"></body>)");
    auto result = processor->process(input, {});
    EXPECT_NE(result.body.find("two &amp; three"), std::string::npos);
    EXPECT_EQ(result.body.find("owned-secret"), std::string::npos);
    EXPECT_EQ(result.body.find("127.0.0.1"), std::string::npos);
    EXPECT_TRUE(hasReference(result, "https://example.test/docs/ok", SiteContentKind::Image));
}

TEST(SiteContent, PreservesSupportedTextAndRejectsUnknownDeclaredEncoding) {
    auto processor = makeSiteContentProcessor();
    auto input = page("<p>caf\xe9</p>");
    input.contentType = "text/html; charset=ISO-8859-1";
    EXPECT_NE(processor->process(input, {}).body.find("caf\xc3\xa9"), std::string::npos);
    for (const auto* type : {"text/html; charset=GBK", "text/html; charset=unknown",
                             "text/html; charset=UTF-8; charset=ISO-8859-1"}) {
        input.contentType = type;
        EXPECT_THROW(processor->process(input, {}), std::exception);
    }
    input = page("<meta charset='GB18030'><p>unsupported</p>");
    EXPECT_THROW(processor->process(input, {}), std::exception);
}

TEST(SiteContent, CssTokenizerHandlesCommentsEscapedUrlsImportsAndFontReferences) {
    auto processor = makeSiteContentProcessor();
    SiteContentInput input{"https://example.test/styles/main", "text/css; charset=UTF-8", R"(
        @import "base"; @font-face { font-family: Own; src: url(font) format("woff2"); }
        .photo > img { background: u\72l(/* comment */ '../images/photo'); color: rgb(1, 2, 3); }
        .blocked { background: url(https://foreign.test/pixel); }
        @media screen and (max-width: 900px) { .photo > img { width: calc(100% - 1px); } }
    )"};
    auto result =
        processor->process(input, {}, [](const auto&) { return std::optional<std::string>("local.bin"); });
    EXPECT_TRUE(hasReference(result, "https://example.test/styles/base", SiteContentKind::Css));
    EXPECT_TRUE(hasReference(result, "https://example.test/styles/font", SiteContentKind::Font));
    EXPECT_TRUE(hasReference(result, "https://example.test/images/photo", SiteContentKind::Image));
    EXPECT_EQ(result.references.size(), 3u);
    EXPECT_EQ(result.body.find("foreign.test"), std::string::npos);
    EXPECT_NE(result.body.find("data:,"), std::string::npos);
    EXPECT_NE(result.body.find("local.bin"), std::string::npos);
}

TEST(SiteContent, CssFailsClosedForObscureFunctionsMalformedTokensAndActiveSyntax) {
    auto processor = makeSiteContentProcessor();
    for (const auto* css :
         {"x {background: image-set('file:///C:/secret' 1x);}", "x {width: e\\78pression(bad);}",
          "x {-moz-binding: url(x);}", "x {background:url('unterminated);}", "/* missing close",
          "x { color:red;", "@unknown 'file:///C:/secret';"}) {
        SiteContentInput input{"https://example.test/a", "text/css", css};
        EXPECT_THROW(processor->process(input, {}), std::exception) << css;
    }
    const auto input = page("<p style='background:image-set(url(secret) 1x)'>Safe text</p>");
    const auto output = processor->process(input, {});
    EXPECT_NE(output.body.find("Safe text"), std::string::npos);
    EXPECT_EQ(output.body.find("style="), std::string::npos);
}

TEST(SiteContent, EscapedCssCannotCreateMarkupAtKeywordsOrSwallowResolverFailures) {
    auto processor = makeSiteContentProcessor();
    auto input = page(
        R"HTML(<style>p::before { content:"\3c /style\3e \3c img id='injected' src='file:///C:/private.png'\3e "; }</style><p>safe</p>)HTML");
    auto result = processor->process(input, {});
    EXPECT_EQ(result.body.find("<img"), std::string::npos);
    EXPECT_NE(result.body.find("\\3c "), std::string::npos);
    input = {"https://example.test/a", "text/css", "\\40import \"https://foreign.test/a.css\";"};
    result = processor->process(input, {});
    EXPECT_EQ(result.body.find("@import"), std::string::npos);
    input = page("<style>body { background: url(a); }</style>");
    EXPECT_THROW(processor->process(input, {},
                                    [](const auto&) -> std::optional<std::string> {
                                        throw std::invalid_argument("Resolver failed");
                                    }),
                 std::invalid_argument);
}

TEST(SiteContent, CssExpansionStopsAtOutputBudgetBeforeRepeatedLongPathRewrites) {
    auto processor = makeSiteContentProcessor();
    std::string css;
    for (int n = 0; n < 1000; ++n)
        css += "a{background:url(x)}";
    SiteContentInput input{"https://example.test/a", "text/css", css};
    SiteContentLimits limits;
    limits.maxOutputBytes = 1024;
    int calls = 0;
    EXPECT_THROW(processor->process(input, limits,
                                    [&](const auto&) {
                                        ++calls;
                                        return std::optional<std::string>(std::string(500, 'a') + ".png");
                                    }),
                 std::length_error);
    EXPECT_LT(calls, 4);
}

TEST(SiteContent, FirstUsableBaseIsChosenInDocumentOrderOutsideRemovedSubtrees) {
    auto processor = makeSiteContentProcessor();
    const auto input = page("<html><head><base href='/head/'></head><body><base href='/body/'>"
                            "<template><base href='/hidden/'></template><img src='logo'></body></html>");
    const auto result = processor->process(input, {});
    EXPECT_TRUE(hasReference(result, "https://example.test/head/logo", SiteContentKind::Image));
    EXPECT_FALSE(hasReference(result, "https://example.test/body/logo", SiteContentKind::Image));
}

TEST(SiteContent, BinaryAssetsNeedObservedSupportedTypeAndMatchingSignature) {
    auto processor = makeSiteContentProcessor();
    SiteContentInput input{"https://example.test/logo", "image/png", std::string("\x89PNG\r\n\x1a\n", 8)};
    EXPECT_EQ(processor->process(input, {}).body, input.body);
    for (const auto* type : {"image/png", "image/jpeg", "image/gif", "font/woff2", "video/mp4"}) {
        input.contentType = type;
        input.body = "<svg xmlns='http://www.w3.org/2000/svg' onload='bad()'></svg>";
        EXPECT_THROW(processor->process(input, {}), std::exception);
        input.body = "<html><script>bad()</script></html>";
        EXPECT_THROW(processor->process(input, {}), std::exception);
    }
    input.contentType = "image/svg+xml";
    EXPECT_THROW(processor->process(input, {}), std::exception);
}
