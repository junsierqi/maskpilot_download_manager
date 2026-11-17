#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QTranslator>
#include <QUuid>
#include <QtEndian>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/media_tools.h>
#include <maskpilot/ui/main_window.h>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName("MaskPilot");
    QCoreApplication::setOrganizationName("MaskPilot");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.setApplicationDescription("MaskPilot native download manager");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"data-dir", "Use an isolated application data directory.", "directory"});
    parser.addOption({"screenshot", "Save a real Qt window rendering, then exit.", "png"});
    parser.addOption({"smoke-test", "Start the complete desktop event loop and exit."});
    parser.addOption({"background", "Start in background for browser integration."});
    parser.addOption({"url", "Open the new download dialog with a URL.", "url"});
    parser.process(application);
    const auto data = parser.isSet("data-dir")
                          ? QDir(parser.value("data-dir")).absolutePath()
                          : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(data)) {
        QMessageBox::critical(nullptr, "MaskPilot", "Cannot create application data directory.");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, data);
    QLockFile lock(QDir(data).filePath("instance.lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(100)) {
        if (!parser.isSet("background"))
            QMessageBox::information(nullptr, "MaskPilot",
                                     "MaskPilot is already running. Open it from the system tray.");
        return 0;
    }
    try {
        QSettings settings;
        QTranslator qtTranslation;
        if (settings.value("language", QLocale::system().language() == QLocale::Chinese ? "zh" : "en")
                    .toString() == "zh" &&
            qtTranslation.load("qtbase_zh_CN", QCoreApplication::applicationDirPath() + "/translations"))
            application.installTranslator(&qtTranslation);
        for (const auto& pair :
             {std::pair<QString, QString>{"mediaDownloader", "yt-dlp"}, {"mediaMuxer", "ffmpeg"}}) {
            if (QFileInfo(settings.value(pair.first).toString()).isExecutable())
                continue;
            auto found = QStandardPaths::findExecutable(pair.second);
            for (const auto& base :
                 {QCoreApplication::applicationDirPath() + "/tools",
                  QCoreApplication::applicationDirPath() + "/../../tools", data + "/tools"})
                if (found.isEmpty() && QFileInfo(QDir(base).filePath(pair.second + ".exe")).isExecutable())
                    found = QDir(base).filePath(pair.second + ".exe");
            if (!found.isEmpty())
                settings.setValue(pair.first, QFileInfo(found).absoluteFilePath());
        }
        const auto token = (QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122()).toHex();
        QSaveFile tokenFile(QDir(data).filePath("ipc.token"));
        if (!tokenFile.open(QIODevice::WriteOnly) || tokenFile.write(token) != token.size() ||
            !tokenFile.commit())
            throw std::runtime_error("Cannot create local authentication token.");
        QFile::setPermissions(QDir(data).filePath("ipc.token"),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const auto directory = std::filesystem::path(data.toStdWString());
        maskpilot::DownloadService service(maskpilot::makeSqliteRepository(directory / L"tasks.db"),
                                           maskpilot::makeUnifiedTransfer, directory / L"parts");
        maskpilot::MainWindow window(service);
        if (!parser.isSet("background"))
            window.show();
        else
            application.setQuitOnLastWindowClosed(false);
        const auto name = "maskpilot-" + QCryptographicHash::hash(QDir(data).absolutePath().toUtf8(),
                                                                  QCryptographicHash::Sha256)
                                             .toHex()
                                             .left(24);
        QLocalServer::removeServer(name);
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(name))
            throw std::runtime_error("Cannot open the local download bridge.");
        QObject::connect(&server, &QLocalServer::newConnection, &window, [&] {
            while (server.hasPendingConnections()) {
                auto* client = server.nextPendingConnection();
                client->setParent(&server);
                auto buffer = std::make_shared<QByteArray>();
                auto processed = std::make_shared<bool>(false);
                QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
                QTimer::singleShot(10000, client, [client] { client->abort(); });
                auto consume = [&, client, buffer, processed] {
                    if (*processed)
                        return;
                    *buffer += client->readAll();
                    if (buffer->size() > 1024 * 1024 + 4) {
                        client->abort();
                        return;
                    }
                    if (buffer->size() < 4)
                        return;
                    const auto length =
                        qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(buffer->constData()));
                    if (length == 0 || length > 1024 * 1024) {
                        client->abort();
                        return;
                    }
                    if (buffer->size() < static_cast<qsizetype>(length) + 4)
                        return;
                    *processed = true;
                    auto document = QJsonDocument::fromJson(buffer->mid(4, length));
                    auto request = document.object();
                    auto reply = [guard = QPointer<QLocalSocket>(client)](QJsonObject response) {
                        if (!guard)
                            return;
                        const auto payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
                        if (payload.size() > 1024 * 1024) {
                            guard->abort();
                            return;
                        }
                        const auto size = qToLittleEndian(static_cast<quint32>(payload.size()));
                        guard->write(reinterpret_cast<const char*>(&size), 4);
                        guard->write(payload);
                        guard->flush();
                        guard->disconnectFromServer();
                    };
                    if (!document.isObject() || request.value("token").toString().toLatin1() != token) {
                        reply({{"version", 1},
                               {"requestId", request.value("requestId")},
                               {"ok", false},
                               {"error", "Local authentication failed."}});
                        return;
                    }
                    request.remove("token");
                    window.handleNative(request, std::move(reply));
                };
                QObject::connect(client, &QLocalSocket::readyRead, &window, consume);
                if (client->bytesAvailable() > 0)
                    QTimer::singleShot(0, client, consume);
            }
        });
        if (parser.isSet("url"))
            QTimer::singleShot(0, &window, [&] { window.newDownload(parser.value("url")); });
        if (parser.isSet("screenshot"))
            QTimer::singleShot(1200, &window, [&] {
                const bool saved = window.grab().save(parser.value("screenshot"));
                window.requestQuit();
                application.exit(saved ? 0 : 2);
            });
        else if (parser.isSet("smoke-test"))
            QTimer::singleShot(1500, &window, &maskpilot::MainWindow::requestQuit);
        return application.exec();
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "MaskPilot", QString::fromUtf8(e.what()));
        return 1;
    }
}
