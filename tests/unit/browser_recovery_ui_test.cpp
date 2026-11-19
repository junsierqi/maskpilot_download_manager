#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QMenu>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <chrono>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/ui/main_window.h>
#include <thread>

using namespace maskpilot;
class RecoveryUrlReceiver final : public QObject {
    Q_OBJECT
  public:
    QUrl opened;
  public slots:
    void capture(const QUrl& url) { opened = url; }
};
namespace {
struct NoRecoveryNetwork final : ITransferTransport {
    void run(const Task&, const std::filesystem::path&, TransferControl&,
             const std::function<void(TransferEvent)>& callback) override {
        ADD_FAILURE() << "Source-page recovery action unexpectedly started a download";
        TransferEvent event;
        event.outcome = TransferOutcome::Failed;
        callback(event);
    }
};
bool recoveryUiUntil(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do {
        QCoreApplication::processEvents();
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
struct RecoveryUrlHandler {
    explicit RecoveryUrlHandler(RecoveryUrlReceiver& receiver) {
        QDesktopServices::setUrlHandler("https", &receiver, "capture");
    }
    ~RecoveryUrlHandler() { QDesktopServices::unsetUrlHandler("https"); }
};
} // namespace
TEST(BrowserRecoveryUi, ActualSourceActionOpensOwnedHandlerAndKeepsOriginalTaskPaused) {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char name[] = "BrowserRecoveryWidget";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(name);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings settings;
    settings.setValue("downloadDirectory", temporary.path());
    settings.setValue("closeToTray", false);
    settings.setValue("language", "en");
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    DownloadService service(
        makeSqliteRepository(root / "tasks.db"), [] { return std::make_unique<NoRecoveryNetwork>(); },
        root / "parts");
    DownloadSpec spec;
    spec.url = "https://example.test/expired";
    spec.referrer = "https://example.test/source";
    spec.destination = root / "original.bin";
    spec.startPaused = true;
    const auto taskId = service.add(spec).get();
    RecoveryUrlReceiver receiver;
    RecoveryUrlHandler handler(receiver);
    MainWindow window(service);
    window.show();
    auto* table = window.findChild<QTableView*>("downloadTable");
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(recoveryUiUntil([&] { return table->model()->rowCount() == 1; }));
    table->selectRow(0);
    bool triggered = false;
    QTimer picker;
    picker.setInterval(20);
    QObject::connect(&picker, &QTimer::timeout, &window, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu || triggered)
            return;
        auto* action = menu->findChild<QAction*>("browserRecoverySourceAction");
        if (!action)
            return;
        EXPECT_TRUE(action->isEnabled());
        triggered = true;
        action->trigger();
        menu->close();
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &window, [&] {
        ADD_FAILURE() << "Owned recovery source action exceeded its UI budget";
        if (auto* menu = QApplication::activePopupWidget())
            menu->close();
    });
    picker.start();
    watchdog.start(6000);
    QTimer::singleShot(0, &window, [&] {
        QMetaObject::invokeMethod(table, "customContextMenuRequested", Qt::DirectConnection,
                                  Q_ARG(QPoint, QPoint(10, 10)));
    });
    ASSERT_TRUE(recoveryUiUntil([&] { return triggered && !receiver.opened.isEmpty(); }));
    picker.stop();
    watchdog.stop();
    EXPECT_EQ(receiver.opened, QUrl("https://example.test/source"));
    const auto tasks = service.snapshot();
    ASSERT_EQ(tasks.size(), 1u);
    EXPECT_EQ(tasks.front().id, taskId);
    EXPECT_EQ(tasks.front().spec.url, spec.url);
    EXPECT_EQ(tasks.front().spec.destination, spec.destination);
    EXPECT_EQ(tasks.front().state, TaskState::Paused);
    if (qEnvironmentVariableIsSet("MASKPILOT_TEST_RECOVERY_SCREENSHOT"))
        EXPECT_TRUE(window.grab().save(qEnvironmentVariable("MASKPILOT_TEST_RECOVERY_SCREENSHOT")));
    window.close();
}
#include "browser_recovery_ui_test.moc"
