#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <chrono>
#include <iostream>
#include <iterator>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/media_tools.h>
#include <maskpilot/ui/add_download_dialog.h>
#include <maskpilot/ui/main_window.h>
#include <string_view>
#include <thread>

using namespace maskpilot;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T* control(QWidget* widget, const char* name) {
    auto* result = widget->findChild<T*>(name);
    require(result != nullptr, "Missing actual media control");
    return result;
}
QByteArray run(const QString& program, const QStringList& arguments, const QString& workingDirectory = {}) {
    QProcess process;
    if (!workingDirectory.isEmpty())
        process.setWorkingDirectory(workingDirectory);
    process.start(program, arguments);
    require(process.waitForStarted(5000), "Cannot launch owned media test component");
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(5000);
        throw std::runtime_error("Owned media test component timed out");
    }
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
            "Owned media test component failed");
    return process.readAllStandardOutput();
}
class Origin final : public QObject {
    QTcpServer server_;
    QMap<QString, QByteArray> files_;

  public:
    explicit Origin(const QString& directory) {
        for (const auto& path : QDir(directory).entryList(QDir::Files)) {
            QFile file(QDir(directory).filePath(path));
            require(file.open(QIODevice::ReadOnly), "Cannot read generated media fixture");
            files_.insert("/" + path, file.readAll());
        }
        require(server_.listen(QHostAddress::LocalHost, 0), "Cannot bind owned media origin");
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server_.nextPendingConnection()) {
                auto input = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, input] {
                    if (socket->property("answered").toBool())
                        return;
                    input->append(socket->readAll());
                    if (input->size() > 65536) {
                        socket->abort();
                        return;
                    }
                    if (!input->contains("\r\n\r\n"))
                        return;
                    socket->setProperty("answered", true);
                    const auto words = input->left(input->indexOf("\r\n")).split(' ');
                    if (words.size() != 3) {
                        socket->abort();
                        return;
                    }
                    const auto path = QUrl::fromEncoded(words[1]).path();
                    const auto found = files_.constFind(path);
                    if (found == files_.cend()) {
                        socket->write(
                            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    auto begin = qsizetype{0}, end = found->size() - 1;
                    const QRegularExpression range("(?im)^Range: bytes=(\\d+)-(\\d*)\\r?$");
                    const auto match = range.match(QString::fromLatin1(*input));
                    if (match.hasMatch()) {
                        begin = match.captured(1).toLongLong();
                        if (!match.captured(2).isEmpty())
                            end = match.captured(2).toLongLong();
                    }
                    if (begin < 0 || end < begin || end >= found->size()) {
                        socket->abort();
                        return;
                    }
                    const auto body = found->mid(begin, end - begin + 1);
                    QByteArray headers =
                        match.hasMatch() ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
                    headers += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
                    headers += path.endsWith(".mpd") ? "Content-Type: application/dash+xml\r\n"
                                                     : "Content-Type: video/mp4\r\n";
                    if (match.hasMatch())
                        headers += "Content-Range: bytes " + QByteArray::number(begin) + "-" +
                                   QByteArray::number(end) + "/" + QByteArray::number(found->size()) + "\r\n";
                    headers += "Connection: close\r\n\r\n";
                    socket->write(headers);
                    if (words[0] != "HEAD")
                        socket->write(body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QString url() const {
        return "http://127.0.0.1:" + QString::number(server_.serverPort()) + "/manifest.mpd";
    }
};
} // namespace
int main(int argc, char** argv) {
    const auto helperMode = qEnvironmentVariable("MASKPILOT_TEST_INSPECTOR_MODE");
    if (!helperMode.isEmpty()) {
        std::string privateInput((std::istreambuf_iterator<char>(std::cin)), {});
        if (helperMode == "malformed")
            std::cout << "{invalid-json";
        else if (helperMode == "oversize")
            std::cout << std::string(9 * 1024 * 1024, 'x');
        else if (helperMode == "cancel")
            std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    try {
        const auto parser = qEnvironmentVariable("MASKPILOT_MEDIA_TEST_PARSER");
        const auto muxer = qEnvironmentVariable("MASKPILOT_MEDIA_TEST_MUXER");
        require(QFileInfo(parser).isExecutable() && QFileInfo(muxer).isExecutable(),
                "Set actual supported media test tool paths");
        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create owned media system fixture");
        DownloadSpec invalidFixture;
        invalidFixture.kind = TransferKind::Media;
        invalidFixture.url = "http://127.0.0.1/owned-unused-source";
        invalidFixture.destination = std::filesystem::path(temporary.path().toStdWString()) / L"unused.mkv";
        invalidFixture.mediaDownloader =
            std::filesystem::path(QCoreApplication::applicationFilePath().toStdWString());
        invalidFixture.mediaMuxer = std::filesystem::path(muxer.toStdWString());
        for (const auto* mode : {"malformed", "oversize", "cancel"}) {
            qputenv("MASKPILOT_TEST_INSPECTOR_MODE", mode);
            std::stop_source cancellation;
            std::jthread timer;
            if (std::string_view(mode) == "cancel")
                timer = std::jthread([&] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    cancellation.request_stop();
                });
            bool rejected = false;
            const auto started = std::chrono::steady_clock::now();
            try {
                makeMediaInspector()->inspect(invalidFixture, cancellation.get_token());
            } catch (const std::exception&) {
                rejected = true;
            }
            qunsetenv("MASKPILOT_TEST_INSPECTOR_MODE");
            require(rejected, "Owned invalid helper output was accepted");
            require(std::chrono::steady_clock::now() - started < std::chrono::seconds(8),
                    "Owned invalid/cancelled helper did not stop within its fixture budget");
        }
        std::cout << "PASS actual owned helper malformed/oversized output and process-tree cancellation\n";
        const auto assets = temporary.path() + "/assets";
        require(QDir().mkpath(assets), "Cannot create generated media assets");
        run(muxer,
            {"-v",
             "error",
             "-y",
             "-f",
             "lavfi",
             "-i",
             "testsrc=size=320x180:rate=10",
             "-f",
             "lavfi",
             "-i",
             "sine=frequency=700:sample_rate=48000",
             "-t",
             "2",
             "-map",
             "0:v",
             "-map",
             "0:v",
             "-map",
             "1:a",
             "-filter:v:0",
             "scale=160:90",
             "-filter:v:1",
             "scale=320:180",
             "-c:v",
             "libx264",
             "-threads",
             "1",
             "-pix_fmt",
             "yuv420p",
             "-b:v:0",
             "100k",
             "-b:v:1",
             "220k",
             "-c:a",
             "aac",
             "-use_template",
             "1",
             "-use_timeline",
             "1",
             "-seg_duration",
             "1",
             "-adaptation_sets",
             "id=0,streams=v id=1,streams=a",
             "-f",
             "dash",
             "manifest.mpd"},
            assets);
        Origin origin(assets);
        QCoreApplication::setOrganizationName("MaskPilotTests");
        QCoreApplication::setApplicationName("ActualMediaSystem");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings settings;
        settings.setValue("language", "en");
        settings.setValue("downloadDirectory", temporary.path());
        settings.setValue("closeToTray", false);
        settings.setValue("mediaDownloader", parser);
        settings.setValue("mediaMuxer", muxer);
        const auto root = std::filesystem::path(temporary.path().toStdWString());
        auto store = makeSqliteStore(root / L"tasks.db");
        DownloadService service(
            std::move(store.tasks), makeUnifiedTransferFactory(), root / L"parts", {},
            QueueDependencies{std::move(store.queues), makeQueueCivilTime(), {}, std::move(store.traffic)});
        MainWindow window(service);
        window.setMediaInspector(
            [&service, inspector = makeMediaInspector()](MediaInspectionRequest input, std::stop_token stop) {
                return inspector->inspect(service.resolveMediaInspection(std::move(input)).get(), stop);
            });
        window.show();
        int stage = 0;
        std::string failure;
        QTimer driver;
        driver.setInterval(25);
        QObject::connect(&driver, &QTimer::timeout, &window, [&] {
            try {
                auto* modal = QApplication::activeModalWidget();
                if (!failure.empty()) {
                    if (auto* dialog = qobject_cast<QDialog*>(modal))
                        dialog->reject();
                    return;
                }
                if (auto* warning = qobject_cast<QMessageBox*>(modal)) {
                    failure = warning->text().toStdString();
                    warning->accept();
                    return;
                }
                auto* dialog = dynamic_cast<AddDownloadDialog*>(modal);
                if (!dialog)
                    return;
                if (stage == 0) {
                    DownloadSpec spec;
                    spec.kind = TransferKind::Media;
                    spec.url = origin.url().toStdString();
                    spec.destination = root / L"selected.mkv";
                    spec.mediaDownloader = std::filesystem::path(parser.toStdWString());
                    spec.mediaMuxer = std::filesystem::path(muxer.toStdWString());
                    dialog->setSpecification(spec);
                    auto* tabs = control<QTabWidget>(dialog, "downloadOptionsTabs");
                    tabs->setCurrentIndex(tabs->count() - 1);
                    control<QComboBox>(dialog, "mediaSelectionMode")->setCurrentIndex(3);
                    stage = 1;
                    control<QPushButton>(dialog, "mediaInspect")->click();
                } else if (stage == 1) {
                    auto* video = control<QComboBox>(dialog, "mediaPrimaryFormat");
                    auto* audio = control<QComboBox>(dialog, "mediaAudioFormat");
                    int selected = -1;
                    for (int n = 1; n < video->count(); ++n)
                        if (video->itemText(n).contains("160×90"))
                            selected = n;
                    if (selected < 0 || audio->count() < 2) {
                        if (control<QPushButton>(dialog, "mediaInspect")->isEnabled())
                            throw std::runtime_error(
                                "Actual inventory did not offer the generated video/audio formats");
                        return;
                    }
                    video->setCurrentIndex(selected);
                    audio->setCurrentIndex(1);
                    auto* container = control<QComboBox>(dialog, "mediaContainer");
                    container->setCurrentIndex(container->findData(static_cast<int>(MediaContainer::Mkv)));
                    const auto capture = qEnvironmentVariable("MASKPILOT_TEST_MEDIA_SCREENSHOT");
                    auto* status = control<QLabel>(dialog, "mediaInspectionStatus");
                    require(status->height() >= status->heightForWidth(status->width()),
                            "Actual media inspection status is clipped");
                    if (!capture.isEmpty())
                        require(dialog->grab().save(capture), "Cannot capture real media selection");
                    stage = 2;
                    control<QPushButton>(dialog, "confirmDownload")->click();
                }
            } catch (const std::exception& error) {
                failure = error.what();
                if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                    modal->reject();
            }
        });
        QTimer modalDeadline;
        modalDeadline.setSingleShot(true);
        QObject::connect(&modalDeadline, &QTimer::timeout, &window, [&] {
            failure = "Actual Qt media system fixture exceeded its 75-second deadline";
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        modalDeadline.start(75000);
        driver.start();
        QTimer::singleShot(0, &window, [&] { window.newDownload(origin.url()); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(75);
        bool complete = false;
        while (std::chrono::steady_clock::now() < deadline && failure.empty()) {
            QCoreApplication::processEvents();
            const auto tasks = service.snapshot();
            if (tasks.size() == 1) {
                if (tasks[0].state == TaskState::Completed) {
                    complete = true;
                    break;
                }
                if (tasks[0].state == TaskState::Failed || tasks[0].state == TaskState::NeedsAttention) {
                    failure = tasks[0].error;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        driver.stop();
        modalDeadline.stop();
        if (!failure.empty())
            throw std::runtime_error(failure);
        require(stage == 2 && complete, "Actual Qt format selection/download timed out");
        auto probe = QFileInfo(muxer).dir().filePath("ffprobe.exe");
#ifndef _WIN32
        probe = QFileInfo(muxer).dir().filePath("ffprobe");
#endif
        const auto document = QJsonDocument::fromJson(
            run(probe, {"-v", "error", "-show_streams", "-of", "json", temporary.path() + "/selected.mkv"}));
        bool video = false, audio = false;
        for (const auto& value : document.object().value("streams").toArray()) {
            const auto stream = value.toObject();
            if (stream.value("codec_type").toString() == "video")
                video = stream.value("width").toInt() == 160 && stream.value("height").toInt() == 90;
            if (stream.value("codec_type").toString() == "audio")
                audio = true;
        }
        require(video && audio, "Selected real formats did not produce requested video/audio streams");
        window.requestQuit();
        std::cout << "PASS actual Qt inventory -> explicit 160x90 video + audio -> native media output\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
