#include "../support/local_http_server.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/ui/completion_scan_settings.h>
#include <maskpilot/ui/main_window.h>
#include <sqlite3.h>

using namespace maskpilot;
namespace {
template <class T> T* control(QObject* parent, const char* name) {
    auto* value = parent->findChild<T*>(name);
    if (!value)
        throw std::runtime_error(std::string("Missing scanner UI control: ") + name);
    return value;
}
bool until(const std::function<bool()>& condition, int milliseconds = 15000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return condition();
}
void platform() {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
}
void configure(MainWindow& window, const std::function<void(QDialog*)>& edit,
               const std::function<bool(QDialog*)>& afterSave = {}) {
    QTimer driver, budget;
    bool edited = false, finished = false;
    driver.setInterval(10);
    QObject::connect(&driver, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "completionScanDialog")
            return;
        auto* save = control<QPushButton>(dialog, "saveCompletionScan");
        if (!save->isEnabled())
            return;
        if (!edited) {
            edited = true;
            edit(dialog);
            save->click();
            if (!afterSave)
                driver.stop();
        } else if (afterSave && afterSave(dialog)) {
            finished = true;
            driver.stop();
            dialog->reject();
        }
    });
    budget.setSingleShot(true);
    QObject::connect(&budget, &QTimer::timeout, [&] {
        ADD_FAILURE() << "Owned scan dialog exceeded its 15-second fixture budget";
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
            dialog->done(QDialog::Rejected);
    });
    driver.start();
    budget.start(15000);
    control<QAction>(&window, "completionScanSettingsAction")->trigger();
    EXPECT_TRUE(edited);
    if (afterSave)
        EXPECT_TRUE(finished);
}
void requirement(MainWindow&, QTableView* table) {
    table->selectRow(0);
    bool triggered = false;
    QTimer picker, budget;
    picker.setInterval(10);
    QObject::connect(&picker, &QTimer::timeout, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu)
            return;
        auto* action = menu->findChild<QAction*>("scanRequirementAction");
        if (!action)
            return;
        EXPECT_TRUE(action->isEnabled());
        triggered = true;
        action->trigger();
        menu->close();
    });
    budget.setSingleShot(true);
    QObject::connect(&budget, &QTimer::timeout, [&] {
        ADD_FAILURE() << "Owned scan requirement menu exceeded fixture budget";
        if (auto* menu = QApplication::activePopupWidget())
            menu->close();
    });
    picker.start();
    budget.start(6000);
    QMetaObject::invokeMethod(table, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, QPoint(10, 10)));
    EXPECT_TRUE(triggered);
}
struct OwnedDesktop {
    QProcess process;
    ~OwnedDesktop() {
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(5000)) {
                process.kill();
                process.waitForFinished(5000);
            }
        }
    }
};
int storedScanState(const std::filesystem::path& database, bool completed) {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(database.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (raw)
            sqlite3_close(raw);
        return -1;
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
    sqlite3_stmt* query = nullptr;
    const char* sql = completed ? "SELECT scan_state FROM tasks WHERE state=9 AND scan_required=1"
                                : "SELECT scan_state FROM tasks WHERE state=13 AND scan_required=1";
    if (sqlite3_prepare_v2(raw, sql, -1, &query, nullptr) != SQLITE_OK)
        return -1;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(query, sqlite3_finalize);
    return sqlite3_step(query) == SQLITE_ROW ? sqlite3_column_int(query, 0) : -1;
}
} // namespace

TEST(CompletionScanUi, ActualSettingsPauseWaiverAndDetailsPreservePublicationRequirement) {
    platform();
    int argc = 1;
    char name[] = "CompletionScanWidgets";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(name);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings settings;
    settings.setValue("language", "en");
    settings.setValue("closeToTray", false);
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    LocalHttpServer origin;
    DownloadService service(makeSqliteRepository(root / L"tasks.db"), [] { return makeCurlTransfer(); },
                            root / L"parts", {}, {}, {{}, makeCompletionScanner});
    MainWindow window(service);
    window.show();
    const auto program = QString::fromUtf8(MASKPILOT_TEST_SCAN_PROBE);
    configure(window, [&](QDialog* dialog) {
        control<QCheckBox>(dialog, "scanEnabled")->setChecked(true);
        control<QLineEdit>(dialog, "scanProgram")->setText(program);
        control<QPlainTextEdit>(dialog, "scanArguments")->setPlainText("--mode\nsleep\n{file}");
        control<QSpinBox>(dialog, "scanTimeout")->setValue(30);
        if (qEnvironmentVariableIsSet("MASKPILOT_TEST_SCAN_SETTINGS_SCREENSHOT"))
            EXPECT_TRUE(dialog->grab().save(qEnvironmentVariable("MASKPILOT_TEST_SCAN_SETTINGS_SCREENSHOT")));
    });
    auto saved = ui::loadCompletionScanSettings(settings);
    EXPECT_TRUE(saved.enabled);
    EXPECT_EQ(saved.program, std::filesystem::path(program.toStdWString()));
    EXPECT_EQ(saved.timeoutSeconds, 30);
    configure(
        window,
        [&](QDialog* dialog) {
            EXPECT_EQ(control<QLineEdit>(dialog, "scanProgram")->text(), program);
            control<QLineEdit>(dialog, "scanProgram")->setText(temporary.path() + "/missing.exe");
        },
        [](QDialog* dialog) { return !control<QLabel>(dialog, "scanSettingsStatus")->text().isEmpty(); });
    EXPECT_EQ(ui::loadCompletionScanSettings(settings).program, saved.program);
    EXPECT_EQ(service.completionScanOptions().get().program, saved.program);
    DownloadSpec spec;
    spec.url = origin.url("/ui-scan").toStdString();
    spec.destination = root / L"final.bin";
    spec.startPaused = true;
    const auto id = service.add(spec).get();
    auto task = [&] { return service.snapshot().front(); };
    auto* table = control<QTableView>(&window, "downloadTable");
    ASSERT_TRUE(
        until([&] { return table->model()->rowCount() == 1 && !window.findChildren<QComboBox*>().empty(); }));
    table->selectRow(0);
    control<QAction>(&window, "resumeTasksAction")->trigger();
    ASSERT_TRUE(until([&] { return task().scanState == ScanState::Scanning; }));
    EXPECT_FALSE(std::filesystem::exists(spec.destination));
    ASSERT_TRUE(until([&] { return table->model()->index(0, 4).data().toString() == "Scanning"; }));
    configure(
        window, [](QDialog* dialog) { control<QCheckBox>(dialog, "scanEnabled")->setChecked(false); },
        [](QDialog* dialog) {
            return control<QLabel>(dialog, "scanSettingsStatus")->text().contains("Pause active");
        });
    EXPECT_TRUE(ui::loadCompletionScanSettings(settings).enabled);
    table->selectRow(0);
    control<QAction>(&window, "pauseTasksAction")->trigger();
    ASSERT_TRUE(until([&] { return task().state == TaskState::Paused; }));
    configure(window, [](QDialog* dialog) { control<QCheckBox>(dialog, "scanEnabled")->setChecked(false); });
    EXPECT_FALSE(ui::loadCompletionScanSettings(settings).enabled);
    ASSERT_TRUE(until([&] {
        return table->model()->index(0, 4).data().toString() ==
               QString::fromUtf8(stateName(TaskState::Paused));
    }));
    const auto requests = origin.requests("/ui-scan");
    table->selectRow(0);
    ASSERT_TRUE(control<QAction>(&window, "resumeTasksAction")->isEnabled());
    control<QAction>(&window, "resumeTasksAction")->trigger();
    ASSERT_TRUE(until([&] { return task().state == TaskState::NeedsAttention; }));
    EXPECT_EQ(origin.requests("/ui-scan"), requests);
    // Wait for the real table refresh before opening the action built from its snapshot.
    ASSERT_TRUE(until([&] {
        return table->model()->index(0, 4).data().toString() == "Needs attention" ||
               table->model()->index(0, 4).data().toString() ==
                   QString::fromUtf8(stateName(TaskState::NeedsAttention));
    }));
    requirement(window, table);
    ASSERT_TRUE(
        until([&] { return task().scanState == ScanState::Waived && task().state == TaskState::Paused; }));
    ASSERT_TRUE(until([&] {
        return table->model()->index(0, 0).data(Qt::ToolTipRole).toString().contains("Explicitly skipped");
    }));
    table->selectRow(0);
    control<QAction>(&window, "taskDetailsAction")->trigger();
    auto* details = control<QDialog>(&window, "taskDetailsDialog");
    EXPECT_TRUE(
        control<QPlainTextEdit>(details, "taskDetailsText")->toPlainText().contains("Explicitly skipped"));
    details->close();
    requirement(window, table);
    ASSERT_TRUE(until([&] {
        return task().scanRequired && task().scanState == ScanState::Pending &&
               task().state == TaskState::Paused;
    }));
    ASSERT_TRUE(until([&] {
        return table->model()->index(0, 0).data(Qt::ToolTipRole).toString().contains("Required; pending");
    }));
    requirement(window, table);
    ASSERT_TRUE(until([&] {
        return !task().scanRequired && task().scanState == ScanState::Waived &&
               task().state == TaskState::Paused;
    }));
    ASSERT_TRUE(until([&] {
        return table->model()->index(0, 0).data(Qt::ToolTipRole).toString().contains("Explicitly skipped");
    }));
    if (qEnvironmentVariableIsSet("MASKPILOT_TEST_SCAN_SCREENSHOT"))
        EXPECT_TRUE(window.grab().save(qEnvironmentVariable("MASKPILOT_TEST_SCAN_SCREENSHOT")));
    table->selectRow(0);
    ASSERT_TRUE(control<QAction>(&window, "resumeTasksAction")->isEnabled());
    control<QAction>(&window, "resumeTasksAction")->trigger();
    ASSERT_TRUE(until([&] { return task().state == TaskState::Completed; }));
    EXPECT_EQ(task().id, id);
    EXPECT_EQ(task().scanState, ScanState::Waived);
    QFile final(QString::fromStdWString(spec.destination.wstring()));
    ASSERT_TRUE(final.open(QIODevice::ReadOnly));
    EXPECT_EQ(final.readAll(), origin.data());
    window.close();
}

TEST(CompletionScanUi, ActualDesktopStartupLoadsPersistedPolicyAndMissingToolRemainsRepairable) {
    platform();
    int argc = 1;
    char name[] = "CompletionScanStartup";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    LocalHttpServer origin;
    for (bool available : {false, true}) {
        const auto directory = temporary.path() + (available ? "/valid" : "/missing");
        ASSERT_TRUE(QDir().mkpath(directory));
        const auto root = std::filesystem::path(directory.toStdWString());
        CompletionScanOptions options;
        options.enabled = true;
        options.program =
            available ? std::filesystem::path(MASKPILOT_TEST_SCAN_PROBE) : root / L"missing.exe";
        options.arguments = {"--record", (root / L"probe.json").string(), "--final",
                             (root / L"final.bin").string(), "{file}"};
        QSettings settings(directory + "/MaskPilot/MaskPilot.ini", QSettings::IniFormat);
        settings.setValue("language", "en");
        ui::saveCompletionScanSettings(settings, options);
        {
            auto store = makeSqliteStore(root / L"tasks.db");
            auto queue = store.queues->loadQueues().front();
            const auto revision = queue.revision;
            ++queue.revision;
            queue.dailyPayloadBudget = 64 * 1024 * 1024;
            store.queues->saveQueue(queue, revision);
            Task task;
            task.id = newTaskId();
            task.spec.url = origin.url(available ? "/startup-pass" : "/startup-blocked").toStdString();
            task.spec.destination = root / L"final.bin";
            task.state = TaskState::PolicyWaiting;
            task.runIntent = RunIntent::Run;
            store.tasks->save(task);
        }
        const auto before = origin.requests("/startup-blocked");
        OwnedDesktop desktop;
        desktop.process.setStandardOutputFile(QProcess::nullDevice());
        desktop.process.setStandardErrorFile(QProcess::nullDevice());
        desktop.process.start(QString::fromUtf8(MASKPILOT_TEST_DESKTOP),
                              {"--background", "--data-dir", directory});
        ASSERT_TRUE(desktop.process.waitForStarted(5000));
        ASSERT_TRUE(until(
            [&] {
                // Service state can be persisted before GUI/IPC initialization finishes.
                return QFile::exists(directory + "/ipc.token") &&
                       storedScanState(root / L"tasks.db", available) == (available ? 3 : 1);
            },
            20000));
        EXPECT_EQ(desktop.process.state(), QProcess::Running);
        if (available) {
            QFile final(directory + "/final.bin");
            ASSERT_TRUE(final.open(QIODevice::ReadOnly));
            EXPECT_EQ(final.readAll(), origin.data());
            QFile record(directory + "/probe.json");
            ASSERT_TRUE(record.open(QIODevice::ReadOnly));
            const auto json = QJsonDocument::fromJson(record.readAll()).object();
            EXPECT_FALSE(json["finalExists"].toBool(true));
            EXPECT_NE(json["path"].toString(), directory + "/final.bin");
            ASSERT_EQ(json["files"].toArray().size(), 1);
            EXPECT_EQ(json["files"].toArray()[0].toObject()["sha256"].toString(),
                      QString::fromLatin1(
                          QCryptographicHash::hash(origin.data(), QCryptographicHash::Sha256).toHex()));
        } else {
            EXPECT_EQ(origin.requests("/startup-blocked"), before);
            EXPECT_FALSE(QFile::exists(directory + "/final.bin"));
            EXPECT_TRUE(QFile::exists(directory + "/ipc.token"));
        }
    }
}
