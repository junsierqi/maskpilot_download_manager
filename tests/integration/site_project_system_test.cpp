#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <maskpilot/application/site_project_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/ui/main_window.h>
#include <maskpilot/ui/site_project_dialog.h>
#include <stdexcept>
#include <type_traits>
using namespace maskpilot;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void wait(const std::function<bool()>& predicate, int milliseconds = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > milliseconds)
            throw std::runtime_error("Actual project UI condition timed out.");
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}
template <class T> T finish(std::future<T> future) {
    wait([&] { return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
    if constexpr (std::is_void_v<T>) {
        future.get();
        return;
    } else
        return future.get();
}
struct Clock final : IQueueCivilTime {
    std::atomic<std::int64_t> utc{0};
    std::int64_t utcNow() const override { return utc.load(); }
    std::optional<CivilMinute> at(const std::string& zone, std::int64_t instant) const override {
        if (zone != "UTC")
            return std::nullopt;
        return CivilMinute{0, static_cast<int>((instant / 60) % 1440)};
    }
};
struct Origin {
    QTcpServer server;
    int requests = 0, images = 0, version = 1;
    Origin() {
        require(server.listen(QHostAddress::LocalHost, 0), "Cannot bind owned HTTP server");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto buffer = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", buffer);
                    if (!buffer.contains("\r\n\r\n") || socket->property("answered").toBool())
                        return;
                    socket->setProperty("answered", true);
                    auto line = buffer.split('\n').front().trimmed().split(' ');
                    if (line.size() < 2)
                        return;
                    auto path = line[1];
                    ++requests;
                    if (path == "/docs") {
                        socket->write("HTTP/1.1 301 Moved\r\nLocation: /docs/index.html\r\nContent-Length: "
                                      "0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    QByteArray body, mime = "text/html; charset=UTF-8";
                    if (path == "/docs/index.html")
                        body =
                            "<html><head><title>Project UI v" + QByteArray::number(version) +
                            "</title><link rel=\"stylesheet\" href=\"../style.css\"></head><body><h1>Actual "
                            "UI</h1><a href=\"page\">Page</a><img src=\"../image.png\"></body></html>";
                    else if (path == "/docs/page")
                        body = "<html><body><a href=\"index.html\">Home</a></body></html>";
                    else if (path == "/style.css") {
                        body = "body{background:#eef;color:#123}";
                        mime = "text/css; charset=UTF-8";
                    } else if (path == "/image.png") {
                        ++images;
                        QImage image(1, 1, QImage::Format_ARGB32);
                        image.fill(QColor(51, 102, 153));
                        QBuffer imageBuffer(&body);
                        imageBuffer.open(QIODevice::WriteOnly);
                        require(image.save(&imageBuffer, "PNG"), "Cannot generate owned PNG fixture");
                        mime = "image/png";
                    } else {
                        socket->write(
                            "HTTP/1.1 404 Missing\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    qint64 begin = 0, end = body.size() - 1;
                    bool ranged = false;
                    for (auto header : buffer.split('\n'))
                        if (header.trimmed().toLower().startsWith("range: bytes=")) {
                            auto parts = header.trimmed().mid(13).split('-');
                            if (parts.size() == 2) {
                                begin = parts[0].toLongLong();
                                end = std::min<qint64>(parts[1].toLongLong(), body.size() - 1);
                                ranged = true;
                            }
                        }
                    auto selected = body.mid(begin, end - begin + 1);
                    QByteArray response = ranged ? "HTTP/1.1 206 Partial\r\n" : "HTTP/1.1 200 OK\r\n";
                    response += "Content-Type: " + mime +
                                "\r\nContent-Length: " + QByteArray::number(selected.size()) +
                                "\r\nETag: \"ui-v" + QByteArray::number(version) +
                                "\"\r\nConnection: close\r\n";
                    if (ranged)
                        response += "Content-Range: bytes " + QByteArray::number(begin) + "-" +
                                    QByteArray::number(end) + "/" + QByteArray::number(body.size()) + "\r\n";
                    socket->write(response + "\r\n" + selected);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return QString("http://127.0.0.1:%1/docs").arg(server.serverPort()); }
};
QPushButton* button(QWidget& widget, const char* name) {
    auto* value = widget.findChild<QPushButton*>(name);
    require(value, "Missing product project action");
    return value;
}
SiteProjectState state(SiteProjectService& service, const std::string& id) {
    auto value = service.inspect(id);
    require(value.has_value(), "Project disappeared");
    return value->state;
}
std::string create(SiteProjectDialog& dialog, SiteProjectService& service, const QString& url,
                   const QString& output, const std::string& queue) {
    auto before = service.snapshot().size();
    button(dialog, "siteProjectNew")->click();
    QDialog* create = nullptr;
    wait([&] {
        for (auto* w : QApplication::topLevelWidgets())
            if (w->objectName() == "siteProjectCreateDialog")
                create = qobject_cast<QDialog*>(w);
        return create;
    });
    auto* root = create->findChild<QLineEdit*>("siteProjectRootUrl");
    auto* directory = create->findChild<QLineEdit*>("siteProjectOutputDirectory");
    auto* queueBox = create->findChild<QComboBox*>("siteProjectQueue");
    require(root && directory && queueBox, "Missing actual creation fields");
    root->setText(url);
    directory->setText(output);
    auto index = queueBox->findData(QString::fromStdString(queue));
    require(index >= 0, "Target queue absent from project options");
    queueBox->setCurrentIndex(index);
    button(*create, "siteProjectCreate")->click();
    wait([&] { return service.snapshot().size() == before + 1; });
    auto values = service.snapshot();
    std::string id;
    for (auto& s : values)
        if (s.state == SiteProjectState::Draft)
            id = s.id;
    require(!id.empty(), "No durable Draft after actual create click");
    wait([&] { return button(dialog, "siteProjectPreview")->isEnabled(); });
    return id;
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create owned project workspace");
        QCoreApplication::setOrganizationName("MaskPilotProjectSystemFixture");
        QCoreApplication::setApplicationName("OwnedProjectDialog");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings settings;
        settings.setValue("language", "en");
        settings.setValue("closeToTray", false);
        settings.setValue("completionAction", 0);
        settings.sync();
        Origin origin;
        auto root = std::filesystem::path(temporary.path().toStdWString());
        auto clock = std::make_shared<Clock>();
        auto store = makeSqliteStore(root / L"tasks.db");
        auto traffic = store.traffic;
        DownloadService downloads(
            std::move(store.tasks), makeUnifiedTransferFactory(), root / L"parts", {},
            QueueDependencies{std::move(store.queues), clock, InitialQueueLimits{3, 0, 0}, traffic});
        auto policies = finish(downloads.queues());
        auto normal = policies.front();
        QueueConfiguration closed;
        closed.id = normal.id;
        closed.name = normal.name;
        closed.expectedRevision = normal.revision;
        closed.schedule = {true, 127, 1, 2, "UTC"};
        finish(downloads.configureQueue(closed));
        QueueConfiguration limited;
        limited.name = "Project budget";
        limited.dailyPayloadBudget = 1;
        auto budget = finish(downloads.configureQueue(limited));
        SiteProjectService projects(downloads, makeSiteProjectRepository(root / L"site-projects"),
                                    makeSiteContentProcessor(), makeSiteProjectPublisher(),
                                    [&] { return clock->utc.load(); });
        require(projects.startupError().empty(), "Project startup rejected owned repository");
        std::vector<std::filesystem::path> opened;
        auto opener = [&](const auto& path) { opened.push_back(path); };
        MainWindow window(downloads);
        window.setSiteProjectService(projects, opener);
        SiteProjectDialog dialog(projects, downloads, false, opener, &window);
        dialog.show();
        wait([&] {
            auto* q = dialog.findChild<QComboBox*>("siteProjectQueue");
            return q && q->count() == 2;
        });
        auto id = create(dialog, projects, origin.url(), temporary.filePath("exports"), defaultQueueId);
        require(origin.requests == 0, "Create unexpectedly contacted origin");
        button(dialog, "siteProjectPreview")->click();
        wait([&] {
            auto tasks = downloads.snapshot();
            return std::any_of(tasks.begin(), tasks.end(),
                               [](const auto& t) { return t.state == TaskState::PolicyWaiting; });
        });
        require(origin.requests == 0, "Project bypassed closed civil window");
        clock->utc = 60;
        wait([&] { return state(projects, id) == SiteProjectState::PreviewReady; });
        require(origin.images == 0, "Preview fetched an identified binary");
        button(dialog, "siteProjectStart")->click();
        wait([&] { return state(projects, id) == SiteProjectState::Completed; });
        button(dialog, "siteProjectOpen")->click();
        wait([&] { return opened.size() == 1; });
        require(std::filesystem::is_regular_file(opened.front()),
                "Product Open did not return a verified local file");
        const auto first = opened.front();
        origin.version = 2;
        button(dialog, "siteProjectUpdate")->click();
        wait([&] {
            auto p = projects.inspect(id);
            return p && p->revision == 2 && p->state == SiteProjectState::PreviewReady;
        });
        button(dialog, "siteProjectStart")->click();
        wait([&] { return state(projects, id) == SiteProjectState::Completed; });
        button(dialog, "siteProjectOpen")->click();
        wait([&] { return opened.size() == 2; });
        require(opened.back() != first && std::filesystem::exists(first),
                "Update replaced the previous immutable export");
        std::cout
            << "PASS actual project create/preview/start/open/update controls and native civil-window hold\n";
        auto second = create(dialog, projects, origin.url(), temporary.filePath("exports"), budget.id);
        button(dialog, "siteProjectPreview")->click();
        wait([&] {
            auto usage = finish(downloads.queueTraffic(budget.id));
            return usage.charged == 1;
        });
        wait([&] {
            auto tasks = downloads.snapshot();
            return std::any_of(tasks.begin(), tasks.end(), [&](const auto& t) {
                return t.queueId == budget.id && t.state == TaskState::PolicyWaiting;
            });
        });
        button(dialog, "siteProjectPause")->click();
        wait([&] { return state(projects, second) == SiteProjectState::Paused; });
        QueueConfiguration raised;
        raised.id = budget.id;
        raised.name = budget.name;
        raised.expectedRevision = budget.revision;
        raised.dailyPayloadBudget = 65536;
        finish(downloads.configureQueue(raised));
        button(dialog, "siteProjectResume")->click();
        wait([&] { return state(projects, second) == SiteProjectState::PreviewReady; });
        button(dialog, "siteProjectStart")->click();
        wait([&] { return state(projects, second) == SiteProjectState::Completed; });
        button(dialog, "siteProjectOpen")->click();
        wait([&] { return opened.size() == 3; });
        auto retained = opened.back();
        button(dialog, "siteProjectRemove")->click();
        wait([&] { return !projects.inspect(second); });
        require(std::filesystem::exists(retained), "Removing project history deleted export");
        wait([&] {
            auto* list = dialog.findChild<QTableWidget*>("siteProjectList");
            auto* plan = dialog.findChild<QTableWidget*>("siteProjectPlan");
            auto* queue = dialog.findChild<QComboBox*>("siteProjectQueue");
            return list && plan && queue && !list->selectedItems().empty() &&
                   plan->rowCount() == static_cast<int>(projects.inspect(id)->entries.size()) &&
                   queue->currentData().toString().toStdString() == projects.inspect(id)->options.queueId;
        });
        std::cout
            << "PASS actual native daily-budget hold, pause/raise/resume and history-preserving UI removal\n";
        // Advance only the clock; all UI commands, HTTP transfers, manifests and publication are real.
        const auto currentPolicies = finish(downloads.queues());
        const auto normalPolicy = std::find_if(currentPolicies.begin(), currentPolicies.end(),
                                               [](const auto& queue) { return queue.id == defaultQueueId; });
        require(normalPolicy != currentPolicies.end(), "Default queue disappeared during periodic fixture");
        normal = *normalPolicy;
        QueueConfiguration open;
        open.id = normal.id;
        open.name = normal.name;
        open.expectedRevision = normal.revision;
        finish(downloads.configureQueue(open));
        const auto prior = finish(projects.openPath(id));
        QFile priorFile(QString::fromStdWString(prior.wstring()));
        require(priorFile.open(QIODevice::ReadOnly), "Cannot read prior project export");
        const auto priorBytes = priorFile.readAll();
        priorFile.close();
        auto* interval = dialog.findChild<QSpinBox*>("siteProjectSyncInterval");
        auto* runs = dialog.findChild<QSpinBox*>("siteProjectSyncRuns");
        require(interval && runs, "Missing real periodic synchronization controls");
        interval->setValue(5);
        runs->setValue(1);
        wait([&] { return button(dialog, "siteProjectSyncEnable")->isEnabled(); });
        button(dialog, "siteProjectSyncEnable")->click();
        wait([&] { return projects.inspect(id)->syncNextAt == 360; });
        require(projects.hasUnfinished(), "Waiting schedule incorrectly permits completion action");
        origin.version = 3;
        clock->utc = 360;
        wait([&] {
            const auto p = projects.inspect(id);
            return p && p->revision == 3 && p->state == SiteProjectState::Completed;
        });
        const auto current = finish(projects.openPath(id));
        QFile currentFile(QString::fromStdWString(current.wstring()));
        require(currentFile.open(QIODevice::ReadOnly) && currentFile.readAll().contains("Project UI v3"),
                "Periodic revision did not contain actual changed HTTP document");
        require(priorFile.open(QIODevice::ReadOnly) && priorFile.readAll() == priorBytes && current != prior,
                "Periodic revision altered the old immutable export");
        require(projects.inspect(id)->syncIntervalMinutes == 0 && !projects.hasUnfinished(),
                "Finite synchronization allowance failed to disarm");
        wait([&] { return button(dialog, "siteProjectSyncEnable")->isEnabled(); });
        button(dialog, "siteProjectSyncEnable")->click();
        wait([&] { return button(dialog, "siteProjectSyncDisable")->isEnabled(); });
        button(dialog, "siteProjectSyncDisable")->click();
        wait([&] { return projects.inspect(id)->syncIntervalMinutes == 0; });
        std::cout << "PASS actual Qt bounded periodic synchronization and HTTP v3 immutable export; stop "
                     "recurrence\n";
        if (argc == 3 && std::string(argv[1]) == "--screenshot") {
            QCoreApplication::processEvents();
            require(dialog.grab().save(QString::fromUtf8(argv[2])),
                    "Cannot save owned project dialog screenshot");
        }
        finish(projects.pauseAll());
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
