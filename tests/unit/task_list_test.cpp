#include "../../src/ui/src/models/task_list_codec.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <gtest/gtest.h>

namespace {
maskpilot::DownloadSpec defaults(const QString& url) {
    maskpilot::DownloadSpec spec;
    spec.url = url.toStdString();
    spec.proxy = "http://legacy-default.test:8080";
    spec.mediaDownloader = "trusted-local-parser.exe";
    spec.mediaMuxer = "trusted-local-muxer.exe";
    return spec;
}
QJsonObject entry() {
    return {{"url", "https://example.test/file"},
            {"destination", "reviewed.bin"},
            {"proxyAuthenticationRequired", false},
            {"originAuthenticationRequired", false}};
}
QByteArray document(const QJsonObject& value, double version = 2) {
    return QJsonDocument(QJsonObject{{"version", version}, {"downloads", QJsonArray{value}}}).toJson();
}
} // namespace

TEST(TaskList, VersionTwoPreservesRequirementsButNeverRestoresContextAuthorityOrSecrets) {
    maskpilot::Task task;
    task.id = maskpilot::newTaskId();
    task.ownerId = maskpilot::newTaskId();
    task.requestId = maskpilot::newTaskId();
    task.contextRevision = 91;
    task.proxyCredentialRef = maskpilot::newTaskId();
    task.originGrantId = maskpilot::newTaskId();
    task.originAuthenticationRequired = true;
    task.spec.url = "https://example.test/file";
    task.spec.destination = "reviewed.bin";
    task.spec.proxy = "socks5://127.0.0.1:1080";
    task.spec.proxyUsername = "private-list-proxy-user";
    task.spec.proxyPassword = "private-list-proxy-password";
    task.spec.cookie = "private-list-cookie";
    task.spec.authorization = "private-list-authorization";
    task.spec.headers = {{"X-Api-Key", "private-list-key"}};
    task.spec.mediaDownloader = "private-tool-path.exe";
    const auto bytes = maskpilot::ui::encodeTaskList({task});
    const auto root = QJsonDocument::fromJson(bytes).object();
    EXPECT_EQ(root.value("version").toInt(), 2);
    const auto object = root.value("downloads").toArray().first().toObject();
    EXPECT_TRUE(object.value("contextBound").toBool());
    EXPECT_TRUE(object.value("originAuthenticationRequired").toBool());
    EXPECT_TRUE(object.value("proxyAuthenticationRequired").toBool());
    for (const auto& forbidden :
         {task.id, task.ownerId, task.requestId, task.proxyCredentialRef, task.originGrantId,
          task.spec.proxyUsername, task.spec.proxyPassword, task.spec.cookie, task.spec.authorization,
          task.spec.headers[0].second, task.spec.mediaDownloader.string()})
        EXPECT_FALSE(bytes.contains(forbidden.c_str()));
    const auto decoded = maskpilot::ui::decodeTaskList(bytes, defaults);
    ASSERT_EQ(decoded.downloads.size(), 1);
    EXPECT_EQ(decoded.version, 2);
    EXPECT_EQ(decoded.contextBoundCount, 1);
    const auto& spec = decoded.downloads[0];
    EXPECT_TRUE(spec.startPaused);
    EXPECT_TRUE(spec.originAuthenticationRequired);
    EXPECT_TRUE(spec.proxyAuthenticationRequired);
    EXPECT_TRUE(spec.cookie.empty());
    EXPECT_TRUE(spec.authorization.empty());
    EXPECT_TRUE(spec.headers.empty());
    EXPECT_TRUE(spec.proxyUsername.empty());
    EXPECT_TRUE(spec.proxyPassword.empty());
    EXPECT_EQ(spec.mediaDownloader, "trusted-local-parser.exe");
    EXPECT_EQ(spec.proxy, task.spec.proxy);
}

TEST(TaskList, VersionOneKeepsVisibleLegacyDefaultRouteWithoutInheritingCredentials) {
    auto value = entry();
    value.remove("originAuthenticationRequired");
    value.remove("proxyAuthenticationRequired");
    const auto decoded = maskpilot::ui::decodeTaskList(document(value, 1), [](const QString& url) {
        auto spec = defaults(url);
        spec.cookie = "must-not-inherit-cookie";
        spec.authorization = "must-not-inherit-authorization";
        spec.proxyUsername = "must-not-inherit-user";
        spec.proxyPassword = "must-not-inherit-password";
        spec.ftpUsername = "must-not-inherit-ftp";
        spec.ftpPassword = "must-not-inherit-ftp-password";
        spec.headers = {{"X-Key", "must-not-inherit-key"}};
        return spec;
    });
    ASSERT_EQ(decoded.downloads.size(), 1);
    EXPECT_EQ(decoded.version, 1);
    EXPECT_EQ(decoded.contextBoundCount, 0);
    const auto& spec = decoded.downloads[0];
    EXPECT_EQ(spec.proxy, "http://legacy-default.test:8080");
    EXPECT_TRUE(spec.startPaused);
    EXPECT_FALSE(spec.originAuthenticationRequired);
    EXPECT_TRUE(spec.cookie.empty());
    EXPECT_TRUE(spec.authorization.empty());
    EXPECT_TRUE(spec.proxyUsername.empty());
    EXPECT_TRUE(spec.proxyPassword.empty());
    EXPECT_TRUE(spec.ftpUsername.empty());
    EXPECT_TRUE(spec.ftpPassword.empty());
    EXPECT_TRUE(spec.headers.empty());
}

TEST(TaskList, RejectsNonIntegralVersionsBadTypesAndAuthorityInjection) {
    for (const double version : {0.0, 1.1, 1.9, 2.1, 3.0})
        EXPECT_THROW(maskpilot::ui::decodeTaskList(document(entry(), version), defaults), std::exception);
    for (const auto* field : {"originAuthenticationRequired", "proxyAuthenticationRequired", "contextBound",
                              "media", "ftpActive", "audioOnly"}) {
        auto malformed = entry();
        malformed[field] = "true";
        EXPECT_THROW(maskpilot::ui::decodeTaskList(document(malformed), defaults), std::exception) << field;
    }
    for (const auto* field : {"ownerId", "contextId", "proxyCredentialRef", "originGrantId", "sessionToken",
                              "requestId", "ownershipNonce", "cookie", "authorization", "headers",
                              "proxyUsername", "proxyPassword", "mediaDownloader", "mediaMuxer"}) {
        auto injected = entry();
        injected[field] = "untrusted-private-input";
        EXPECT_THROW(maskpilot::ui::decodeTaskList(document(injected), defaults), std::exception) << field;
    }
    auto missing = entry();
    missing.remove("originAuthenticationRequired");
    EXPECT_THROW(maskpilot::ui::decodeTaskList(document(missing), defaults), std::exception);
    auto fractional = entry();
    fractional["connections"] = 2.5;
    EXPECT_THROW(maskpilot::ui::decodeTaskList(document(fractional), defaults), std::exception);
    const auto invalidLast =
        QJsonDocument(
            QJsonObject{{"version", 2},
                        {"downloads", QJsonArray{entry(), QJsonObject{{"url", "file:///private"}}}}})
            .toJson();
    EXPECT_THROW(maskpilot::ui::decodeTaskList(invalidLast, defaults), std::exception);
    const auto nonObject =
        QJsonDocument(QJsonObject{{"version", 2}, {"downloads", QJsonArray{entry(), 7}}}).toJson();
    EXPECT_THROW(maskpilot::ui::decodeTaskList(nonObject, defaults), std::exception);
}

TEST(TaskList, TorrentBinaryMetadataAndSelectedFilesSurvivePureCodec) {
    maskpilot::Task task;
    task.spec.url = "torrent:C:/original.torrent";
    task.spec.kind = maskpilot::TransferKind::BitTorrent;
    task.spec.destination = "torrent-output";
    task.spec.torrent.allFiles = false;
    task.spec.torrent.selectedFiles = {0, 2};
    task.spec.torrent.metadata =
        std::make_shared<const std::vector<char>>(std::initializer_list<char>{'d', 0, 'e'});
    task.spec.torrent.uploadLimit = 12345;
    const auto decoded = maskpilot::ui::decodeTaskList(maskpilot::ui::encodeTaskList({task}), defaults);
    ASSERT_EQ(decoded.downloads.size(), 1);
    const auto& spec = decoded.downloads[0];
    ASSERT_NE(spec.torrent.metadata, nullptr);
    EXPECT_EQ(*spec.torrent.metadata, *task.spec.torrent.metadata);
    EXPECT_EQ(spec.torrent.selectedFiles, task.spec.torrent.selectedFiles);
    EXPECT_EQ(spec.torrent.uploadLimit, 12345);
    EXPECT_TRUE(spec.startPaused);
}
