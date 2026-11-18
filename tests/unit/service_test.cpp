#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <condition_variable>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/torrent_engine.h>
#include <maskpilot/ui/main_window.h>
#include <mutex>
#include <thread>

namespace {
struct MemoryRepository final : maskpilot::ITaskRepository {
    std::vector<maskpilot::Task> tasks;
    std::vector<maskpilot::Task> load() override { return tasks; }
    void save(const maskpilot::Task& task) override {
        auto it =
            std::find_if(tasks.begin(), tasks.end(), [&](const auto& value) { return value.id == task.id; });
        if (it == tasks.end())
            tasks.push_back(task);
        else
            *it = task;
    }
    void erase(const std::string& id) override {
        std::erase_if(tasks, [&](const auto& value) { return value.id == id; });
    }
};
struct ControlledTransport final : maskpilot::ITransferTransport {
    void run(const maskpilot::Task&, const std::filesystem::path&, maskpilot::TransferControl& control,
             const std::function<void(maskpilot::TransferEvent)>& callback) override {
        while (!control.pause && !control.cancel)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        maskpilot::TransferEvent event;
        event.outcome =
            control.cancel ? maskpilot::TransferOutcome::Cancelled : maskpilot::TransferOutcome::Paused;
        callback(event);
    }
};
bool waitState(maskpilot::DownloadService& service, const std::string& id, maskpilot::TaskState state) {
    for (int i = 0; i < 200; ++i) {
        for (const auto& task : service.snapshot())
            if (task.id == id && task.state == state)
                return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
} // namespace
TEST(TaskControl, InactiveCancellationReclaimsOwnedStageWhenCacheIsAlreadyGone) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [] { return std::make_unique<ControlledTransport>(); },
        root / L"parts");
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/file";
    spec.destination = root / L"final.bin";
    spec.startPaused = true;
    const auto id = service.add(spec).get();
    auto stage = spec.destination;
    stage += L".maskpilot-" + std::filesystem::path(id).wstring() + L".tmp";
    {
        std::ofstream output(spec.destination);
        output << "keep final bytes";
    }
    {
        std::ofstream output(stage);
        output << "abandoned assembly";
    }
    ASSERT_FALSE(std::filesystem::exists(root / L"parts" / id));
    service.cancel(id);
    ASSERT_TRUE(waitState(service, id, maskpilot::TaskState::Cancelled));
    EXPECT_FALSE(std::filesystem::exists(stage));
    std::ifstream final(spec.destination);
    std::string contents;
    std::getline(final, contents);
    EXPECT_EQ(contents, "keep final bytes");
}
TEST(TaskControl, PauseResumeCancelAndRepeatedCommands) {
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [] { return std::make_unique<ControlledTransport>(); },
        std::filesystem::temp_directory_path());
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/a";
    spec.destination = std::filesystem::temp_directory_path() / (maskpilot::newTaskId() + ".bin");
    const auto id = service.add(spec).get();
    ASSERT_TRUE(waitState(service, id, maskpilot::TaskState::Probing));
    service.pause(id);
    service.pause(id);
    ASSERT_TRUE(waitState(service, id, maskpilot::TaskState::Paused));
    service.resume(id);
    ASSERT_TRUE(waitState(service, id, maskpilot::TaskState::Probing));
    service.cancel(id);
    ASSERT_TRUE(waitState(service, id, maskpilot::TaskState::Cancelled));
    service.resume(id);
    EXPECT_TRUE(waitState(service, id, maskpilot::TaskState::Cancelled));
}
TEST(TaskControl, FutureScheduleDoesNotStartEarly) {
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [] { return std::make_unique<ControlledTransport>(); },
        std::filesystem::temp_directory_path());
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/b";
    spec.destination = std::filesystem::temp_directory_path() / (maskpilot::newTaskId() + ".bin");
    spec.scheduledAt = maskpilot::unixTime() + 3600;
    const auto id = service.add(spec).get();
    EXPECT_TRUE(waitState(service, id, maskpilot::TaskState::Scheduled));
    service.pause(id);
    EXPECT_TRUE(waitState(service, id, maskpilot::TaskState::Paused));
}
TEST(TaskControl, InactiveCancellationDeletesOnlyOwnedCache) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [] { return std::make_unique<ControlledTransport>(); },
        root / "parts");
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/cache";
    spec.destination = root / "published.bin";
    spec.startPaused = true;
    const auto id = service.add(spec).get();
    const auto cache = root / "parts" / id;
    std::filesystem::create_directories(cache / "media");
    std::ofstream(cache / "part-0") << "partial";
    std::ofstream(cache / "part-255") << "last adaptive extent";
    std::ofstream(cache / "offset-255.new") << "3";
    std::ofstream(cache / "identity.new") << "uncommitted layout";
    std::ofstream(cache / "media" / "fragment") << "partial";
    std::ofstream(spec.destination) << "keep final";
    service.cancel(id);
    ASSERT_TRUE(waitState(service, id, maskpilot::TaskState::Cancelled));
    EXPECT_FALSE(std::filesystem::exists(cache));
    EXPECT_TRUE(std::filesystem::exists(spec.destination));
}
TEST(TaskControl, RestartClearsHighRecoveryIdsButPreservesUnrelatedFilesAndDestination) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [] { return std::make_unique<ControlledTransport>(); },
        root / "parts");
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/restart";
    spec.destination = root / "published.bin";
    spec.startPaused = true;
    spec.originAuthenticationRequired = true; // Keep this restart from creating any network worker.
    const auto id = service.add(spec).get();
    const auto cache = root / "parts" / id;
    std::filesystem::create_directories(cache);
    for (const auto* name : {"part-255", "offset-255", "offset-255.new", "identity", "identity.new",
                             "part-256", "part-025", "part-2.backup", "notes.txt"})
        std::ofstream(cache / name) << "owned fixture bytes";
    std::ofstream(spec.destination) << "preserve published bytes";
    service.restart(id);
    ASSERT_TRUE(waitState(service, id, maskpilot::TaskState::NeedsAttention));
    for (const auto* name : {"part-255", "offset-255", "offset-255.new", "identity", "identity.new"})
        EXPECT_FALSE(std::filesystem::exists(cache / name));
    for (const auto* name : {"part-256", "part-025", "part-2.backup", "notes.txt"})
        EXPECT_TRUE(std::filesystem::exists(cache / name));
    std::ifstream published(spec.destination);
    EXPECT_EQ(std::string((std::istreambuf_iterator<char>(published)), {}), "preserve published bytes");
}
TEST(TaskControl, DamagedSavedPathCannotStartOrDeleteAnExistingFile) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    const auto existing = root / "existing.bin";
    std::ofstream(existing) << "keep this existing file";
    auto repository = std::make_unique<MemoryRepository>();
    maskpilot::Task damaged;
    damaged.id = maskpilot::newTaskId();
    damaged.spec.url = "https://example.test/file";
    auto pathBytes = existing.u8string();
    pathBytes.push_back(u8'\0');
    pathBytes += u8".ignored";
    damaged.spec.destination = std::filesystem::path(pathBytes);
    damaged.state = maskpilot::TaskState::Paused;
    repository->tasks.push_back(damaged);
    maskpilot::DownloadService service(
        std::move(repository), [] { return std::make_unique<ControlledTransport>(); }, root / "parts");
    service.resume(damaged.id);
    ASSERT_TRUE(waitState(service, damaged.id, maskpilot::TaskState::NeedsAttention));
    service.cancel(damaged.id);
    ASSERT_TRUE(waitState(service, damaged.id, maskpilot::TaskState::Cancelled));
    ASSERT_TRUE(std::filesystem::exists(existing));
    std::ifstream preserved(existing);
    std::string contents;
    std::getline(preserved, contents);
    EXPECT_EQ(contents, "keep this existing file");
}
TEST(TaskControl, ReducingConcurrencyKeepsCombinedMediaAndNativeBudgetBounded) {
    struct Observations {
        std::mutex mutex;
        std::map<std::string, std::int64_t> rates;
    } observed;
    struct RateTransport final : maskpilot::ITransferTransport {
        Observations& observed;
        explicit RateTransport(Observations& value) : observed(value) {}
        void run(const maskpilot::Task& task, const std::filesystem::path&,
                 maskpilot::TransferControl& control,
                 const std::function<void(maskpilot::TransferEvent)>& callback) override {
            while (!control.pause && !control.cancel) {
                {
                    std::lock_guard lock(observed.mutex);
                    observed.rates[task.id] = control.speedLimit;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            maskpilot::TransferEvent event;
            event.outcome = maskpilot::TransferOutcome::Cancelled;
            callback(event);
        }
    };
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [&] { return std::make_unique<RateTransport>(observed); },
        std::filesystem::temp_directory_path());
    service.setLimits(8, 800 * 1024);
    for (int n = 0; n < 8; ++n) {
        maskpilot::DownloadSpec spec;
        spec.url = "https://example.test/budget";
        spec.destination = std::filesystem::temp_directory_path() / (maskpilot::newTaskId() + ".bin");
        spec.kind = n < 4 ? maskpilot::TransferKind::Media : maskpilot::TransferKind::Http;
        service.add(spec).get();
    }
    for (int n = 0; n < 200; ++n) {
        {
            std::lock_guard lock(observed.mutex);
            if (observed.rates.size() == 8)
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    service.setLimits(1, 800 * 1024);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::lock_guard lock(observed.mutex);
    ASSERT_EQ(observed.rates.size(), 8);
    std::int64_t combined = 0;
    for (const auto& [id, rate] : observed.rates) {
        (void)id;
        EXPECT_GT(rate, 0);
        combined += rate;
    }
    EXPECT_LE(combined, 800 * 1024);
}
TEST(TaskControl, TorrentUploadBudgetRespectsGlobalAndTaskLimitsAndRebalancesAfterPause) {
    struct Observations {
        std::mutex mutex;
        std::map<std::string, std::int64_t> rates;
    } observed;
    struct UploadTransport final : maskpilot::ITransferTransport {
        Observations& observed;
        explicit UploadTransport(Observations& value) : observed(value) {}
        void run(const maskpilot::Task& task, const std::filesystem::path&,
                 maskpilot::TransferControl& control,
                 const std::function<void(maskpilot::TransferEvent)>& callback) override {
            while (!control.pause && !control.cancel) {
                {
                    std::lock_guard lock(observed.mutex);
                    observed.rates[task.id] = control.uploadLimit;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            maskpilot::TransferEvent event;
            event.outcome =
                control.cancel ? maskpilot::TransferOutcome::Cancelled : maskpilot::TransferOutcome::Paused;
            callback(event);
        }
    };
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [&] { return std::make_unique<UploadTransport>(observed); },
        root / "parts");
    service.setLimits(3, 0, 64 * 1024);
    maskpilot::DownloadSpec spec;
    spec.url = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";
    spec.destination = root / "first";
    spec.torrent.uploadLimit = 16 * 1024;
    const auto first = service.add(spec).get();
    spec.url = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234568";
    spec.destination = root / "second";
    spec.torrent.uploadLimit = 0;
    const auto second = service.add(spec).get();
    spec.url = "https://example.test/file";
    spec.destination = root / "http.bin";
    service.add(spec).get();
    auto waitRate = [&](const std::string& id, std::int64_t expected) {
        for (int n = 0; n < 200; ++n) {
            {
                std::lock_guard lock(observed.mutex);
                if (observed.rates.contains(id) && observed.rates.at(id) == expected)
                    return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    ASSERT_TRUE(waitRate(first, 16 * 1024));
    // Queue allocation redistributes the first task's unused fair share without exceeding 64 KiB/s.
    ASSERT_TRUE(waitRate(second, 48 * 1024));
    service.setLimits(3, 0, 32 * 1024);
    ASSERT_TRUE(waitRate(second, 16 * 1024));
    ASSERT_TRUE(waitRate(first, 16 * 1024));
    service.pause(first);
    ASSERT_TRUE(waitState(service, first, maskpilot::TaskState::Paused));
    EXPECT_TRUE(waitRate(second, 32 * 1024));
}
TEST(TaskControl, ResumePreservesFutureStopDeadline) {
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [] { return std::make_unique<ControlledTransport>(); },
        std::filesystem::temp_directory_path());
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/deadline";
    spec.destination = std::filesystem::temp_directory_path() / (maskpilot::newTaskId() + ".bin");
    spec.startPaused = true;
    spec.stopAt = maskpilot::unixTime() + 3600;
    auto id = service.add(spec).get();
    service.resume(id);
    ASSERT_TRUE(waitState(service, id, maskpilot::TaskState::Probing));
    auto tasks = service.snapshot();
    ASSERT_EQ(tasks.size(), 1);
    EXPECT_EQ(tasks[0].spec.stopAt, spec.stopAt);
    service.cancel(id);
}
TEST(DesktopFlow, AddDialogCreatesPausedTaskThroughActualWidgets) {
    qputenv("QT_QPA_PLATFORM",
            qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI") ? "windows" : "offscreen");
    int argc = 1;
    char program[] = "MaskPilotUiTest";
    char* argv[] = {program, nullptr};
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName("WidgetFlow");
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [] { return std::make_unique<ControlledTransport>(); },
        std::filesystem::path(temporary.path().toStdWString()));
    maskpilot::MainWindow window(service);
    window.show();
    bool exercised = false;
    QTimer::singleShot(50, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        ASSERT_NE(modal, nullptr);
        auto* destination = modal->findChild<QLineEdit*>("downloadDestination");
        auto* paused = modal->findChild<QCheckBox*>("addPaused");
        auto* confirm = modal->findChild<QPushButton*>("confirmDownload");
        ASSERT_NE(destination, nullptr);
        ASSERT_NE(paused, nullptr);
        ASSERT_NE(confirm, nullptr);
        auto* proxy = modal->findChild<QLineEdit*>("proxyEndpoint");
        auto* proxyUser = modal->findChild<QLineEdit*>("proxyUsername");
        auto* proxyPassword = modal->findChild<QLineEdit*>("proxyPassword");
        ASSERT_NE(proxy, nullptr);
        ASSERT_NE(proxyUser, nullptr);
        ASSERT_NE(proxyPassword, nullptr);
        auto* tabs = modal->findChild<QTabWidget*>();
        ASSERT_NE(tabs, nullptr);
        tabs->setCurrentWidget(proxy->parentWidget());
        EXPECT_TRUE(proxy->isVisible());
        EXPECT_EQ(proxyPassword->echoMode(), QLineEdit::Password);
        proxy->setText("socks5://127.0.0.1:1080");
        proxyUser->setText("widget-proxy-user");
        proxyPassword->setText("widget-proxy-password");
        auto* required = modal->findChild<QCheckBox*>("proxyAuthenticationRequired");
        ASSERT_NE(required, nullptr);
        EXPECT_TRUE(required->isChecked());
        if (const auto screenshot = qEnvironmentVariable("MASKPILOT_TEST_PROXY_SCREENSHOT");
            !screenshot.isEmpty()) {
            QApplication::processEvents();
            EXPECT_TRUE(modal->grab().save(screenshot));
        }
        destination->setText(temporary.filePath("widget-flow.bin"));
        paused->setChecked(true);
        exercised = true;
        confirm->click();
    });
    window.newDownload("https://example.test/widget-flow.bin");
    for (int n = 0; n < 100 && service.snapshot().empty(); ++n) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(exercised);
    auto tasks = service.snapshot();
    ASSERT_EQ(tasks.size(), 1);
    EXPECT_EQ(tasks[0].state, maskpilot::TaskState::Paused);
    EXPECT_EQ(tasks[0].spec.destination.filename(), std::filesystem::path("widget-flow.bin"));
    EXPECT_EQ(tasks[0].spec.proxy, "socks5://127.0.0.1:1080");
    EXPECT_EQ(tasks[0].spec.proxyUsername, "widget-proxy-user");
    EXPECT_EQ(tasks[0].spec.proxyPassword, "widget-proxy-password");
    EXPECT_TRUE(tasks[0].spec.proxyAuthenticationRequired);
}
TEST(DesktopFlow, ExportImportPreservesMediaModeWithoutExportingLoginHeaders) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    int argc = 1;
    char program[] = "MaskPilotRoundTripTest";
    char* argv[] = {program, nullptr};
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName("RoundTripFlow");
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings settings;
    settings.setValue("language", "en");
    settings.setValue("mediaDownloader", temporary.filePath("configured-parser.exe"));
    settings.setValue("mediaMuxer", temporary.filePath("configured-muxer.exe"));
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [] { return std::make_unique<ControlledTransport>(); },
        root / "parts");
    maskpilot::DownloadSpec file;
    file.url = "https://example.test/file";
    file.destination = root / "direct.bin";
    file.cookie = "fixture-private-session";
    file.authorization = "Bearer fixture-private-header";
    file.proxy = "http://proxy.example.test:8080";
    file.proxyUsername = "fixture-private-proxy-user";
    file.proxyPassword = "fixture-private-proxy-password";
    file.startPaused = true;
    service.add(file).get();
    auto media = file;
    media.url = "https://example.test/playlist.m3u8";
    media.destination = root / "audio.m4a";
    media.cookie.clear();
    media.authorization.clear();
    media.kind = maskpilot::TransferKind::Media;
    media.audioOnly = true;
    media.mediaFormat = "bestaudio";
    service.add(media).get();
    maskpilot::MainWindow window(service);
    window.show();
    auto waitEvents = [] {
        QEventLoop loop;
        QTimer::singleShot(350, &loop, &QEventLoop::quit);
        loop.exec();
    };
    waitEvents();
    QAction *exportAction = nullptr, *importAction = nullptr;
    for (auto* action : window.findChildren<QAction*>()) {
        if (action->text().startsWith("Export tasks"))
            exportAction = action;
        if (action->text().startsWith("Import tasks"))
            importAction = action;
    }
    ASSERT_NE(exportAction, nullptr);
    ASSERT_NE(importAction, nullptr);
    const auto exported = QDir(QFileInfo(temporary.path()).canonicalFilePath()).filePath("tasks.json");
    QString lastSelection;
    int pickerResult = -1;
    auto chooseFile = [&] {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        const QFileInfo target(exported);
        if (dialog->directory().absolutePath() != target.absolutePath())
            dialog->setDirectory(target.absolutePath());
        // Qt intentionally leaves selectFile() unchanged while this editor has focus. Enter the
        // filename through the actual widget so focus timing cannot silently keep downloads.json.
        auto* filename = dialog->findChild<QLineEdit*>("fileNameEdit");
        if (!filename)
            return;
        filename->setText(target.fileName());
        lastSelection = dialog->selectedFiles().value(0);
        if (QDir::cleanPath(lastSelection) != QDir::cleanPath(exported))
            return;
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        pickerResult = dialog->result();
    };
    // QFileDialog loads its directory model asynchronously. Keep driving the same file selection
    // until it accepts, rather than relying on one timer firing after an arbitrary startup delay.
    QTimer fileChooser;
    fileChooser.setInterval(50);
    QObject::connect(&fileChooser, &QTimer::timeout, &window, chooseFile);
    QTimer modalWatchdog;
    modalWatchdog.setSingleShot(true);
    QObject::connect(&modalWatchdog, &QTimer::timeout, &window, [] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        const auto* message = qobject_cast<QMessageBox*>(dialog);
        ADD_FAILURE() << "Task-list round trip blocked on modal: "
                      << (dialog ? dialog->windowTitle().toStdString() : "none") << " / "
                      << (message ? message->text().toStdString() : "no message");
        if (dialog)
            dialog->reject();
    });
    modalWatchdog.start(15000);
    fileChooser.start();
    exportAction->trigger();
    fileChooser.stop();
    QFile json(exported);
    ASSERT_TRUE(json.open(QIODevice::ReadOnly))
        << json.errorString().toStdString() << "; selected=" << lastSelection.toStdString()
        << "; result=" << pickerResult;
    const auto bytes = json.readAll();
    json.close();
    EXPECT_FALSE(bytes.contains("fixture-private-session"));
    EXPECT_FALSE(bytes.contains("fixture-private-header"));
    EXPECT_FALSE(bytes.contains("fixture-private-proxy-user"));
    EXPECT_FALSE(bytes.contains("fixture-private-proxy-password"));
    const auto saved = QJsonDocument::fromJson(bytes).object().value("downloads").toArray();
    EXPECT_EQ(QJsonDocument::fromJson(bytes).object().value("version").toInt(), 2);
    ASSERT_EQ(saved.size(), 2);
    bool foundMedia = false;
    for (const auto& entry : saved)
        EXPECT_TRUE(entry.toObject().value("proxyAuthenticationRequired").toBool());
    for (const auto& entry : saved)
        EXPECT_EQ(entry.toObject().value("originAuthenticationRequired").toBool(),
                  entry.toObject().value("url").toString() == QString::fromStdString(file.url));
    for (const auto& entry : saved)
        if (entry.toObject().value("url").toString().contains("playlist")) {
            foundMedia = true;
            EXPECT_TRUE(entry.toObject().value("media").toBool());
            EXPECT_TRUE(entry.toObject().value("audioOnly").toBool());
        }
    ASSERT_TRUE(foundMedia);
    for (const auto& task : service.snapshot())
        service.remove(task.id);
    for (int n = 0; n < 100 && !service.snapshot().empty(); ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(service.snapshot().empty());
    bool previewSeen = false;
    QTimer previewHandler;
    previewHandler.setInterval(25);
    QObject::connect(&previewHandler, &QTimer::timeout, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "taskImportPreview")
            return;
        auto* summary = dialog->findChild<QLabel*>("taskImportSummary");
        auto* rows = dialog->findChild<QTreeWidget*>("taskImportPreviewTasks");
        auto* confirm = dialog->findChild<QPushButton*>("confirmTaskImport");
        ASSERT_NE(summary, nullptr);
        ASSERT_NE(rows, nullptr);
        ASSERT_NE(confirm, nullptr);
        EXPECT_TRUE(summary->text().contains("standalone"));
        EXPECT_EQ(rows->topLevelItemCount(), 2);
        EXPECT_EQ(dialog->findChildren<QComboBox*>().size(), 0);
        previewSeen = true;
        confirm->click();
    });
    previewHandler.start();
    fileChooser.start();
    importAction->trigger();
    fileChooser.stop();
    for (int n = 0; n < 500 && service.snapshot().size() != 2; ++n) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    previewHandler.stop();
    modalWatchdog.stop();
    EXPECT_TRUE(previewSeen);
    const auto imported = service.snapshot();
    ASSERT_EQ(imported.size(), 2);
    for (const auto& task : imported) {
        EXPECT_EQ(task.state, maskpilot::TaskState::Paused);
        EXPECT_TRUE(task.spec.cookie.empty());
        EXPECT_TRUE(task.spec.authorization.empty());
        EXPECT_EQ(task.spec.proxy, file.proxy);
        EXPECT_TRUE(task.spec.proxyAuthenticationRequired);
        EXPECT_TRUE(task.spec.proxyUsername.empty());
        EXPECT_TRUE(task.spec.proxyPassword.empty());
        EXPECT_EQ(task.ownerId, "standalone");
        EXPECT_EQ(task.contextRevision, 0);
        EXPECT_TRUE(task.proxyCredentialRef.empty());
        EXPECT_TRUE(task.originGrantId.empty());
        EXPECT_EQ(task.spec.originAuthenticationRequired, task.spec.url == file.url);
        if (task.spec.url == media.url) {
            EXPECT_TRUE(task.spec.kind == maskpilot::TransferKind::Media);
            EXPECT_TRUE(task.spec.audioOnly);
            EXPECT_EQ(task.spec.mediaFormat, "bestaudio");
            EXPECT_EQ(task.spec.mediaDownloader, root / "configured-parser.exe");
        } else {
            EXPECT_FALSE(task.spec.kind == maskpilot::TransferKind::Media);
        }
    }
}

TEST(DesktopFlow, TaskImportCancelAndPartialSaveRetryPreserveCountsAndRequestIdentity) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    int argc = 1;
    char program[] = "MaskPilotImportRetryTest";
    char* argv[] = {program, nullptr};
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName("ImportRetryFlow");
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings().setValue("language", "en");
    struct Attempts {
        std::mutex mutex;
        std::vector<std::string> ids;
        std::atomic_int transfers{0};
        std::atomic_size_t failAt{2};
    };
    struct FailingRepository final : maskpilot::ITaskRepository {
        std::shared_ptr<Attempts> attempts;
        MemoryRepository inner;
        explicit FailingRepository(std::shared_ptr<Attempts> value) : attempts(std::move(value)) {}
        std::vector<maskpilot::Task> load() override { return inner.load(); }
        void save(const maskpilot::Task& task) override {
            {
                std::lock_guard lock(attempts->mutex);
                attempts->ids.push_back(task.id);
                if (attempts->ids.size() == attempts->failAt)
                    throw std::runtime_error("Owned fixture save failure");
            }
            inner.save(task);
        }
        void erase(const std::string& id) override { inner.erase(id); }
    };
    struct UnexpectedTransfer final : maskpilot::ITransferTransport {
        std::shared_ptr<Attempts> attempts;
        explicit UnexpectedTransfer(std::shared_ptr<Attempts> value) : attempts(std::move(value)) {}
        void run(const maskpilot::Task&, const std::filesystem::path&, maskpilot::TransferControl&,
                 const std::function<void(maskpilot::TransferEvent)>& callback) override {
            ++attempts->transfers;
            maskpilot::TransferEvent event;
            event.outcome = maskpilot::TransferOutcome::Failed;
            callback(event);
        }
    };
    const auto attempts = std::make_shared<Attempts>();
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    maskpilot::DownloadService service(
        std::make_unique<FailingRepository>(attempts),
        [attempts] { return std::make_unique<UnexpectedTransfer>(attempts); }, root / "parts");
    QJsonArray rows;
    for (int n = 0; n < 3; ++n)
        rows.append(QJsonObject{{"url", QString("https://example.test/file-%1").arg(n)},
                                {"destination", temporary.filePath(QString("file-%1.bin").arg(n))},
                                {"proxy", ""},
                                {"proxyAuthenticationRequired", false},
                                {"originAuthenticationRequired", true},
                                {"contextBound", true}});
    const auto listPath = temporary.filePath("import.json");
    QFile file(listPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{{"version", 2}, {"downloads", rows}}).toJson());
    file.close();
    maskpilot::MainWindow window(service);
    window.show();
    QAction* importAction = nullptr;
    for (auto* action : window.findChildren<QAction*>())
        if (action->text().startsWith("Import tasks"))
            importAction = action;
    ASSERT_NE(importAction, nullptr);
    enum Phase {
        Cancel,
        Partial,
        Retry,
        KeepRemaining,
        Discard,
        FirstFailure,
        DiscardZero,
        ChooseNew
    } phase = Cancel;
    int previews = 0;
    bool partialWarning = false;
    bool newChooserSeen = false;
    QTimer handler;
    handler.setInterval(25);
    QObject::connect(&handler, &QTimer::timeout, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        if (auto* chooser = qobject_cast<QFileDialog*>(modal)) {
            if (phase == ChooseNew) {
                newChooserSeen = true;
                chooser->reject();
                return;
            }
            const QFileInfo target(listPath);
            if (chooser->directory().absolutePath() != target.absolutePath())
                chooser->setDirectory(target.absolutePath());
            auto* name = chooser->findChild<QLineEdit*>("fileNameEdit");
            if (!name)
                return;
            name->setText(target.fileName());
            if (QDir::cleanPath(chooser->selectedFiles().value(0)) == QDir::cleanPath(listPath))
                QMetaObject::invokeMethod(chooser, "accept", Qt::DirectConnection);
        } else if (auto* dialog = qobject_cast<QDialog*>(modal);
                   dialog && dialog->objectName() == "taskImportPreview") {
            ++previews;
            auto* summary = dialog->findChild<QLabel*>("taskImportSummary");
            auto* tasks = dialog->findChild<QTreeWidget*>("taskImportPreviewTasks");
            ASSERT_NE(summary, nullptr);
            ASSERT_NE(tasks, nullptr);
            EXPECT_TRUE(summary->text().contains("standalone"));
            if (phase == Retry || phase == KeepRemaining || phase == Discard) {
                EXPECT_TRUE(summary->text().contains("1 already imported"));
                EXPECT_EQ(tasks->topLevelItemCount(), 2);
            } else
                EXPECT_EQ(tasks->topLevelItemCount(), 3);
            if (phase == Cancel || phase == KeepRemaining)
                dialog->reject();
            else if (phase == Discard || phase == DiscardZero) {
                auto* discard = dialog->findChild<QPushButton*>("discardTaskImport");
                ASSERT_NE(discard, nullptr);
                if (phase == DiscardZero)
                    EXPECT_TRUE(summary->text().contains("0 already imported"));
                discard->click();
            } else
                dialog->findChild<QPushButton*>("confirmTaskImport")->click();
        } else if (auto* warning = qobject_cast<QMessageBox*>(modal)) {
            EXPECT_TRUE(phase == Partial || phase == FirstFailure);
            EXPECT_TRUE(warning->text().contains(phase == FirstFailure ? "0 tasks were imported"
                                                                       : "1 tasks were imported"));
            EXPECT_TRUE(warning->text().contains(phase == FirstFailure ? "3 remain" : "2 remain"));
            partialWarning = true;
            warning->accept();
        }
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &window, [] {
        ADD_FAILURE() << "Import preview/retry modal exceeded bounded test deadline";
        if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
            modal->reject();
    });
    watchdog.start(15000);
    handler.start();
    auto waitFor = [&](const auto& predicate) {
        for (int n = 0; n < 1000 && !predicate(); ++n) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return predicate();
    };
    importAction->trigger();
    ASSERT_TRUE(waitFor([&] { return previews == 1; }));
    EXPECT_TRUE(service.snapshot().empty());
    {
        std::lock_guard lock(attempts->mutex);
        EXPECT_TRUE(attempts->ids.empty());
    }
    phase = Partial;
    importAction->trigger();
    ASSERT_TRUE(waitFor([&] { return partialWarning; }));
    ASSERT_EQ(service.snapshot().size(), 1);
    phase = Retry;
    importAction->trigger();
    ASSERT_TRUE(waitFor([&] {
        return service.snapshot().size() == 3 &&
               window.statusBar()->currentMessage().startsWith("Imported 3 paused");
    }));
    EXPECT_EQ(previews, 3);
    EXPECT_EQ(attempts->transfers, 0);
    EXPECT_TRUE(service.lastError().empty());
    {
        std::lock_guard lock(attempts->mutex);
        ASSERT_EQ(attempts->ids.size(), 4);
        EXPECT_EQ(attempts->ids[1], attempts->ids[2]);
        EXPECT_NE(attempts->ids[0], attempts->ids[1]);
    }
    for (const auto& task : service.snapshot()) {
        EXPECT_EQ(task.ownerId, "standalone");
        EXPECT_EQ(task.state, maskpilot::TaskState::Paused);
        EXPECT_TRUE(task.spec.originAuthenticationRequired);
        EXPECT_TRUE(task.originGrantId.empty());
        EXPECT_TRUE(task.proxyCredentialRef.empty());
    }
    attempts->failAt = 6; // One success, then a second partial-batch failure.
    partialWarning = false;
    phase = Partial;
    importAction->trigger();
    ASSERT_TRUE(waitFor([&] { return partialWarning; }));
    ASSERT_EQ(service.snapshot().size(), 4);
    const auto preserved = service.snapshot();
    {
        std::ofstream preservedOutput(preserved.front().spec.destination);
        preservedOutput << "preserve imported task bytes";
    }
    phase = KeepRemaining;
    importAction->trigger();
    EXPECT_EQ(service.snapshot().size(), preserved.size());
    phase = Discard;
    importAction->trigger();
    phase = ChooseNew;
    importAction->trigger();
    EXPECT_TRUE(newChooserSeen);
    const auto afterDiscard = service.snapshot();
    for (const auto& task : preserved)
        EXPECT_TRUE(std::any_of(afterDiscard.begin(), afterDiscard.end(),
                                [&](const auto& current) { return current.id == task.id; }));
    {
        std::ifstream preservedInput(preserved.front().spec.destination);
        EXPECT_EQ(std::string((std::istreambuf_iterator<char>(preservedInput)), {}),
                  "preserve imported task bytes");
    }

    // A failure before any successful item still needs an explicit escape from the retained batch.
    attempts->failAt = 7;
    partialWarning = false;
    phase = FirstFailure;
    importAction->trigger();
    ASSERT_TRUE(waitFor([&] { return partialWarning; }));
    phase = DiscardZero;
    importAction->trigger();
    newChooserSeen = false;
    phase = ChooseNew;
    importAction->trigger();
    EXPECT_TRUE(newChooserSeen);
    EXPECT_EQ(service.snapshot().size(), preserved.size());
    EXPECT_EQ(attempts->transfers, 0);
    handler.stop();
    watchdog.stop();
}

TEST(DesktopFlow, FtpCredentialsReachPausedTaskWithoutHttpHeaders) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char program[] = "MaskPilotFtpWidgetTest";
    char* argv[] = {program, nullptr};
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName("FtpWidgetFlow");
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    maskpilot::DownloadService service(
        std::make_unique<MemoryRepository>(), [] { return std::make_unique<ControlledTransport>(); },
        std::filesystem::path(temporary.path().toStdWString()) / "parts");
    maskpilot::MainWindow window(service);
    window.show();
    bool exercised = false;
    QTimer::singleShot(50, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        ASSERT_NE(modal, nullptr);
        auto* username = modal->findChild<QLineEdit*>("ftpUsername");
        auto* password = modal->findChild<QLineEdit*>("ftpPassword");
        ASSERT_NE(username, nullptr);
        ASSERT_NE(password, nullptr);
        EXPECT_EQ(password->echoMode(), QLineEdit::Password);
        username->setText("fixture-user");
        password->setText("fixture-password");
        modal->findChild<QLineEdit*>("downloadDestination")->setText(temporary.filePath("ftp.bin"));
        modal->findChild<QCheckBox*>("addPaused")->setChecked(true);
        exercised = true;
        modal->findChild<QPushButton*>("confirmDownload")->click();
    });
    window.newDownload("ftps://example.test/ftp.bin");
    for (int n = 0; n < 100 && service.snapshot().empty(); ++n) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(exercised);
    const auto tasks = service.snapshot();
    ASSERT_EQ(tasks.size(), 1);
    EXPECT_EQ(tasks[0].state, maskpilot::TaskState::Paused);
    EXPECT_EQ(tasks[0].spec.ftpUsername, "fixture-user");
    EXPECT_EQ(tasks[0].spec.ftpPassword, "fixture-password");
    EXPECT_TRUE(tasks[0].spec.headers.empty());
    EXPECT_FALSE(tasks[0].spec.kind == maskpilot::TransferKind::Media);
}

TEST(DesktopFlow, TorrentFileSelectionUsesActualWidgetsAndDoesNotStartPayload) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char program[] = "MaskPilotTorrentWidgetTest";
    char* argv[] = {program, nullptr};
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName("TorrentWidgetFlow");
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings().setValue("language", "en");
    std::string bytes = "d4:infod6:lengthi3e4:name8:file.bin12:piece lengthi16384e6:pieces20:";
    const unsigned char sha1[] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                  0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    bytes.append(reinterpret_cast<const char*>(sha1), std::size(sha1));
    bytes += "ee";
    const auto source = std::filesystem::path(temporary.filePath("fixture.torrent").toStdWString());
    {
        std::ofstream out(source, std::ios::binary);
        out.write(bytes.data(), bytes.size());
    }
    auto factory = maskpilot::makeTorrentTransferFactory({false, "127.0.0.1:0"});
    maskpilot::DownloadSpec check;
    const auto sourceText = source.generic_u8string();
    check.url = "torrent:" + std::string(sourceText.begin(), sourceText.end());
    check.kind = maskpilot::TransferKind::BitTorrent;
    ASSERT_NO_THROW(check.torrent.metadata = factory()->captureMetadata(check));
    ASSERT_NO_THROW(factory()->describe(check));
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    maskpilot::DownloadService service(std::make_unique<MemoryRepository>(), factory, root / "parts");
    maskpilot::MainWindow window(service);
    window.show();
    QTimer::singleShot(50, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        ASSERT_NE(modal, nullptr);
        modal->findChild<QLineEdit*>("downloadDestination")->setText(temporary.filePath("payload"));
        modal->findChild<QCheckBox*>("torrentAllFiles")->setChecked(false);
        modal->findChild<QCheckBox*>("addPaused")->setChecked(true);
        modal->findChild<QPushButton*>("confirmDownload")->click();
    });
    window.newDownload(QString::fromStdString(check.url));
    auto* table = window.findChild<QTableView*>("downloadTable");
    ASSERT_NE(table, nullptr);
    for (int n = 0; n < 1000 && table->model()->rowCount() != 1; ++n) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(table->model()->rowCount(), 1);
    ASSERT_EQ(service.snapshot()[0].state, maskpilot::TaskState::AwaitingSelection);
    table->selectRow(0);
    for (auto* action : window.findChildren<QAction*>())
        if (action->text() == "Details") {
            action->trigger();
            break;
        }
    auto* files = window.findChild<QTreeWidget*>("torrentFiles");
    ASSERT_NE(files, nullptr);
    ASSERT_EQ(files->topLevelItemCount(), 1);
    EXPECT_EQ(files->topLevelItem(0)->checkState(0), Qt::Unchecked);
    files->topLevelItem(0)->setCheckState(0, Qt::Checked);
    auto* apply = window.findChild<QPushButton*>("applyTorrentSelection");
    ASSERT_NE(apply, nullptr);
    ASSERT_TRUE(apply->isEnabled());
    apply->click();
    for (int n = 0; n < 1000 && service.snapshot()[0].state != maskpilot::TaskState::Paused; ++n) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto task = service.snapshot()[0];
    EXPECT_EQ(task.state, maskpilot::TaskState::Paused);
    EXPECT_EQ(task.spec.torrent.selectedFiles, std::vector<int>{0});
    EXPECT_NE(task.spec.torrent.metadata, nullptr);
    EXPECT_FALSE(std::filesystem::exists(root / "payload"));
}
