#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <iostream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
constexpr quint32 kMaxMessage = 1024 * 1024;
void respond(QJsonObject message) {
    const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    auto length = qToLittleEndian(static_cast<quint32>(bytes.size()));
    std::cout.write(reinterpret_cast<const char*>(&length), 4);
    std::cout.write(bytes.constData(), bytes.size());
    std::cout.flush();
}
QString pipeName(const QString& data) {
    return "maskpilot-" +
           QCryptographicHash::hash(QDir(data).absolutePath().toUtf8(), QCryptographicHash::Sha256)
               .toHex()
               .left(24);
}
bool hexadecimal(const QByteArray& value, qsizetype length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}
bool ordinary(const QString& command) {
    return command == "ping" || command == "prepare" || command == "add" || command == "list" ||
           command == "resume" || command == "pause" || command == "cancel" || command == "remove";
}
bool administrative(const QString& command) {
    return command == "context.configure" || command == "context.list" || command == "context.issue" ||
           command == "origin.grant" || command == "context.revoke" || command == "task.rebind";
}
} // namespace
int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilot");
    QCoreApplication::setApplicationName("MaskPilot");
    const auto arguments = application.arguments();
    const bool broker = arguments.size() > 1 && arguments[1] == "--broker";
    QString brokerDirectory;
    if (broker) {
        if (arguments.size() != 4 || arguments[2] != "--data-dir" || !QDir::isAbsolutePath(arguments[3]) ||
            std::any_of(arguments[3].begin(), arguments[3].end(),
                        [](QChar ch) { return ch.unicode() < 0x20; })) {
            respond({{"version", 2},
                     {"requestId", ""},
                     {"ok", false},
                     {"error", "Invalid administrative broker invocation."}});
            return 1;
        }
        brokerDirectory = QDir(arguments[3]).absolutePath();
    }
    QFile configFile(QDir(application.applicationDirPath()).filePath("native-bridge.json"));
    QJsonObject config;
    if (configFile.open(QIODevice::ReadOnly) && configFile.size() < 65536)
        config = QJsonDocument::fromJson(configFile.readAll()).object();
    if (!broker) {
        bool allowed = false;
        const auto origin = arguments.size() > 1 ? arguments[1] : QString{};
        if (arguments.size() == 2 || arguments.size() == 3)
            for (const auto& entry : config.value("allowedOrigins").toArray())
                if (entry.isString() && entry.toString() == origin &&
                    origin.startsWith("chrome-extension://"))
                    allowed = true;
        if (!allowed) {
            respond({{"version", 1},
                     {"requestId", ""},
                     {"ok", false},
                     {"error", "Browser origin is not registered. Run browser integration setup."}});
            return 1;
        }
    }
    std::array<char, 4> header{};
    if (!std::cin.read(header.data(), header.size()))
        return 2;
    const auto length = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header.data()));
    if (length == 0 || length > kMaxMessage) {
        respond({{"version", broker ? 2 : 1},
                 {"requestId", ""},
                 {"ok", false},
                 {"error", "Native message size is invalid."}});
        return 2;
    }
    QByteArray payload(static_cast<qsizetype>(length), '\0');
    if (!std::cin.read(payload.data(), length))
        return 2;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    auto request = document.object();
    const auto versionValue = request.value("version");
    const auto version = versionValue.toDouble() == 2 ? 2 : 1;
    const auto rawId = request.value("requestId").toString();
    const auto requestId = hexadecimal(rawId.toLatin1(), 32) ? rawId : QString{};
    auto fail = [&](const QString& message) {
        respond({{"version", version}, {"requestId", requestId}, {"ok", false}, {"error", message}});
        return 1;
    };
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || !versionValue.isDouble() ||
        (versionValue.toDouble() != 1 && versionValue.toDouble() != 2) || requestId.isEmpty() ||
        !request.value("command").isString())
        return fail("Invalid native protocol message.");
    const auto command = request.value("command").toString();
    QByteArray forwardedToken;
    if (broker) {
        if (version != 2 || !administrative(command))
            return fail("Administrative broker only accepts version 2 administrative commands.");
    } else {
        if (!ordinary(command))
            return fail("Browser clients cannot issue administrative commands.");
        if (version == 2) {
            if (!request.value("sessionToken").isString())
                return fail("A scoped session token is required.");
            forwardedToken = request.value("sessionToken").toString().toLatin1();
            if (!hexadecimal(forwardedToken, 32))
                return fail("The scoped session token is invalid.");
        }
    }
    // Authority comes exclusively from invocation mode or the issued bearer.
    // Caller master-token fields are never forwarded, adopted or used as fallback.
    request.remove("token");
    request.remove("sessionToken");
    const auto data =
        broker ? brokerDirectory
               : QDir(config.value("dataDirectory")
                          .toString(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)))
                     .absolutePath();
    const auto desktop = config.value("desktopPath")
                             .toString(QDir(application.applicationDirPath()).filePath("maskpilot.exe"));
    QLocalSocket socket;
    bool launched = false;
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < 9500) {
        socket.abort();
        socket.connectToServer(pipeName(data));
        if (socket.waitForConnected(200))
            break;
        if (!launched) {
            if (!QProcess::startDetached(desktop, {"--background", "--data-dir", data}))
                return fail("Cannot start MaskPilot desktop.");
            launched = true;
        }
        QThread::msleep(100);
    }
    if (socket.state() != QLocalSocket::ConnectedState)
        return fail("Cannot connect to MaskPilot desktop.");
    if (broker || version == 1) {
        // Ordinary version 2 never opens either administrative or legacy token file.
        QFile tokenFile(QDir(data).filePath(broker ? "ipc-admin.token" : "ipc.token"));
        if (!tokenFile.open(QIODevice::ReadOnly) || tokenFile.size() > 128)
            return fail("Desktop authentication token is unavailable.");
        forwardedToken = tokenFile.readAll().trimmed();
        if (!hexadecimal(forwardedToken, 64))
            return fail("Desktop authentication token is invalid.");
    }
    request.insert("token", QString::fromLatin1(forwardedToken));
    const auto body = QJsonDocument(request).toJson(QJsonDocument::Compact);
    if (body.size() > kMaxMessage)
        return fail("Native message exceeded its size limit.");
    const auto size = qToLittleEndian(static_cast<quint32>(body.size()));
    socket.write(reinterpret_cast<const char*>(&size), 4);
    socket.write(body);
    if (!socket.waitForBytesWritten(2000))
        return fail("Cannot send request to desktop.");
    QByteArray response;
    QElapsedTimer replyTimeout;
    replyTimeout.start();
    const auto replyBudget = broker ? 60000 : 10000;
    while (replyTimeout.elapsed() < replyBudget) {
        if (socket.bytesAvailable() == 0)
            socket.waitForReadyRead(200);
        response += socket.readAll();
        if (response.size() > static_cast<qsizetype>(kMaxMessage) + 4)
            return fail("Desktop response exceeded size limit.");
        if (response.size() >= 4) {
            const auto expected =
                qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(response.constData()));
            if (expected == 0 || expected > kMaxMessage)
                return fail("Invalid desktop response size.");
            if (response.size() >= static_cast<qsizetype>(expected) + 4) {
                const auto parsed = QJsonDocument::fromJson(response.mid(4, expected));
                const auto answer = parsed.object();
                if (!parsed.isObject() || answer.value("requestId").toString() != requestId ||
                    answer.value("version").toDouble() != version || !answer.value("ok").isBool())
                    return fail("Invalid desktop response.");
                respond(answer);
                return answer.value("ok").toBool() ? 0 : 1;
            }
        }
        if (socket.state() == QLocalSocket::UnconnectedState && socket.bytesAvailable() == 0)
            break;
    }
    return fail("Desktop acknowledgement timed out. Original browser download can be resumed.");
}
