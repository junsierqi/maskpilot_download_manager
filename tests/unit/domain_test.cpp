#include <gtest/gtest.h>
#include <limits>
#include <maskpilot/domain/task.h>

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
