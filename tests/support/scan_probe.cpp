#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QThread>
#include <array>
#include <csignal>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 2)
        return 2;
    auto value = [&](const QString& key, const QString& fallback = {}) {
        const auto index = args.indexOf(key);
        return index > 0 && index + 1 < args.size() ? args[index + 1] : fallback;
    };
    const auto mode = value("--mode", "pass");
    const auto path = args.back();
    QJsonObject record{{"path", path},
                       {"mode", mode},
                       {"arguments", QJsonArray::fromStringList(args.mid(1))},
                       {"finalExists", QFileInfo::exists(value("--final"))},
                       {"directory", QFileInfo(path).isDir()}};
    record.insert("privateEnvironmentPresent", qEnvironmentVariableIsSet("MASKPILOT_SCAN_PRIVATE_ENV"));
    QJsonArray files;
    auto inspect = [&](const QString& name) {
        QFile file(name);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file))
            return false;
        files.append(QJsonObject{{"path", name}, {"sha256", QString::fromLatin1(hash.result().toHex())}});
        return true;
    };
    if (QFileInfo(path).isDir()) {
        QDirIterator iterator(path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (iterator.hasNext())
            if (!inspect(iterator.next()))
                return 3;
    } else if (!inspect(path))
        return 3;
    record.insert("files", files);
    const auto output = value("--record");
    if (!output.isEmpty()) {
        QFile file(output);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(record).toJson(QJsonDocument::Compact)) < 0 || !file.flush())
            return 4;
    }
    const auto childRecord = value("--child-record");
    if (!childRecord.isEmpty() && mode == "sleep") {
        QJsonObject identity{{"pid", static_cast<qint64>(QCoreApplication::applicationPid())},
                             {"program", QCoreApplication::applicationFilePath()}};
#ifdef _WIN32
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            return 5;
        const auto stamp = (static_cast<qulonglong>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        identity.insert("created", QString::number(stamp));
#elif defined(__linux__)
        QFile stat("/proc/self/stat");
        if (!stat.open(QIODevice::ReadOnly))
            return 5;
        const auto bytes = stat.readAll();
        const auto fields = bytes.mid(bytes.lastIndexOf(')') + 2).split(' ');
        if (fields.size() <= 19)
            return 5;
        identity.insert("created", QString::fromLatin1(fields[19]));
#endif
        QFile file(childRecord);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(identity).toJson(QJsonDocument::Compact)) < 0 || !file.flush())
            return 5;
    }
    if (mode == "spawn") {
        QProcess child;
        child.setStandardOutputFile(QProcess::nullDevice());
        child.setStandardErrorFile(QProcess::nullDevice());
        child.start(QCoreApplication::applicationFilePath(),
                    {"--mode", "sleep", "--child-record", childRecord, path});
        if (!child.waitForStarted(5000))
            return 6;
        for (;;)
            QThread::msleep(50);
    }
    if (mode == "sleep")
        for (;;)
            QThread::msleep(50);
    if (mode == "reject")
        return 23;
    if (mode == "mutate") {
        QFile file(path);
        if (!file.open(QIODevice::Append) || file.write("changed") != 7)
            return 7;
    }
    if (mode == "flood") {
        std::array<char, 65536> bytes{};
        for (int count = 0; count < 256; ++count) {
            fwrite(bytes.data(), 1, bytes.size(), stdout);
            fwrite(bytes.data(), 1, bytes.size(), stderr);
        }
    }
    if (mode == "crash") {
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 0xC0000005);
#else
        std::raise(SIGSEGV);
        return 139;
#endif
    }
    return 0;
}
