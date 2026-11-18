#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/credential_vault.h>
#include <maskpilot/ui/add_download_dialog.h>
#include <maskpilot/ui/main_window.h>
#include <thread>

using namespace maskpilot;
namespace {
template <class T> T* field(QObject* parent, const char* name) {
    auto* value = parent->findChild<T*>(name);
    if (!value)
        throw std::runtime_error(std::string("Missing widget: ") + name);
    return value;
}
bool untilUi(const std::function<bool()>& predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(12);
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
struct NoNetwork final : ITransferTransport {
    void run(const Task&, const std::filesystem::path&, TransferControl&,
             const std::function<void(TransferEvent)>& callback) override {
        ADD_FAILURE() << "Paused widget flow started a transport";
        TransferEvent event;
        event.outcome = TransferOutcome::Failed;
        callback(event);
    }
};
struct UiStore {
    QTemporaryDir temporary;
    std::filesystem::path root = std::filesystem::path(temporary.path().toStdWString());
    std::unique_ptr<DownloadService> service;
    std::string installation;
    UiStore() {
        if (!temporary.isValid())
            throw std::runtime_error("Cannot create an owned UI fixture directory");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings settings;
        settings.setValue("language", qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI") ? "zh" : "en");
        settings.setValue("downloadDirectory", temporary.path());
        settings.setValue("closeToTray", false);
        open();
    }
    void open() {
        auto store = makeSqliteStore(root / L"tasks.db");
        installation = store.contexts->installationId();
        service = std::make_unique<DownloadService>(
            std::move(store.tasks), [] { return std::make_unique<NoNetwork>(); }, root / L"parts",
            ContextDependencies{std::move(store.contexts), makeNativeCredentialVault(installation)});
    }
    ProxyCredentialBinding binding(const std::string& reference) {
        auto store = makeSqliteStore(root / L"tasks.db");
        for (const auto& record : store.contexts->loadCredentialRecords())
            if (record.reference == reference)
                return {record.ownerId, record.proxyEndpoint, record.ownershipNonce};
        throw std::runtime_error("Missing exact UI credential journal entry");
    }
    ~UiStore() {
        service.reset();
        try {
            auto store = makeSqliteStore(root / L"tasks.db");
            auto vault = makeNativeCredentialVault(installation);
            for (const auto& record : store.contexts->loadCredentialRecords())
                vault->forget({record.ownerId, record.proxyEndpoint, record.ownershipNonce},
                              record.reference);
        } catch (...) {
            ADD_FAILURE() << "Owned UI credential cleanup failed";
        }
    }
};
struct ModalWatchdog {
    QTimer timer;
    ModalWatchdog() {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, [] {
            ADD_FAILURE() << "Owned widget flow exceeded its modal budget";
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                dialog->done(QDialog::Rejected);
            if (auto* popup = QApplication::activePopupWidget())
                popup->close();
        });
        timer.start(15000);
    }
};
void popupAction(MainWindow& window, QTableView* table, const char* name) {
    table->selectRow(0);
    bool triggered = false;
    QTimer picker;
    picker.setInterval(25);
    QObject::connect(&picker, &QTimer::timeout, &window, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu || triggered)
            return;
        auto* action = field<QAction>(menu, name);
        EXPECT_TRUE(action->isEnabled());
        triggered = true;
        action->trigger();
        menu->close();
    });
    picker.start();
    QTimer::singleShot(0, &window, [&] {
        QMetaObject::invokeMethod(table, "customContextMenuRequested", Qt::DirectConnection,
                                  Q_ARG(QPoint, QPoint(10, 10)));
    });
    EXPECT_TRUE(untilUi([&] { return triggered; }));
}
} // namespace

TEST(ContextUi, ManagerSavesProfileThenCreatesFilteredTaskAndRevokesCredential) {
    platform();
    int argc = 1;
    char name[] = "ContextWidgetFlow";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    ModalWatchdog watchdog;
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(name);
    UiStore fixture;
    ASSERT_TRUE(fixture.temporary.isValid());
    MainWindow window(*fixture.service);
    window.show();
    int stage = 0;
    QTimer driver;
    driver.setInterval(25);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        if (!modal || modal->objectName() != "contextManagerDialog")
            return;
        if (stage == 0 && field<QPushButton>(modal, "contextSave")->isEnabled()) {
            EXPECT_EQ(field<QComboBox>(modal, "contextCredentialStorage")->currentData().toString(),
                      "session");
            field<QLineEdit>(modal, "contextLabel")->setText("UI profile");
            field<QLineEdit>(modal, "contextDownloadRoot")->setText(fixture.temporary.path() + "/profile");
            field<QLineEdit>(modal, "contextProxyEndpoint")->setText("http://127.0.0.1:18081");
            field<QLineEdit>(modal, "contextProxyUsername")->setText("widget-user");
            auto* password = field<QLineEdit>(modal, "contextProxyPassword");
            EXPECT_EQ(password->echoMode(), QLineEdit::Password);
            password->setText("widget-password");
            field<QComboBox>(modal, "contextCredentialStorage")->setCurrentIndex(2);
            stage = 1;
            field<QPushButton>(modal, "contextSave")->click();
        } else if (stage == 1 && field<QTreeWidget>(modal, "contextList")->topLevelItemCount() == 1 &&
                   field<QPushButton>(modal, "contextSave")->isEnabled()) {
            EXPECT_TRUE(field<QLineEdit>(modal, "contextProxyPassword")->text().isEmpty());
            const auto screenshot = qEnvironmentVariable("MASKPILOT_TEST_CONTEXT_SCREENSHOT");
            if (!screenshot.isEmpty())
                EXPECT_TRUE(modal->grab().save(screenshot));
            stage = 2;
            field<QPushButton>(modal, "contextClose")->click();
        }
    });
    driver.start();
    QTimer::singleShot(0, &window, [&] { field<QAction>(&window, "manageContextsAction")->trigger(); });
    ASSERT_TRUE(untilUi([&] { return stage == 2; }));
    driver.stop();
    const auto profile = fixture.service->contexts().get().at(0);
    ASSERT_TRUE(isOpaqueId(profile.proxyCredentialRef));
    auto vault = makeNativeCredentialVault(fixture.installation);
    const auto binding = fixture.binding(profile.proxyCredentialRef);
    ASSERT_TRUE(vault->recall(binding, profile.proxyCredentialRef));
    auto* filter = field<QComboBox>(&window, "contextFilter");
    ASSERT_TRUE(untilUi([&] { return filter->findData(QString::fromStdString(profile.id)) >= 0; }));
    filter->setCurrentIndex(filter->findData(QString::fromStdString(profile.id)));
    QTimer::singleShot(50, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        ASSERT_NE(modal, nullptr);
        field<QLineEdit>(modal, "contextDownloadUrl")->setText("https://example.invalid/file");
        field<QLineEdit>(modal, "contextDownloadFilename")->setText("scoped.bin");
        field<QCheckBox>(modal, "contextDownloadPaused")->setChecked(true);
        field<QPushButton>(modal, "contextDownloadConfirm")->click();
    });
    window.newDownload();
    ASSERT_TRUE(untilUi([&] { return fixture.service->snapshot().size() == 1; }));
    const auto task = fixture.service->snapshot()[0];
    EXPECT_EQ(task.ownerId, profile.id);
    EXPECT_EQ(task.state, TaskState::Paused);
    EXPECT_EQ(task.spec.destination, fixture.root / L"profile" / L"scoped.bin");
    auto* table = field<QTableView>(&window, "downloadTable");
    DownloadSpec independent;
    independent.url = "https://example.invalid/standalone";
    independent.destination = fixture.root / L"standalone.bin";
    independent.startPaused = true;
    fixture.service->add(independent).get();
    ASSERT_EQ(fixture.service->snapshot().size(), 2);
    ASSERT_TRUE(untilUi([&] { return table->model()->rowCount() == 1; }));
    EXPECT_TRUE(table->model()->index(0, 0).data(Qt::UserRole + 2).toString().contains("UI profile"));
    popupAction(window, table, "rebindContextAction");
    stage = 3;
    QObject::disconnect(&driver, nullptr, &window, nullptr);
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        if (!modal || modal->objectName() != "contextManagerDialog")
            return;
        auto* tree = field<QTreeWidget>(modal, "contextList");
        if (stage == 3 && tree->topLevelItemCount() == 1 &&
            field<QPushButton>(modal, "contextSave")->isEnabled()) {
            tree->setCurrentItem(tree->topLevelItem(0));
            stage = 4;
            field<QPushButton>(modal, "contextRevoke")->click();
        } else if (stage == 4 && field<QPushButton>(modal, "contextReactivate")->isEnabled()) {
            field<QLineEdit>(modal, "contextProxyUsername")->setText("replacement-user");
            field<QLineEdit>(modal, "contextProxyPassword")->setText("replacement-password");
            field<QComboBox>(modal, "contextCredentialStorage")->setCurrentIndex(2);
            stage = 5;
            field<QPushButton>(modal, "contextReactivate")->click();
        } else if (stage == 5 && field<QPushButton>(modal, "contextSave")->isEnabled() &&
                   !field<QPushButton>(modal, "contextReactivate")->isEnabled()) {
            stage = 6;
            field<QPushButton>(modal, "contextRevoke")->click();
        } else if (stage == 6 && field<QPushButton>(modal, "contextReactivate")->isEnabled()) {
            stage = 7;
            field<QPushButton>(modal, "contextCleanupRetry")->click();
        } else if (stage == 7 && field<QPushButton>(modal, "contextCleanupRetry")->isEnabled() &&
                   field<QLabel>(modal, "contextStatus")
                       ->text()
                       .contains(qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI")
                                     ? QString::fromUtf8("清理完成")
                                     : QString("cleanup finished"))) {
            stage = 8;
            field<QPushButton>(modal, "contextClose")->click();
        }
    });
    driver.start();
    QTimer::singleShot(0, &window, [&] { field<QAction>(&window, "manageContextsAction")->trigger(); });
    ASSERT_TRUE(untilUi([&] { return stage == 8; }));
    driver.stop();
    EXPECT_FALSE(fixture.service->contexts().get()[0].enabled);
    EXPECT_TRUE(fixture.service->contexts().get()[0].proxyCredentialRef.empty());
    EXPECT_GE(fixture.service->contexts().get()[0].revision, profile.revision + 5);
    EXPECT_FALSE(vault->recall(binding, profile.proxyCredentialRef));
    window.requestQuit();
}

TEST(ContextUi, RememberCheckboxStoresAndReopenedEditorKeepsRequirementsUntilForget) {
    platform();
    int argc = 1;
    char name[] = "RememberWidgetFlow";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    ModalWatchdog watchdog;
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(name);
    UiStore fixture;
    ASSERT_TRUE(fixture.temporary.isValid());
    {
        MainWindow window(*fixture.service);
        window.show();
        QTimer::singleShot(50, &window, [&] {
            auto* dialog = QApplication::activeModalWidget();
            ASSERT_NE(dialog, nullptr);
            auto* remember = field<QCheckBox>(dialog, "rememberProxyCredentials");
            EXPECT_FALSE(remember->isChecked());
            field<QLineEdit>(dialog, "downloadDestination")
                ->setText(fixture.temporary.path() + "/stored.bin");
            field<QCheckBox>(dialog, "addPaused")->setChecked(true);
            field<QLineEdit>(dialog, "proxyEndpoint")->setText("http://127.0.0.1:18081");
            field<QLineEdit>(dialog, "proxyUsername")->setText("widget-user");
            field<QLineEdit>(dialog, "proxyPassword")->setText("widget-password");
            field<QLineEdit>(dialog, "requestCookie")->setText("session=widget-private");
            ASSERT_TRUE(remember->isEnabled());
            remember->setChecked(true);
            field<QPushButton>(dialog, "confirmDownload")->click();
        });
        window.newDownload("https://example.invalid/stored");
        ASSERT_TRUE(untilUi([&] { return fixture.service->snapshot().size() == 1; }));
        window.requestQuit();
    }
    auto task = fixture.service->snapshot()[0];
    ASSERT_TRUE(isOpaqueId(task.proxyCredentialRef));
    const auto binding = fixture.binding(task.proxyCredentialRef);
    auto vault = makeNativeCredentialVault(fixture.installation);
    ASSERT_TRUE(vault->recall(binding, task.proxyCredentialRef));
    fixture.service.reset();
    fixture.open();
    task = fixture.service->snapshot()[0];
    EXPECT_TRUE(task.originAuthenticationRequired);
    EXPECT_TRUE(task.spec.originAuthenticationRequired);
    EXPECT_TRUE(task.spec.cookie.empty());
    AddDownloadDialog editor(false);
    editor.setSpecification(task.spec);
    editor.setStoredProxyCredentials(true);
    EXPECT_TRUE(field<QCheckBox>(&editor, "rememberProxyCredentials")->isChecked());
    EXPECT_TRUE(field<QCheckBox>(&editor, "originAuthenticationRequired")->isChecked());
    EXPECT_TRUE(field<QLineEdit>(&editor, "proxyPassword")->text().isEmpty());
    auto refreshed = task.spec;
    refreshed.cookie = "session=old-source-private";
    refreshed.authorization = "Bearer old-source-private";
    refreshed.headers = {{"X-Source-Private", "old-source-private"}};
    refreshed.referrer = "https://example.invalid/private";
    editor.setSpecification(refreshed);
    auto* source = field<QLineEdit>(&editor, "downloadUrl");
    source->selectAll();
    QKeyEvent edit(QEvent::KeyPress, 0, Qt::NoModifier, "https://another.invalid/new-target");
    QApplication::sendEvent(source, &edit);
    EXPECT_EQ(source->text(), "https://another.invalid/new-target");
    const auto changedSource = editor.specification();
    EXPECT_TRUE(changedSource.cookie.empty());
    EXPECT_TRUE(changedSource.authorization.empty());
    EXPECT_TRUE(changedSource.headers.empty());
    EXPECT_TRUE(changedSource.referrer.empty());
    EXPECT_TRUE(changedSource.originAuthenticationRequired);
    EXPECT_EQ(changedSource.proxy, refreshed.proxy);
    fixture.service->updateRequestWithCredentials(task.id, changedSource, true).get();
    fixture.service->resume(task.id);
    ASSERT_TRUE(untilUi([&] { return fixture.service->snapshot()[0].state == TaskState::NeedsAttention; }));
    auto ftp = refreshed;
    ftp.url = "ftp://example.invalid/private";
    ftp.cookie.clear();
    ftp.authorization.clear();
    ftp.headers.clear();
    ftp.ftpUsername = "old-ftp-user";
    ftp.ftpPassword = "old-ftp-password";
    editor.setSpecification(ftp);
    source->selectAll();
    QKeyEvent ftpEdit(QEvent::KeyPress, 0, Qt::NoModifier, "ftp://another.invalid/new-target");
    QApplication::sendEvent(source, &ftpEdit);
    EXPECT_EQ(source->text(), "ftp://another.invalid/new-target");
    EXPECT_TRUE(editor.specification().ftpUsername.empty());
    EXPECT_TRUE(editor.specification().ftpPassword.empty());
    EXPECT_TRUE(editor.specification().originAuthenticationRequired);
    MainWindow window(*fixture.service);
    window.show();
    auto* table = field<QTableView>(&window, "downloadTable");
    ASSERT_TRUE(untilUi([&] { return table->model()->rowCount() == 1; }));
    popupAction(window, table, "forgetProxyCredentialsAction");
    ASSERT_TRUE(untilUi([&] { return fixture.service->snapshot()[0].proxyCredentialRef.empty(); }));
    EXPECT_FALSE(vault->recall(binding, task.proxyCredentialRef));
    EXPECT_TRUE(fixture.service->snapshot()[0].spec.proxyAuthenticationRequired);
    window.requestQuit();
}
