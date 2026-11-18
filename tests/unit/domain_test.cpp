#include <gtest/gtest.h>
#include <limits>
#include <maskpilot/domain/proxy.h>
#include <maskpilot/domain/task.h>

TEST(OriginAuthentication, MissingSavedMaterialIsStructurallyValidButCannotStart) {
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/private";
    spec.destination = "private.bin";
    spec.originAuthenticationRequired = true;
    EXPECT_TRUE(maskpilot::validateSpec(spec).empty());
    EXPECT_TRUE(maskpilot::validateOriginAuthentication(spec).empty());
    EXPECT_FALSE(maskpilot::validateOriginAuthentication(spec, true).empty());
    spec.cookie = "session=fixture";
    EXPECT_TRUE(maskpilot::validateOriginAuthentication(spec, true).empty());
    spec.originAuthenticationRequired = false;
    EXPECT_TRUE(maskpilot::requiresOriginAuthentication(spec));
    spec.cookie.clear();
    spec.headers = {{"X-Request-Identity", "fixture"}};
    EXPECT_TRUE(maskpilot::hasOriginMaterial(spec));
    EXPECT_TRUE(maskpilot::requiresOriginAuthentication(spec));
    spec.headers.clear();
    spec.proxyUsername = "proxy-user";
    spec.proxyPassword = "proxy-password";
    EXPECT_FALSE(maskpilot::requiresOriginAuthentication(spec));
}
TEST(OriginAuthentication, RuntimeMaterialMustMatchItsProtocol) {
    maskpilot::DownloadSpec spec;
    spec.url = "ftp://example.test/private";
    spec.destination = "private.bin";
    spec.kind = maskpilot::TransferKind::Ftp;
    spec.originAuthenticationRequired = true;
    EXPECT_FALSE(maskpilot::validateOriginAuthentication(spec, true).empty());
    spec.ftpPassword = "password-only";
    EXPECT_FALSE(maskpilot::validateOriginAuthentication(spec, true).empty());
    spec.ftpUsername = "fixture-user";
    spec.ftpPassword.clear();
    EXPECT_TRUE(maskpilot::validateOriginAuthentication(spec, true).empty());
    spec.ftpUsername.clear();
    spec.url = "https://example.test/media";
    spec.kind = maskpilot::TransferKind::Media;
    EXPECT_FALSE(maskpilot::validateOriginAuthentication(spec, true).empty());
    spec.kind = maskpilot::TransferKind::BitTorrent;
    EXPECT_FALSE(maskpilot::validateOriginAuthentication(spec, true).empty());
}

TEST(Proxy, KeepsProxyHeadersSeparateFromOriginRequestHeaders) {
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/file";
    spec.destination = "file.bin";
    for (const auto* name : {"Proxy-Authorization", "proxy-authorization", "pRoXy-CoNnEcTiOn"}) {
        spec.headers = {{name, "fixture-value"}};
        EXPECT_EQ(maskpilot::validateSpec(spec), "This header is controlled by the download engine.");
    }
    spec.headers = {{"Authorization", "Bearer origin-fixture"}, {"Cookie", "session=origin-fixture"}};
    EXPECT_TRUE(maskpilot::validateSpec(spec).empty());
}

TEST(Proxy, ParsesOnlyCredentialFreeSupportedEndpoints) {
    EXPECT_EQ(maskpilot::parseProxyEndpoint("").type, maskpilot::ProxyType::Direct);
    for (const auto& value : {"http://localhost", "HTTPS://Proxy.Example:443", "socks5://127.0.0.1",
                              "socks5h://[::1]:1080", "http://[2001:db8::1]", "http://[::ffff:127.0.0.1]"})
        EXPECT_NO_THROW(maskpilot::parseProxyEndpoint(value)) << value;
    EXPECT_EQ(maskpilot::parseProxyEndpoint("http://localhost").port, 80);
    EXPECT_EQ(maskpilot::parseProxyEndpoint("https://localhost").port, 443);
    EXPECT_EQ(maskpilot::parseProxyEndpoint("socks5://localhost").port, 1080);
    for (const auto& value : {"localhost:80",
                              "socks4://localhost:80",
                              "http://user:secret@host:80",
                              "http://host/path",
                              "http://host?query",
                              "http://host#fragment",
                              "http://host:",
                              "http://host:0",
                              "http://host:65536",
                              "http://host:-1",
                              "http://host:+80",
                              "http://host:80:81",
                              "http://[:::1]:80",
                              "http://[1:2:3:4:5:6:7]:80",
                              "http://[]:80",
                              "http://::1",
                              "http://256.1.1.1",
                              "http://bad_host",
                              "http://-host",
                              "http://host\n"})
        EXPECT_THROW(maskpilot::parseProxyEndpoint(value), std::invalid_argument) << value;
}
TEST(Proxy, RequiresProcessLocalCredentialsAndRejectsUnsafeCombinations) {
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/file";
    spec.destination = "file.bin";
    spec.proxy = "socks5h://localhost:1080";
    spec.proxyAuthenticationRequired = true;
    EXPECT_TRUE(maskpilot::validateProxy(spec).empty());
    EXPECT_TRUE(maskpilot::validateSpec(spec).empty());
    EXPECT_FALSE(maskpilot::validateProxy(spec, true).empty());
    spec.proxyUsername = "fixture-user";
    spec.proxyPassword = "fixture-password";
    EXPECT_TRUE(maskpilot::validateProxy(spec).empty());
    spec.proxyPassword.assign(256, 'p');
    EXPECT_FALSE(maskpilot::validateProxy(spec).empty());
    spec.proxyPassword = std::string("secret\0suffix", 13);
    EXPECT_FALSE(maskpilot::validateProxy(spec).empty());
    spec.proxyPassword = "fixture-password";
    spec.url = "ftp://localhost/file";
    spec.ftpActiveMode = true;
    EXPECT_FALSE(maskpilot::validateProxy(spec).empty());
    spec.ftpActiveMode = false;
    spec.proxy.clear();
    EXPECT_FALSE(maskpilot::validateProxy(spec).empty());
    spec.proxyUsername.clear();
    spec.proxyPassword.clear();
    spec.proxyAuthenticationRequired = false;
    EXPECT_TRUE(maskpilot::validateProxy(spec).empty());
}

TEST(Segments, PartitionsLargeFilesWithoutGapsOrOverflow) {
    for (auto length : {0LL, 1LL, 7LL, 4294967301LL, std::numeric_limits<long long>::max()}) {
        auto parts = maskpilot::splitSegments(length, 8);
        ASSERT_FALSE(parts.empty());
        EXPECT_EQ(parts.front().begin, 0);
        EXPECT_EQ(parts.back().end, length);
        for (size_t i = 1; i < parts.size(); ++i)
            EXPECT_EQ(parts[i - 1].end, parts[i].begin);
    }
}
TEST(Segments, RejectsInvalidArguments) {
    EXPECT_TRUE(maskpilot::splitSegments(-1, 2).empty());
    EXPECT_TRUE(maskpilot::splitSegments(100, 0).empty());
}
TEST(Validation, RejectsProtocolAndHeaderInjection) {
    maskpilot::DownloadSpec s;
    s.url = "https://example.test/file";
    s.destination = "file.bin";
    EXPECT_TRUE(maskpilot::validateSpec(s).empty());
    s.cookie = "x\r\nInjected: 1";
    EXPECT_FALSE(maskpilot::validateSpec(s).empty());
    s.cookie.clear();
    s.headers = {{"Range", "bytes=1-"}};
    EXPECT_FALSE(maskpilot::validateSpec(s).empty());
    s.headers.clear();
    s.url = "file:///etc/passwd";
    EXPECT_FALSE(maskpilot::validateSpec(s).empty());
}
TEST(Validation, RejectsEmbeddedPathTerminatorsAndInvalidHeaderNames) {
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/file";
    spec.destination = std::filesystem::path(std::u8string(u8"existing.bin\0suffix", 19));
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
    spec.destination = std::filesystem::path(u8"parent\nfolder/file.bin");
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
#ifdef _WIN32
    spec.destination = std::filesystem::path(std::wstring(1, static_cast<wchar_t>(0xd800)));
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
#endif
    for (const auto* reserved : {u8"COM\u00b9.bin", u8"LPT\u00b2.txt", u8"CONOUT$.bin"}) {
        spec.destination = std::filesystem::path(reserved);
        EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
    }
    spec.destination = "file.bin";
    spec.headers = {{std::string("X-Name\0suffix", 13), "value"}};
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
    spec.headers = {{"X_Custom-Token", "value"}};
    EXPECT_TRUE(maskpilot::validateSpec(spec).empty());
}
TEST(Identity, GeneratesPathSafeDistinctIds) {
    auto a = maskpilot::newTaskId(), b = maskpilot::newTaskId();
    EXPECT_EQ(a.size(), 32);
    EXPECT_NE(a, b);
}
TEST(Validation, RejectsProxySecretsAndAcceptsQueryAtSign) {
    maskpilot::DownloadSpec s;
    s.destination = "file";
    s.url = "https://example.test?email=a@b";
    EXPECT_TRUE(maskpilot::validateSpec(s).empty());
    s.proxy = "http://user:password@proxy.test:8080";
    EXPECT_FALSE(maskpilot::validateSpec(s).empty());
}

TEST(Validation, SeparatesFtpCredentialsFromHttpAndMediaOptions) {
    maskpilot::DownloadSpec spec;
    spec.url = "ftp://example.test/file.bin";
    spec.destination = "file.bin";
    spec.ftpUsername = "user";
    spec.ftpPassword = "password";
    spec.ftpExplicitTls = true;
    spec.ftpActiveMode = true;
    spec.ftpCaBundle = "private-ca.pem";
    EXPECT_TRUE(maskpilot::validateSpec(spec).empty());
    spec.url = "ftps://example.test/file.bin";
    EXPECT_TRUE(maskpilot::validateSpec(spec).empty());
    spec.cookie = "session=value";
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
    spec.cookie.clear();
    spec.kind = maskpilot::TransferKind::Media;
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
    spec.kind = maskpilot::TransferKind::Http;
    spec.url = "ftp://user:password@example.test/file.bin";
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
    spec.url = "https://example.test/file.bin";
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
    spec.url = "ftp://example.test/file.bin";
    spec.ftpPassword += '\n';
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
}

TEST(TorrentPaths, RejectsTraversalDevicesAndReservedInternalNames) {
    for (const auto* path :
         {"../file", "root/../file", "/absolute", "C:/file", "root\\..\\file", "root/NUL.txt",
          "root/COM1.bin", "root/file:stream", "root/file.", "root/.maskpilot-owner", "root//file", "root/"})
        EXPECT_FALSE(maskpilot::validateTorrentPath(path).empty()) << path;
    EXPECT_TRUE(maskpilot::validateTorrentPath("bundle/subfolder/file.bin").empty());
}
TEST(Validation, TorrentSourceValidationNeverAccessesNetworkPaths) {
    maskpilot::DownloadSpec spec;
    spec.destination = "torrent-output";
    for (const auto* source : {R"(torrent:\\untrusted.invalid\share\fixture.torrent)",
                               R"(torrent:\\?\UNC\untrusted.invalid\share\fixture.torrent)",
                               R"(torrent:/\untrusted.invalid\share\fixture.torrent)",
                               "torrent://untrusted.invalid/share/fixture.torrent"}) {
        spec.url = source;
        EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
    }
    spec.url = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";
    spec.torrent.allFiles = false;
    EXPECT_TRUE(maskpilot::validateSpec(spec).empty());
    spec.torrent.selectedFiles = {1, 1};
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
    spec.torrent.selectedFiles = {-1};
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
    spec.torrent.selectedFiles.clear();
    spec.torrent.uploadLimit = -1;
    EXPECT_FALSE(maskpilot::validateSpec(spec).empty());
}
