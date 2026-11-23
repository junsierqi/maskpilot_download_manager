#include "../../src/infrastructure/src/media/media_origin_gateway.h"
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/media_tools.h>
#include <maskpilot/ui/add_download_dialog.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

using namespace maskpilot;
namespace {
class Origin final : public QObject {
    QTcpServer server;

  public:
    std::vector<QByteArray> requests;
    QByteArray body = "owned bytes";
    Origin() {
        if (!server.listen(QHostAddress::LocalHost, 0))
            throw std::runtime_error("Cannot open owned media test origin");
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto* socket = server.nextPendingConnection()) {
                auto input = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, input] {
                    *input += socket->readAll();
                    if (!input->contains("\r\n\r\n"))
                        return;
                    requests.push_back(*input);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n");
                    if (!input->startsWith("HEAD "))
                        socket->write(body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return "http://127.0.0.1:" + QString::number(server.serverPort()) + "/media.bin"; }
};
QByteArray send(const DownloadSpec& routed, const QByteArray& target, const QByteArray& host,
                const QByteArray& method = "GET", const QByteArray& extra = {}, bool validToken = true) {
    const QUrl proxy(QString::fromStdString(routed.proxy));
    QTcpSocket socket;
    socket.connectToHost(proxy.host(), static_cast<quint16>(proxy.port()));
    if (!socket.waitForConnected(2000))
        return {};
    const auto material = QByteArray::fromStdString(routed.proxyUsername + ":" + routed.proxyPassword);
    socket.write(method + " " + target + " HTTP/1.1\r\nHost: " + host + "\r\nProxy-Authorization: Basic " +
                 (validToken ? material.toBase64() : QByteArray("wrong")) +
                 "\r\nCookie: owned-session=secret\r\nAuthorization: Bearer owned-auth\r\n" + extra + "\r\n");
    QByteArray answer;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        answer += socket.readAll();
        if (socket.state() == QAbstractSocket::UnconnectedState)
            break;
        socket.waitForReadyRead(5);
    }
    return answer + socket.readAll();
}
} // namespace
TEST(MediaOriginGateway, ActualHttpRestrictsOriginAndStripsItsPrivateProxyCapability) {
    int argc = 1;
    char name[] = "MediaOriginGateway";
    char* argv[]{name, nullptr};
    QCoreApplication app(argc, argv);
    Origin source, foreign;
    DownloadSpec spec;
    spec.kind = TransferKind::Media;
    spec.url = source.url().toStdString();
    spec.cookie = "owned-session=secret";
    media_process::MediaOriginGateway gateway(spec);
    const auto routed = gateway.routedRequest();
    const auto host = QUrl(source.url()).authority().toLatin1();
    const auto answer = send(routed, source.url().toLatin1(), host);
    ASSERT_TRUE(answer.endsWith("owned bytes")) << answer.toStdString();
    ASSERT_EQ(source.requests.size(), 1u);
    EXPECT_TRUE(source.requests[0].contains("GET /media.bin HTTP/1.1"));
    EXPECT_TRUE(source.requests[0].contains("Cookie: owned-session=secret"));
    EXPECT_FALSE(source.requests[0].toLower().contains("proxy-authorization"));
    EXPECT_FALSE(source.requests[0].contains(QByteArray::fromStdString(routed.proxyPassword)));
    EXPECT_TRUE(
        send(routed, foreign.url().toLatin1(), QUrl(foreign.url()).authority().toLatin1()).contains("403"));
    EXPECT_TRUE(send(routed, source.url().toLatin1(), host, "GET", {}, false).contains("407"));
    EXPECT_TRUE(send(routed, source.url().toLatin1(), host, "POST").contains("403"));
    EXPECT_TRUE(send(routed, source.url().toLatin1(), host, "GET", "Content-Length: 1\r\n").contains("403"));
    EXPECT_TRUE(
        send(routed, source.url().toLatin1(), QUrl(foreign.url()).authority().toLatin1()).contains("403"));
    EXPECT_EQ(source.requests.size(), 1u);
    EXPECT_TRUE(foreign.requests.empty());
}
TEST(MediaOriginGateway, HttpsConnectPinsAuthorityWithoutSendingSyntheticOriginHttp) {
    int argc = 1;
    char name[] = "MediaOriginTunnel";
    char* argv[]{name, nullptr};
    QCoreApplication app(argc, argv);
    Origin source, foreign;
    auto url = QUrl(source.url());
    url.setScheme("https");
    DownloadSpec spec;
    spec.url = url.toString().toStdString();
    spec.cookie = "owned-session=secret";
    media_process::MediaOriginGateway gateway(spec);
    const auto routed = gateway.routedRequest();
    const auto host = url.authority().toLatin1();
    EXPECT_TRUE(send(routed, host, host, "CONNECT").contains("200 Connection Established"));
    EXPECT_TRUE(send(routed, QUrl(foreign.url()).authority().toLatin1(), host, "CONNECT").contains("403"));
    EXPECT_TRUE(foreign.requests.empty());
    EXPECT_TRUE(source.requests.empty()); // No TLS terminator or synthetic origin HTTP request was inserted.
}
TEST(MediaOriginGateway, ActualTlsHandshakeAndEncryptedCredentialsReachOnlyOwnedOrigin) {
    int argc = 1;
    char name[] = "MediaOriginTls";
    char* argv[]{name, nullptr};
    QCoreApplication app(argc, argv);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    ASSERT_NE(context, nullptr);
    ASSERT_EQ(EVP_PKEY_keygen_init(context.get()), 1);
    ASSERT_EQ(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048), 1);
    EVP_PKEY* generated = nullptr;
    ASSERT_EQ(EVP_PKEY_keygen(context.get(), &generated), 1);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(generated, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    ASSERT_NE(certificate, nullptr);
    ASSERT_EQ(X509_set_version(certificate.get(), 2), 1);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60);
    X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600);
    ASSERT_EQ(X509_set_pubkey(certificate.get(), key.get()), 1);
    std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> subject(X509_NAME_new(), X509_NAME_free);
    ASSERT_NE(subject, nullptr);
    ASSERT_EQ(X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC,
                                         reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0),
              1);
    ASSERT_EQ(X509_set_subject_name(certificate.get(), subject.get()), 1);
    ASSERT_EQ(X509_set_issuer_name(certificate.get(), subject.get()), 1);
    for (const auto& [nid, value] : {std::pair{NID_basic_constraints, "critical,CA:TRUE"},
                                     std::pair{NID_subject_alt_name, "IP:127.0.0.1"}}) {
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
            X509V3_EXT_conf_nid(nullptr, nullptr, nid, value), X509_EXTENSION_free);
        ASSERT_NE(extension, nullptr);
        ASSERT_EQ(X509_add_ext(certificate.get(), extension.get(), -1), 1);
    }
    ASSERT_GT(X509_sign(certificate.get(), key.get(), EVP_sha256()), 0);
    auto encode = [](const std::function<int(BIO*)>& write) {
        std::unique_ptr<BIO, decltype(&BIO_free)> memory(BIO_new(BIO_s_mem()), BIO_free);
        if (!memory || write(memory.get()) != 1)
            throw std::runtime_error("Cannot encode owned TLS fixture");
        char* bytes = nullptr;
        const auto size = BIO_get_mem_data(memory.get(), &bytes);
        return QByteArray(bytes, static_cast<qsizetype>(size));
    };
    const QSslCertificate cert(
        encode([&](BIO* memory) { return PEM_write_bio_X509(memory, certificate.get()); }));
    const QSslKey privateKey(encode([&](BIO* memory) {
                                 return PEM_write_bio_PrivateKey(memory, key.get(), nullptr, nullptr, 0,
                                                                 nullptr, nullptr);
                             }),
                             QSsl::Rsa);
    class TlsOrigin final : public QTcpServer {
      public:
        QSslCertificate cert;
        QSslKey key;
        QByteArray received;
        void incomingConnection(qintptr descriptor) override {
            auto* socket = new QSslSocket(this);
            socket->setLocalCertificate(cert);
            socket->setPrivateKey(key);
            socket->setPeerVerifyMode(QSslSocket::VerifyNone); // No client certificate is requested.
            if (!socket->setSocketDescriptor(descriptor)) {
                socket->deleteLater();
                return;
            }
            connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QSslSocket::readyRead, this, [this, socket] {
                received += socket->readAll();
                if (received.contains("\r\n\r\n")) {
                    socket->write(
                        "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nConnection: close\r\n\r\nowned bytes");
                    socket->disconnectFromHost();
                }
            });
            socket->startServerEncryption();
        }
    } source;
    source.cert = cert;
    source.key = privateKey;
    ASSERT_TRUE(source.listen(QHostAddress::LocalHost, 0));
    DownloadSpec spec;
    const auto authority = QByteArray("127.0.0.1:") + QByteArray::number(source.serverPort());
    spec.url = (QByteArray("https://") + authority + "/owned").toStdString();
    media_process::MediaOriginGateway gateway(spec);
    const auto routed = gateway.routedRequest();
    const QUrl endpoint(QString::fromStdString(routed.proxy));
    QSslSocket client;
    auto configuration = client.sslConfiguration();
    configuration.setCaCertificates({cert});
    client.setSslConfiguration(configuration);
    client.setPeerVerifyMode(QSslSocket::VerifyPeer);
    client.setPeerVerifyName("127.0.0.1");
    client.connectToHost(endpoint.host(), static_cast<quint16>(endpoint.port()));
    ASSERT_TRUE(client.waitForConnected(2000));
    client.write(
        "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\nProxy-Authorization: Basic " +
        QByteArray::fromStdString(routed.proxyUsername + ":" + routed.proxyPassword).toBase64() + "\r\n\r\n");
    auto until = [](const std::function<bool()>& condition) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!condition() && elapsed.elapsed() < 8000) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        return condition();
    };
    ASSERT_TRUE(until([&] { return client.bytesAvailable() > 0; }));
    ASSERT_TRUE(client.readAll().contains("200 Connection Established"));
    client.startClientEncryption();
    ASSERT_TRUE(until([&] { return client.isEncrypted(); })) << client.errorString().toStdString();
    EXPECT_TRUE(client.sslHandshakeErrors().empty());
    client.write("GET /owned HTTP/1.1\r\nHost: " + authority +
                 "\r\nCookie: owned-tls-secret=private\r\nConnection: close\r\n\r\n");
    QByteArray answer;
    ASSERT_TRUE(until([&] {
        answer += client.readAll();
        return answer.endsWith("owned bytes");
    }));
    EXPECT_TRUE(source.received.contains("Cookie: owned-tls-secret=private"));
    EXPECT_FALSE(source.received.toLower().contains("proxy-authorization"));
}
TEST(MediaOriginGateway, DestructionStopsIncompleteOwnedClientsWithinBoundedTime) {
    int argc = 1;
    char name[] = "MediaOriginLifetime";
    char* argv[]{name, nullptr};
    QCoreApplication app(argc, argv);
    Origin source;
    DownloadSpec spec;
    spec.url = source.url().toStdString();
    spec.cookie = "owned-session=secret";
    auto gateway = std::make_unique<media_process::MediaOriginGateway>(spec);
    const QUrl proxy(QString::fromStdString(gateway->routedRequest().proxy));
    QTcpSocket client;
    client.connectToHost(proxy.host(), static_cast<quint16>(proxy.port()));
    ASSERT_TRUE(client.waitForConnected(2000));
    client.write("GET ");
    client.waitForBytesWritten(500);
    QThread::msleep(100);
    QElapsedTimer elapsed;
    elapsed.start();
    gateway.reset();
    EXPECT_LT(elapsed.elapsed(), 3000);
    EXPECT_TRUE(source.requests.empty());
}
TEST(MediaAuthenticatedUi, ExplicitCredentialsAndRestartRequirementStayWithinTrustedDesktopFlow) {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char name[] = "MediaAuthenticatedWidgets";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(name);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    DownloadSpec spec;
    spec.kind = TransferKind::Media;
    spec.url = "https://example.test/owned.m3u8";
    spec.destination = root / L"result.mkv";
    spec.mediaDownloader = std::filesystem::path(QCoreApplication::applicationFilePath().toStdWString());
    spec.mediaMuxer = spec.mediaDownloader;
    spec.cookie = "owned-session=secret";
    spec.authorization = "Bearer owned-auth";
    spec.headers = {{"X-Media-Session", "owned-custom-header"}};
    AddDownloadDialog dialog(false);
    dialog.setSpecification(spec);
    dialog.show();
    QCoreApplication::processEvents();
    if (qEnvironmentVariableIsSet("MASKPILOT_TEST_MEDIA_AUTH_SCREENSHOT")) {
        auto* tabs = dialog.findChild<QTabWidget*>("downloadOptionsTabs");
        ASSERT_NE(tabs, nullptr);
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == "Media")
                tabs->setCurrentIndex(i);
        QCoreApplication::processEvents();
        EXPECT_TRUE(dialog.grab().save(qEnvironmentVariable("MASKPILOT_TEST_MEDIA_AUTH_SCREENSHOT")));
    }
    auto* confirm = dialog.findChild<QPushButton*>("confirmDownload");
    ASSERT_NE(confirm, nullptr);
    confirm->click();
    ASSERT_EQ(dialog.result(), QDialog::Accepted);
    const auto entered = dialog.specification();
    EXPECT_EQ(entered.cookie, spec.cookie);
    EXPECT_EQ(entered.headers, spec.headers);
    EXPECT_TRUE(entered.originAuthenticationRequired);
    std::string id;
    {
        DownloadService service(makeSqliteRepository(root / L"tasks.db"), makeUnifiedTransferFactory(),
                                root / L"parts");
        auto value = entered;
        value.startPaused = true;
        id = service.add(value).get();
    }
    DownloadService restarted(makeSqliteRepository(root / L"tasks.db"), makeUnifiedTransferFactory(),
                              root / L"parts");
    auto saved = restarted.snapshot().front();
    EXPECT_TRUE(saved.originAuthenticationRequired);
    EXPECT_TRUE(saved.spec.cookie.empty());
    EXPECT_THROW(restarted.resolveMediaInspection({saved.spec, id}).get(), std::invalid_argument);
    saved.spec.cookie = spec.cookie;
    const auto refreshed = restarted.resolveMediaInspection({saved.spec, id}).get();
    EXPECT_TRUE(refreshed.originAuthenticationRequired);
    EXPECT_EQ(refreshed.cookie, spec.cookie);
    QFile database(temporary.path() + "/tasks.db");
    ASSERT_TRUE(database.open(QIODevice::ReadOnly));
    const auto bytes = database.readAll();
    EXPECT_FALSE(bytes.contains("owned-session=secret"));
    EXPECT_FALSE(bytes.contains("Bearer owned-auth"));
    EXPECT_FALSE(bytes.contains("owned-custom-header"));
}
#ifdef MASKPILOT_AUTH_TEST_PARSER
TEST(MediaAuthenticatedUi, ActualQtInspectionAndDownloadUseConfiguredLoginWithRealComponents) {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char name[] = "ActualAuthenticatedMedia";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(name);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    const auto sourceFile = temporary.path() + "/source.mp4";
    const auto muxer = QString::fromUtf8(MASKPILOT_AUTH_TEST_MUXER);
    QProcess generate;
    generate.start(muxer, {"-v",
                           "error",
                           "-y",
                           "-f",
                           "lavfi",
                           "-i",
                           "testsrc=size=160x90:rate=10",
                           "-f",
                           "lavfi",
                           "-i",
                           "sine=frequency=700:sample_rate=48000",
                           "-t",
                           "1",
                           "-c:v",
                           "libx264",
                           "-threads",
                           "1",
                           "-pix_fmt",
                           "yuv420p",
                           "-c:a",
                           "aac",
                           sourceFile});
    ASSERT_TRUE(generate.waitForFinished(30000));
    ASSERT_EQ(generate.exitCode(), 0) << generate.readAllStandardError().toStdString();
    Origin source;
    QFile file(sourceFile);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    source.body = file.readAll();
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    DownloadService service(makeSqliteRepository(root / L"tasks.db"), makeUnifiedTransferFactory(),
                            root / L"parts");
    DownloadSpec spec;
    spec.url = source.url().toStdString();
    spec.kind = TransferKind::Media;
    spec.destination = root / L"result.mkv";
    spec.mediaDownloader = std::filesystem::path(MASKPILOT_AUTH_TEST_PARSER);
    spec.mediaMuxer = std::filesystem::path(MASKPILOT_AUTH_TEST_MUXER);
    spec.cookie = "owned_widget_session=private";
    spec.authorization = "Bearer owned-widget-authorization";
    spec.headers = {{"X-Media-Session", "owned-widget-custom"}};
    AddDownloadDialog dialog(false);
    dialog.setSpecification(spec);
    dialog.setMediaInspector(
        [&service, inspector = makeMediaInspector()](MediaInspectionRequest request, std::stop_token stop) {
            return inspector->inspect(service.resolveMediaInspection(std::move(request)).get(), stop);
        });
    int stage = 0;
    QString error;
    QTimer driver, watchdog;
    driver.setInterval(20);
    QObject::connect(&driver, &QTimer::timeout, [&] {
        auto* inspect = dialog.findChild<QPushButton*>("mediaInspect");
        if (!inspect)
            return;
        if (!stage && inspect->isEnabled()) {
            stage = 1;
            inspect->click();
        } else if (stage == 1 && inspect->isEnabled()) {
            const auto status = dialog.findChild<QLabel*>("mediaInspectionStatus")->text();
            if (!status.contains("actual formats")) {
                error = status;
                dialog.reject();
                return;
            }
            stage = 2;
            dialog.findChild<QPushButton*>("confirmDownload")->click();
        }
    });
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [&] {
        error = "Owned authenticated media UI timed out";
        dialog.reject();
    });
    driver.start();
    watchdog.start(65000);
    ASSERT_EQ(dialog.exec(), QDialog::Accepted) << error.toStdString();
    driver.stop();
    watchdog.stop();
    ASSERT_EQ(stage, 2);
    service.add(dialog.specification()).get();
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 65000) {
        QCoreApplication::processEvents();
        const auto task = service.snapshot().front();
        if (task.state == TaskState::Completed || task.state == TaskState::Failed ||
            task.state == TaskState::NeedsAttention)
            break;
        QThread::msleep(5);
    }
    const auto task = service.snapshot().front();
    ASSERT_EQ(task.state, TaskState::Completed) << task.error;
    EXPECT_TRUE(task.originAuthenticationRequired);
    ASSERT_FALSE(source.requests.empty());
    for (const auto& request : source.requests) {
        EXPECT_TRUE(request.contains("owned_widget_session=private"));
        EXPECT_TRUE(request.contains("Bearer owned-widget-authorization"));
        EXPECT_TRUE(request.contains("X-Media-Session: owned-widget-custom"));
        EXPECT_FALSE(request.toLower().contains("proxy-authorization"));
    }
    EXPECT_GT(QFileInfo(QString::fromStdWString(spec.destination.wstring())).size(), 1000);
}
#endif
