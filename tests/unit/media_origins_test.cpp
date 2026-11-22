#include "../../src/infrastructure/src/media/media_origin_gateway.h"
#include "../../src/infrastructure/src/media/media_process.h"
#include "../../src/ui/src/models/task_list_codec.h"
#include "../../src/ui/src/style/appearance.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/domain/task.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/ui/add_download_dialog.h>
#include <sqlite3.h>

using namespace maskpilot;
namespace {
DownloadSpec policy() {
    DownloadSpec spec;
    spec.url = "https://page.example.test/media";
    spec.destination = "media.mkv";
    spec.kind = TransferKind::Media;
    spec.mediaOrigins = {{"https://cdn.example.test",
                          "owned-cdn-cookie",
                          "Bearer owned-cdn-auth",
                          {{"X-Owned", "private-value"}},
                          false}};
    spec.mediaOriginsConfirmed = true;
    return spec;
}
} // namespace
TEST(MediaOrigins, ExactOriginsNormalizeCasePortsAndIpv6WithoutBroadMatches) {
    EXPECT_EQ(mediaOrigin("HTTPS://CDN.Example.test/a?b=c"), "https://cdn.example.test:443");
    EXPECT_EQ(mediaOrigin("https://[::ffff:127.0.0.1]/x"),
              "https://[0000:0000:0000:0000:0000:ffff:7f00:0001]:443");
    EXPECT_EQ(mediaOrigin("https://[0:0::1]:443/"), "https://[0000:0000:0000:0000:0000:0000:0000:0001]:443");
    EXPECT_NE(mediaOrigin("https://cdn.test"), mediaOrigin("http://cdn.test"));
    EXPECT_NE(mediaOrigin("https://cdn.test"), mediaOrigin("https://cdn.test:444"));
    for (const auto* url : {"https://user:pass@cdn.test", "https://*.test", "https://cdn.test/a",
                            "https://cdn.test?x=1", "https://cdn.test#x", "https://cdn.test:0",
                            "https://cdn.test:65536", "file:///tmp/a", "https://[::1%25lo]"})
        EXPECT_TRUE(mediaOrigin(url, true).empty()) << url;
}
TEST(MediaOrigins, AccessRequiresApprovalAndIndependentMaterialsBeforeAnyContact) {
    auto spec = policy();
    ASSERT_TRUE(validateSpec(spec).empty());
    EXPECT_FALSE(requiresOriginAuthentication(spec)); // Public page, authenticated CDN.
    EXPECT_TRUE(validateOriginAuthentication(spec, true).empty());
    spec.mediaOriginsConfirmed = false;
    EXPECT_FALSE(validateOriginAuthentication(spec, true).empty());
    spec.mediaOriginsConfirmed = true;
    invalidateMediaOrigins(spec);
    ASSERT_EQ(spec.mediaOrigins.size(), 1);
    EXPECT_TRUE(spec.mediaOrigins[0].authenticationRequired);
    spec.mediaOriginsConfirmed = true;
    EXPECT_FALSE(validateOriginAuthentication(spec, true).empty());
    spec.mediaOrigins[0].cookie = "replacement";
    EXPECT_TRUE(validateOriginAuthentication(spec, true).empty());
    spec.originAuthenticationRequired = true;
    EXPECT_FALSE(validateOriginAuthentication(spec, true).empty());
}
TEST(MediaOrigins, InvalidRulesCannotExpandAuthorityOrOverrideTransport) {
    for (const auto* name : {"Host", "Cookie", "Authorization", "Range", "Proxy-Authorization", "Connection",
                             "Transfer-Encoding"}) {
        auto spec = policy();
        spec.mediaOrigins[0].headers = {{name, "x"}};
        EXPECT_FALSE(validateSpec(spec).empty()) << name;
    }
    auto spec = policy();
    spec.mediaOrigins.push_back(spec.mediaOrigins[0]);
    EXPECT_FALSE(validateSpec(spec).empty());
    spec = policy();
    spec.mediaOrigins[0].origin = "https://PAGE.example.test:443/";
    EXPECT_FALSE(validateSpec(spec).empty());
    spec = policy();
    spec.mediaOrigins[0].cookie = "bad\r\nheader";
    EXPECT_FALSE(validateSpec(spec).empty());
    spec = policy();
    spec.mediaOrigins[0].cookie.assign(128 * 1024, 'x');
    EXPECT_FALSE(validateSpec(spec).empty());
    spec = policy();
    spec.kind = TransferKind::Http;
    EXPECT_FALSE(validateSpec(spec).empty());
}
TEST(MediaOrigins, PublicPolicyRoundtripContainsNoMaterialAndRejectsMalformedRecords) {
    const auto spec = policy();
    const auto publicValue = encodeMediaOrigins(spec.mediaOrigins);
    EXPECT_EQ(publicValue, "1 https://cdn.example.test:443\n");
    auto restored = spec;
    restored.mediaOrigins = decodeMediaOrigins(publicValue);
    restored.mediaOriginsConfirmed = false;
    ASSERT_EQ(restored.mediaOrigins.size(), 1);
    EXPECT_TRUE(restored.mediaOrigins[0].cookie.empty());
    EXPECT_TRUE(restored.mediaOrigins[0].authorization.empty());
    EXPECT_TRUE(restored.mediaOrigins[0].headers.empty());
    EXPECT_FALSE(validateOriginAuthentication(restored, true).empty());
    for (const auto* value : {"x https://cdn.test\n", "0 https://cdn.test", "0 https://cdn.test/a\n",
                              "0 https://cdn.test\n1 https://cdn.test:443\n"})
        EXPECT_THROW(decodeMediaOrigins(value), std::invalid_argument);
}
TEST(MediaOrigins, SqliteAndTaskListRetainOnlyPublicRequirementsAndInvalidateApproval) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = std::filesystem::path(temporary.path().toStdU16String()) / "tasks.db";
    Task task;
    task.id = newTaskId();
    task.spec = policy();
    task.state = TaskState::Paused;
    {
        auto store = makeSqliteStore(path);
        store.tasks->save(task);
    }
    {
        auto store = makeSqliteStore(path);
        const auto loaded = store.tasks->load();
        ASSERT_EQ(loaded.size(), 1);
        EXPECT_EQ(encodeMediaOrigins(loaded[0].spec.mediaOrigins),
                  encodeMediaOrigins(task.spec.mediaOrigins));
        EXPECT_FALSE(loaded[0].spec.mediaOriginsConfirmed);
        EXPECT_TRUE(loaded[0].spec.mediaOrigins[0].cookie.empty());
        EXPECT_FALSE(validateOriginAuthentication(loaded[0].spec, true).empty());
    }
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    for (const auto* secret : {"owned-cdn-cookie", "owned-cdn-auth", "private-value"})
        EXPECT_EQ(bytes.find(secret), std::string::npos);
    const auto encoded = ui::encodeTaskList({task});
    auto imported = ui::decodeTaskList(encoded, [](const QString&) { return DownloadSpec{}; });
    ASSERT_EQ(imported.downloads.size(), 1);
    EXPECT_TRUE(imported.downloads[0].startPaused);
    EXPECT_FALSE(imported.downloads[0].mediaOriginsConfirmed);
    EXPECT_TRUE(imported.downloads[0].mediaOrigins[0].authenticationRequired);
    EXPECT_TRUE(imported.downloads[0].mediaOrigins[0].cookie.empty());
    EXPECT_FALSE(validateOriginAuthentication(imported.downloads[0], true).empty());
}
TEST(MediaOrigins, MigratesActualSchema18WithBackupAndRejectsUnknownFutureSchema) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = std::filesystem::path(temporary.path().toStdU16String()) / "tasks.db";
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> close(raw, sqlite3_close);
    const auto migrations =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "database/migrations";
    std::vector<std::filesystem::path> scripts;
    for (const auto& file : std::filesystem::directory_iterator(migrations))
        if (file.path().extension() == ".sql" && file.path().filename().string() < "0019")
            scripts.push_back(file.path());
    std::sort(scripts.begin(), scripts.end());
    ASSERT_EQ(scripts.size(), 18);
    for (const auto& script : scripts) {
        std::ifstream input(script);
        const std::string sql((std::istreambuf_iterator<char>(input)), {});
        ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << script;
    }
    close.reset();
    {
        auto store = makeSqliteStore(path);
        EXPECT_TRUE(store.tasks->load().empty());
    }
    bool backup = false;
    for (const auto& file : std::filesystem::directory_iterator(path.parent_path()))
        backup |= file.path().filename().string().find(".before-v19-") != std::string::npos;
    EXPECT_TRUE(backup);
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
    close.reset(raw);
    ASSERT_EQ(sqlite3_exec(raw, "PRAGMA user_version=20", nullptr, nullptr, nullptr), SQLITE_OK);
    close.reset();
    EXPECT_THROW(makeSqliteStore(path), std::runtime_error);
}
TEST(MediaOrigins, NativeGatewayRejectsDataBeforeAdapterAcknowledgmentAndKeepsPublicCodeSecretFree) {
    int argc = 1;
    char name[] = "MediaScopeGateway";
    char* argv[]{name, nullptr};
    QCoreApplication app(argc, argv);
    const auto spec = policy();
    media_process::MediaOriginGateway gateway(spec);
    EXPECT_FALSE(gateway.adapterReady());
    media_process::ProcessTree tree;
    const auto pendingRoute = tree.route(spec);
    QProcessEnvironment environment;
    QStringList arguments;
    tree.configureAdapter(environment, arguments);
    for (const auto* key : {"HTTP_PROXY", "HTTPS_PROXY", "FTP_PROXY", "ALL_PROXY"})
        EXPECT_EQ(environment.value(key), QString::fromStdString(pendingRoute.proxy));
    EXPECT_TRUE(environment.value("NO_PROXY").isEmpty());
    EXPECT_TRUE(environment.contains("MASKPILOT_MEDIA_SCOPE_ENDPOINT"));
    const auto routed = gateway.routedRequest();
    EXPECT_TRUE(routed.cookie.empty());
    EXPECT_TRUE(routed.authorization.empty());
    EXPECT_TRUE(routed.headers.empty());
    const auto path = std::filesystem::path(gateway.adapterDirectory().toStdU16String()) /
                      "scope/yt_dlp_plugins/extractor/maskpilot_scope.py";
    std::ifstream input(path);
    const std::string script((std::istreambuf_iterator<char>(input)), {});
    ASSERT_FALSE(script.empty());
    EXPECT_EQ(script.find("owned-cdn-cookie"), std::string::npos);
    EXPECT_EQ(script.find("private-value"), std::string::npos);
    const QUrl proxy(QString::fromStdString(routed.proxy));
    QTcpSocket client;
    client.connectToHost(proxy.host(), static_cast<quint16>(proxy.port()));
    ASSERT_TRUE(client.waitForConnected(2000));
    client.write("GET https://page.example.test/media HTTP/1.1\r\nHost: page.example.test\r\n\r\n");
    ASSERT_TRUE(client.waitForReadyRead(2000));
    EXPECT_TRUE(client.readAll().startsWith("HTTP/1.1 403"));
    EXPECT_FALSE(gateway.adapterReady());
}
TEST(MediaOriginsUi, ActualEditorMasksCredentialsAndUrlEditsClearAccess) {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char name[] = "MediaOriginsUi";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName("MediaOrigins");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    AddDownloadDialog dialog(qEnvironmentVariableIsSet("MASKPILOT_TEST_MEDIA_ORIGINS_CHINESE"));
    dialog.setStyleSheet(ui::styleSheet());
    auto spec = policy();
    spec.mediaOrigins.clear();
    spec.mediaOriginsConfirmed = false;
    dialog.setSpecification(spec);
    auto* add = dialog.findChild<QPushButton*>("addMediaOrigin");
    ASSERT_NE(add, nullptr);
    bool edited = false;
    QTimer::singleShot(0, [&] {
        auto* editor = dialog.findChild<QDialog*>("mediaOriginEditor");
        ASSERT_NE(editor, nullptr);
        editor->findChild<QLineEdit*>("mediaOriginUrl")->setText("https://cdn.example.test");
        auto* cookie = editor->findChild<QLineEdit*>("mediaOriginCookie");
        ASSERT_NE(cookie, nullptr);
        EXPECT_EQ(cookie->echoMode(), QLineEdit::Password);
        cookie->setText("owned-cdn-cookie");
        auto* required = editor->findChild<QCheckBox*>("mediaOriginAuthenticationRequired");
        ASSERT_NE(required, nullptr);
        EXPECT_TRUE(required->isChecked());
        EXPECT_FALSE(required->isEnabled());
        auto* addHeader = editor->findChild<QPushButton*>("addMediaOriginHeader");
        auto* headers = editor->findChild<QTableWidget*>("mediaOriginHeaders");
        ASSERT_NE(addHeader, nullptr);
        ASSERT_NE(headers, nullptr);
        addHeader->click();
        addHeader->click();
        ASSERT_EQ(headers->rowCount(), 2);
        qobject_cast<QLineEdit*>(headers->cellWidget(0, 0))->setText("X-Removed");
        qobject_cast<QLineEdit*>(headers->cellWidget(1, 0))->setText("X-Owned");
        auto* privateValue = qobject_cast<QLineEdit*>(headers->cellWidget(1, 1));
        ASSERT_NE(privateValue, nullptr);
        EXPECT_EQ(privateValue->echoMode(), QLineEdit::Password);
        privateValue->setText("owned-header-value");
        qobject_cast<QPushButton*>(headers->cellWidget(0, 2))->click();
        EXPECT_EQ(headers->rowCount(), 1);
        if (qEnvironmentVariableIsSet("MASKPILOT_TEST_MEDIA_ORIGINS_SCREENSHOT")) {
            QCoreApplication::processEvents();
            EXPECT_TRUE(editor->grab().save(qEnvironmentVariable("MASKPILOT_TEST_MEDIA_ORIGINS_SCREENSHOT") +
                                            ".editor.png"));
        }
        editor->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        edited = true;
    });
    add->click();
    ASSERT_TRUE(edited);
    auto* consent = dialog.findChild<QCheckBox*>("mediaOriginsConfirmed");
    ASSERT_NE(consent, nullptr);
    EXPECT_FALSE(consent->isChecked());
    consent->setChecked(true);
    const auto configured = dialog.specification();
    ASSERT_EQ(configured.mediaOrigins.size(), 1);
    EXPECT_EQ(configured.mediaOrigins[0].cookie, "owned-cdn-cookie");
    EXPECT_TRUE(configured.mediaOriginsConfirmed);
    ASSERT_EQ(configured.mediaOrigins[0].headers.size(), 1);
    EXPECT_EQ(configured.mediaOrigins[0].headers[0].first, "X-Owned");
    EXPECT_EQ(configured.mediaOrigins[0].headers[0].second, "owned-header-value");
    if (qEnvironmentVariableIsSet("MASKPILOT_TEST_MEDIA_ORIGINS_SCREENSHOT")) {
        auto* tabs = dialog.findChild<QTabWidget*>("downloadOptionsTabs");
        ASSERT_NE(tabs, nullptr);
        for (int index = 0; index < tabs->count(); ++index)
            if (tabs->widget(index)->isAncestorOf(add))
                tabs->setCurrentIndex(index);
        dialog.show();
        QCoreApplication::processEvents();
        EXPECT_TRUE(dialog.grab().save(qEnvironmentVariable("MASKPILOT_TEST_MEDIA_ORIGINS_SCREENSHOT")));
    }
    auto* table = dialog.findChild<QTableWidget*>("mediaOriginsTable");
    ASSERT_NE(table, nullptr);
    EXPECT_FALSE(table->item(0, 0)->text().contains("cookie"));
    EXPECT_FALSE(table->item(0, 1)->text().contains("cookie"));
    auto* url = dialog.findChild<QLineEdit*>("downloadUrl");
    ASSERT_NE(url, nullptr);
    url->setText("https://page.example.test/replacement");
    ASSERT_TRUE(
        QMetaObject::invokeMethod(url, "textEdited", Qt::DirectConnection, Q_ARG(QString, url->text())));
    const auto changed = dialog.specification();
    ASSERT_EQ(changed.mediaOrigins.size(), 1);
    EXPECT_TRUE(changed.mediaOrigins[0].cookie.empty());
    EXPECT_TRUE(changed.mediaOrigins[0].authenticationRequired);
    EXPECT_FALSE(changed.mediaOriginsConfirmed);
}
