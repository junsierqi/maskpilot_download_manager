#include "../support/local_http_server.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <atomic>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/ui/main_window.h>
#include <thread>

using namespace maskpilot;
namespace {
template <class T> T* widget(QObject* parent, const char* name) {
    auto* value = parent->findChild<T*>(name);
    if (!value)
        throw std::runtime_error(std::string("Missing queue fixture widget: ") + name);
    return value;
}
bool untilGui(const std::function<bool()>& predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do {
        QCoreApplication::processEvents();
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
void platform() {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
}
struct ModalBudget {
    QTimer timer;
    ModalBudget() {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, [] {
            ADD_FAILURE() << "Owned queue modal exceeded 20-second fixture budget";
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                dialog->done(QDialog::Rejected);
            if (auto* popup = QApplication::activePopupWidget())
                popup->close();
        });
        timer.start(20000);
    }
};
struct NoTransfer final : ITransferTransport {
    void run(const Task&, const std::filesystem::path&, TransferControl&,
             const std::function<void(TransferEvent)>& callback) override {
        ADD_FAILURE() << "Paused queue control test started a worker";
        TransferEvent event;
        event.outcome = TransferOutcome::Attention;
        callback(event);
    }
};
struct Store {
    QTemporaryDir temporary;
    std::filesystem::path root;
    TransportFactory factory;
    std::unique_ptr<DownloadService> service;
    explicit Store(bool real = false, TransportFactory custom = {}) {
        if (!temporary.isValid())
            throw std::runtime_error("Cannot create owned queue fixture");
        root = std::filesystem::path(temporary.path().toStdWString());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings settings;
        settings.setValue(
            "language",
            qEnvironmentVariable("MASKPILOT_TEST_QUEUE_LANGUAGE",
                                 qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI") ? "zh" : "en"));
        settings.setValue("downloadDirectory", temporary.path());
        settings.setValue("closeToTray", false);
        factory = custom ? std::move(custom)
                  : real ? TransportFactory([] { return makeCurlTransfer(); })
                         : TransportFactory([] { return std::make_unique<NoTransfer>(); });
        open();
    }
    void open() {
        auto store = makeSqliteStore(root / L"tasks.db");
        service = std::make_unique<DownloadService>(std::move(store.tasks), factory, root / L"parts",
                                                    ContextDependencies{std::move(store.contexts), {}},
                                                    QueueDependencies{std::move(store.queues)});
    }
    Task task(const std::string& id) {
        for (const auto& value : service->snapshot())
            if (value.id == id)
                return value;
        throw std::runtime_error("Queue fixture task missing");
    }
};
void addPaused(MainWindow& window, Store& store, const QString& url, const std::string& queue,
               const QString& filename) {
    const auto count = store.service->snapshot().size();
    QTimer::singleShot(40, &window, [&window, &store, url, queue, filename] {
        auto* dialog = QApplication::activeModalWidget();
        ASSERT_NE(dialog, nullptr);
        widget<QLineEdit>(dialog, "downloadDestination")->setText(store.temporary.filePath(filename));
        widget<QCheckBox>(dialog, "addPaused")->setChecked(true);
        auto* choice = widget<QComboBox>(dialog, "downloadQueue");
        ASSERT_GE(choice->findData(QString::fromStdString(queue)), 0);
        choice->setCurrentIndex(choice->findData(QString::fromStdString(queue)));
        widget<QPushButton>(dialog, "confirmDownload")->click();
    });
    window.newDownload(url);
    ASSERT_TRUE(untilGui([&] { return store.service->snapshot().size() == count + 1; }));
}
bool selectTask(MainWindow& window, const std::string& id) {
    auto* table = widget<QTableView>(&window, "downloadTable");
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        if (table->model()->index(row, 0).data(Qt::UserRole).toString().toStdString() == id) {
            table->selectRow(row);
            return true;
        }
    }
    return false;
}
std::string findTask(Store& store, const QString& url) {
    for (const auto& task : store.service->snapshot())
        if (task.spec.url == url.toStdString())
            return task.id;
    throw std::runtime_error("Added queue task missing");
}
} // namespace

TEST(QueueUi, ActualManagerAddFilterMoveAndReopenPreservePausedMembership) {
    platform();
    int argc = 1;
    char program[] = "QueueWidgets";
    char* argv[]{program, nullptr};
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(program);
    Store store;
    ModalBudget budget;
    std::string taskId, queueId;
    {
        MainWindow window(*store.service);
        window.show();
        int stage = 0;
        QTimer driver;
        driver.setInterval(25);
        QObject::connect(&driver, &QTimer::timeout, &window, [&] {
            auto* dialog = QApplication::activeModalWidget();
            if (!dialog || dialog->objectName() != "queueManagerDialog")
                return;
            if (stage == 0 && widget<QPushButton>(dialog, "queueSave")->isEnabled()) {
                widget<QLineEdit>(dialog, "queueName")->setText("Work queue");
                widget<QSpinBox>(dialog, "queueMaxActive")->setValue(2);
                widget<QSpinBox>(dialog, "queueDownloadLimit")->setValue(64);
                widget<QSpinBox>(dialog, "queueUploadLimit")->setValue(16);
                stage = 1;
                widget<QPushButton>(dialog, "queueSave")->click();
            } else if (stage == 1 && widget<QTreeWidget>(dialog, "queueList")->topLevelItemCount() == 2 &&
                       widget<QPushButton>(dialog, "queueSave")->isEnabled()) {
                const auto screenshot = qEnvironmentVariable("MASKPILOT_TEST_QUEUE_SCREENSHOT");
                if (!screenshot.isEmpty())
                    EXPECT_TRUE(dialog->grab().save(screenshot));
                stage = 2;
                widget<QPushButton>(dialog, "queueClose")->click();
            }
        });
        driver.start();
        QTimer::singleShot(0, &window, [&] { widget<QAction>(&window, "manageQueuesAction")->trigger(); });
        ASSERT_TRUE(untilGui([&] { return stage == 2; }));
        driver.stop();
        const auto queues = store.service->queues().get();
        const auto named =
            std::find_if(queues.begin(), queues.end(), [](const auto& q) { return q.name == "Work queue"; });
        ASSERT_NE(named, queues.end());
        queueId = named->id;
        EXPECT_EQ(named->downloadLimit, 64 * 1024);
        auto* filter = widget<QComboBox>(&window, "queueFilter");
        ASSERT_TRUE(untilGui([&] { return filter->findData(QString::fromStdString(queueId)) >= 0; }));
        filter->setCurrentIndex(filter->findData(QString::fromStdString(queueId)));
        addPaused(window, store, "https://example.invalid/queue-file", queueId, "paused.bin");
        taskId = store.service->snapshot()[0].id;
        EXPECT_EQ(store.task(taskId).queueId, queueId);
        EXPECT_EQ(store.task(taskId).runIntent, RunIntent::Paused);
        auto* table = widget<QTableView>(&window, "downloadTable");
        ASSERT_TRUE(untilGui([&] { return table->model()->rowCount() == 1; }));
        EXPECT_TRUE(table->model()->index(0, 0).data(Qt::UserRole + 2).toString().contains("Work queue"));
        window.resize(850, 560);
        QCoreApplication::processEvents();
        const auto windowScreenshot = qEnvironmentVariable("MASKPILOT_TEST_QUEUE_WINDOW_SCREENSHOT");
        if (!windowScreenshot.isEmpty())
            EXPECT_TRUE(window.grab().save(windowScreenshot));
        table->selectRow(0);
        bool clicked = false, submitted = false;
        QTimer picker, mover;
        picker.setInterval(25);
        mover.setInterval(25);
        QObject::connect(&mover, &QTimer::timeout, &window, [&] {
            auto* dialog = QApplication::activeModalWidget();
            if (submitted || !dialog || dialog->objectName() != "moveQueueDialog")
                return;
            auto* choice = widget<QComboBox>(dialog, "moveQueueTarget");
            choice->setCurrentIndex(choice->findData(defaultQueueId));
            submitted = true;
            widget<QPushButton>(dialog, "confirmQueueMove")->click();
        });
        QObject::connect(&picker, &QTimer::timeout, &window, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu || clicked)
                return;
            clicked = true;
            auto* action = widget<QAction>(menu, "moveToQueueAction");
            EXPECT_TRUE(action->isEnabled());
            action->trigger();
            menu->close();
        });
        picker.start();
        mover.start();
        QTimer::singleShot(0, &window, [&] {
            QMetaObject::invokeMethod(table, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, QPoint(10, 10)));
        });
        ASSERT_TRUE(untilGui([&] { return submitted && store.task(taskId).queueId == defaultQueueId; }));
        picker.stop();
        mover.stop();
        EXPECT_EQ(store.task(taskId).state, TaskState::Paused);
        EXPECT_EQ(store.task(taskId).ownerId, "standalone");
        window.requestQuit();
    }
    store.service.reset();
    store.open();
    EXPECT_EQ(store.task(taskId).queueId, defaultQueueId);
    EXPECT_EQ(store.task(taskId).runIntent, RunIntent::Paused);
    const auto policies = store.service->queues().get();
    EXPECT_TRUE(std::any_of(policies.begin(), policies.end(), [&](const auto& q) {
        return q.id == queueId && q.maxActive == 2 && q.downloadLimit == 64 * 1024 &&
               q.uploadLimit == 16 * 1024;
    }));
}

TEST(QueueUi, RealHttpQueuesPauseEnableWithoutResumingManualTasks) {
    platform();
    int argc = 1;
    char program[] = "QueueNativeFlow";
    char* argv[]{program, nullptr};
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(program);
    Store store(true);
    LocalHttpServer origin;
    QueueConfiguration draft;
    draft.name = "Work";
    draft.maxActive = 1;
    const auto work = store.service->configureQueue(draft).get();
    draft.name = "Other";
    const auto other = store.service->configureQueue(draft).get();
    MainWindow window(*store.service);
    window.show();
    ModalBudget budget;
    auto* filter = widget<QComboBox>(&window, "queueFilter");
    ASSERT_TRUE(untilGui([&] { return filter->findData(QString::fromStdString(other.id)) >= 0; }));
    addPaused(window, store, origin.url("/work"), work.id, "work.bin");
    addPaused(window, store, origin.url("/manual"), work.id, "manual.bin");
    addPaused(window, store, origin.url("/other"), other.id, "other.bin");
    const auto workId = findTask(store, origin.url("/work"));
    const auto manualId = findTask(store, origin.url("/manual"));
    const auto otherId = findTask(store, origin.url("/other"));
    EXPECT_EQ(origin.requests("/manual"), 0);
    filter->setCurrentIndex(0);
    ASSERT_TRUE(untilGui([&] { return selectTask(window, workId); }));
    widget<QAction>(&window, "resumeTasksAction")->trigger();
    ASSERT_TRUE(untilGui([&] { return selectTask(window, otherId); }));
    widget<QAction>(&window, "resumeTasksAction")->trigger();
    ASSERT_TRUE(
        untilGui([&] { return store.task(workId).downloaded > 0 && store.task(otherId).downloaded > 0; }));
    ASSERT_TRUE(selectTask(window, otherId));
    widget<QAction>(&window, "pauseTasksAction")->trigger();
    ASSERT_TRUE(untilGui([&] { return store.task(otherId).state == TaskState::Paused; }));
    EXPECT_EQ(store.task(otherId).runIntent, RunIntent::Paused);
    int stage = 0;
    QTimer driver;
    driver.setInterval(25);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        auto* dialog = QApplication::activeModalWidget();
        if (!dialog || dialog->objectName() != "queueManagerDialog")
            return;
        auto* list = widget<QTreeWidget>(dialog, "queueList");
        if (stage == 0 && list->topLevelItemCount() == 3 &&
            widget<QPushButton>(dialog, "queueSave")->isEnabled()) {
            for (int n = 0; n < list->topLevelItemCount(); ++n)
                if (list->topLevelItem(n)->data(0, Qt::UserRole).toString().toStdString() == work.id)
                    list->setCurrentItem(list->topLevelItem(n));
            stage = 1;
            widget<QPushButton>(dialog, "queuePause")->click();
        } else if (stage == 1 && widget<QPushButton>(dialog, "queueEnable")->isEnabled() &&
                   store.task(workId).state == TaskState::PolicyWaiting) {
            EXPECT_EQ(store.task(workId).runIntent, RunIntent::Run);
            EXPECT_TRUE(std::filesystem::exists(store.root / L"parts" / workId));
            EXPECT_FALSE(std::filesystem::exists(store.task(workId).spec.destination));
            widget<QSpinBox>(dialog, "queueDownloadLimit")->setValue(512);
            stage = 2;
            widget<QPushButton>(dialog, "queueSave")->click();
        } else if (stage == 2 && widget<QPushButton>(dialog, "queueEnable")->isEnabled()) {
            stage = 3;
            widget<QPushButton>(dialog, "queueEnable")->click();
        } else if (stage == 3 && widget<QPushButton>(dialog, "queuePause")->isEnabled()) {
            stage = 4;
            widget<QPushButton>(dialog, "queueClose")->click();
        }
    });
    driver.start();
    QTimer::singleShot(0, &window, [&] { widget<QAction>(&window, "manageQueuesAction")->trigger(); });
    ASSERT_TRUE(untilGui([&] { return stage == 4; }));
    driver.stop();
    ASSERT_TRUE(untilGui([&] { return store.task(workId).state == TaskState::Completed; }));
    QFile output(QString::fromStdWString(store.task(workId).spec.destination.wstring()));
    ASSERT_TRUE(output.open(QIODevice::ReadOnly));
    EXPECT_EQ(output.readAll(), origin.data());
    output.close();
    EXPECT_EQ(store.task(manualId).state, TaskState::Paused);
    EXPECT_EQ(store.task(otherId).state, TaskState::Paused);
    EXPECT_EQ(origin.requests("/manual"), 0);
    EXPECT_GE(origin.bodyRequests("/work"), 2);
    window.requestQuit();
}

TEST(QueueUi, CompletionDoesNotNotifyOrCountDownWhileUnstartedPausedTaskRemains) {
    platform();
    int argc = 1;
    char program[] = "QueueCompletionGuard";
    char* argv[]{program, nullptr};
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(program);
    Store store(true);
    QSettings().setValue("language", "en");
    QSettings().setValue("completionAction", 1);
    LocalHttpServer origin;
    MainWindow window(*store.service);
    window.show();
    DownloadSpec waiting;
    waiting.url = origin.url("/never-started").toStdString();
    waiting.destination = store.root / L"never.bin";
    waiting.startPaused = true;
    store.service->add(waiting).get();
    DownloadSpec running = waiting;
    running.url = origin.url("/completing").toStdString();
    running.destination = store.root / L"complete.bin";
    running.startPaused = false;
    const auto id = store.service->add(running).get();
    ASSERT_TRUE(untilGui([&] { return store.task(id).state == TaskState::Completed; }));
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_FALSE(window.statusBar()->currentMessage().contains("All downloads are complete"));
    EXPECT_TRUE(window.findChildren<QDialog*>().empty());
    EXPECT_EQ(origin.requests("/never-started"), 0);
    window.requestQuit();
}

TEST(QueueUi, RejectedLiveMediaLimitKeepsSettingsAndWorkerRateUnchanged) {
    platform();
    int argc = 1;
    char program[] = "QueueSettingsGuard";
    char* argv[]{program, nullptr};
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(program);
    const auto rate = std::make_shared<std::atomic<std::int64_t>>(-1);
    struct MediaWorker final : ITransferTransport {
        std::shared_ptr<std::atomic<std::int64_t>> rate;
        explicit MediaWorker(std::shared_ptr<std::atomic<std::int64_t>> value) : rate(std::move(value)) {}
        void run(const Task&, const std::filesystem::path&, TransferControl& control,
                 const std::function<void(TransferEvent)>& callback) override {
            TransferEvent event;
            event.phase = TaskState::Downloading;
            callback(event);
            while (!control.pause && !control.cancel) {
                *rate = control.speedLimit.load();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            event.outcome = control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused;
            callback(event);
        }
    };
    Store store(false, [rate] { return std::make_unique<MediaWorker>(rate); });
    QSettings().setValue("language", "en");
    QSettings().setValue("globalLimit", 300);
    QSettings().setValue("concurrentTasks", 3);
    MainWindow window(*store.service);
    window.show();
    store.service->configureLimits(3, 300 * 1024).get();
    DownloadSpec spec;
    spec.kind = TransferKind::Media;
    spec.url = "https://example.invalid/media";
    spec.destination = store.root / L"media.mkv";
    spec.mediaDownloader = std::filesystem::path(QCoreApplication::applicationFilePath().toStdWString());
    spec.mediaMuxer = spec.mediaDownloader;
    const auto id = store.service->add(spec).get();
    ASSERT_TRUE(untilGui([&] { return rate->load() == 100 * 1024; }));
    ModalBudget budget;
    int stage = 0;
    QTimer driver;
    driver.setInterval(25);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        if (!modal)
            return;
        if (auto* warning = qobject_cast<QMessageBox*>(modal)) {
            EXPECT_EQ(stage, 1);
            EXPECT_EQ(QSettings().value("globalLimit").toInt(), 300);
            stage = 2;
            warning->accept();
        } else if (modal->objectName() == "settingsDialog") {
            if (stage == 0) {
                EXPECT_EQ(widget<QSpinBox>(modal, "globalDownloadLimit")->value(), 300);
                widget<QSpinBox>(modal, "globalDownloadLimit")->setValue(1);
                stage = 1;
                widget<QPushButton>(modal, "saveGlobalSettings")->click();
            } else if (stage == 2) {
                stage = 3;
                qobject_cast<QDialog*>(modal)->reject();
            }
        }
    });
    driver.start();
    QTimer::singleShot(0, &window, [&] { widget<QAction>(&window, "globalSettingsAction")->trigger(); });
    ASSERT_TRUE(untilGui([&] { return stage == 3; }));
    driver.stop();
    EXPECT_EQ(QSettings().value("globalLimit").toInt(), 300);
    EXPECT_EQ(rate->load(), 100 * 1024);
    EXPECT_TRUE(store.service->lastError().empty());
    store.service->pause(id);
    ASSERT_TRUE(untilGui([&] { return store.task(id).state == TaskState::Paused; }));
    window.requestQuit();
}

TEST(QueueUi, TinyNativeAllocationUsesOneLiveConnectionAndPreservesPendingExtents) {
    platform();
    int argc = 1;
    char program[] = "QueueTinyNativeRate";
    char* argv[]{program, nullptr};
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(program);
    Store store(true);
    LocalHttpServer origin;
    DownloadSpec spec;
    spec.url = origin.url("/tiny").toStdString();
    spec.destination = store.root / L"tiny.bin";
    spec.connections = 8;
    spec.speedLimit = 1;
    const auto id = store.service->add(spec).get();
    ASSERT_TRUE(untilGui([&] { return origin.bodyRequests("/tiny") > 0; }));
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(origin.bodyRequests("/tiny"), 1);
    store.service->pause(id);
    ASSERT_TRUE(untilGui([&] { return store.task(id).state == TaskState::Paused; }));
    EXPECT_FALSE(std::filesystem::exists(spec.destination));
    std::ifstream identity(store.root / L"parts" / id / L"identity");
    std::string magic, etag, url;
    int version = 0, count = 0;
    std::int64_t total = 0;
    std::uint64_t generation = 0;
    ASSERT_TRUE(identity >> magic >> version >> std::quoted(etag) >> total >> std::quoted(url) >>
                generation >> count);
    EXPECT_EQ(magic, "MPHTTP");
    EXPECT_EQ(version, 2);
    EXPECT_EQ(count, 8);
    EXPECT_EQ(total, origin.data().size());
}
