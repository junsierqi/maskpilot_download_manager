#include "../windows/main_window_p.h"
#include "async_ui.h"
namespace maskpilot {
namespace {
class SettingsDialog final : public QDialog {
  public:
    using QDialog::QDialog;
    bool saving = false;
    void reject() override {
        if (!saving)
            QDialog::reject();
    }
};
} // namespace
void MainWindow::Impl::settingsDialog() {
    SettingsDialog dialog(window);
    dialog.setObjectName("settingsDialog");
    dialog.setWindowTitle(t("Settings", "设置"));
    dialog.resize(560, 450);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto* folder = new QLineEdit(
        settings
            .value("downloadDirectory", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
            .toString());
    auto* pathRow = new QWidget;
    auto* pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->addWidget(folder);
    auto* browse = new QPushButton(t("Browse…", "浏览…"));
    pathLayout->addWidget(browse);
    form->addRow(t("Download folder", "下载目录"), pathRow);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        auto selected = QFileDialog::getExistingDirectory(&dialog, dialog.windowTitle(), folder->text());
        if (!selected.isEmpty())
            folder->setText(selected);
    });
    auto* concurrent = new QSpinBox;
    concurrent->setObjectName("globalConcurrentTasks");
    concurrent->setRange(1, 8);
    concurrent->setValue(settings.value("concurrentTasks", 3).toInt());
    form->addRow(t("Simultaneous downloads", "同时下载任务数"), concurrent);
    auto* connections = new QSpinBox;
    connections->setRange(1, 8);
    connections->setValue(settings.value("connections", 4).toInt());
    form->addRow(t("Default connections", "默认连接数"), connections);
    auto* categoryFolders =
        new QCheckBox(t("Put new downloads in category subfolders", "按分类保存到子目录"));
    categoryFolders->setChecked(settings.value("categorySubfolders", false).toBool());
    form->addRow(categoryFolders);
    auto* categoryRules = new QPlainTextEdit(
        settings
            .value("categoryRules",
                   "Documents=pdf,doc,docx,xls,xlsx,ppt,pptx,txt\nSoftware=exe,msi,msix\nVideo=mp4,mkv,"
                   "webm,mov\nMusic=mp3,flac,wav,m4a\nArchives=zip,7z,rar,tar,gz")
            .toString());
    categoryRules->setMaximumHeight(80);
    categoryRules->setPlaceholderText("Category=ext1,ext2");
    form->addRow(t("Category rules", "分类规则"), categoryRules);
    auto* limit = new QSpinBox;
    limit->setObjectName("globalDownloadLimit");
    limit->setRange(0, 1000000);
    limit->setSpecialValueText(t("Unlimited", "不限速"));
    limit->setSuffix(" KiB/s");
    limit->setValue(settings.value("globalLimit", 0).toInt());
    form->addRow(t("Global speed limit", "全局限速"), limit);
    auto* upload = new QSpinBox;
    upload->setObjectName("globalUploadLimit");
    upload->setRange(0, 1000000);
    upload->setSpecialValueText(t("Unlimited", "不限速"));
    upload->setSuffix(" KiB/s");
    upload->setValue(settings.value("globalUploadLimit", 0).toInt());
    form->addRow(t("BT upload limit", "BT 全局上传限速"), upload);
    auto* proxy = new QLineEdit(settings.value("proxy").toString());
    proxy->setPlaceholderText(
        t("Empty: direct; credentials are entered per task", "留空直连；凭据在任务的代理页输入"));
    form->addRow(t("Proxy URL", "代理地址"), proxy);
    auto* language = new QComboBox;
    language->addItem("简体中文", "zh");
    language->addItem("English", "en");
    language->setCurrentIndex(chinese ? 0 : 1);
    form->addRow(t("Language (restart)", "语言（重启生效）"), language);
    auto* trayOption =
        new QCheckBox(t("Keep downloading when the window closes", "关闭窗口后在托盘继续下载"));
    trayOption->setChecked(settings.value("closeToTray", true).toBool());
    form->addRow(trayOption);
    auto* completeAction = new QComboBox;
    completeAction->addItems({t("Notify only", "仅通知"), t("Exit application", "退出程序"),
                              t("Shut down computer", "关闭计算机")});
    completeAction->setCurrentIndex(settings.value("completionAction", 0).toInt());
    form->addRow(t("When queue completes", "队列完成后"), completeAction);
    auto* notice = new QLabel(t("Power actions show a 60-second cancel countdown. Authentication headers "
                                "are kept only in memory.",
                                "电源操作会显示 60 秒倒计时，可取消。认证信息仅保存在内存中。"));
    notice->setWordWrap(true);
    notice->setStyleSheet("color:#738198;");
    layout->addWidget(notice);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setObjectName("saveGlobalSettings");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (dialog.saving)
            return;
        DownloadSpec check;
        check.url = "https://example.invalid/file";
        check.destination = std::filesystem::path(folder->text().toStdU16String()) / L"test";
        check.proxy = proxy->text().toStdString();
        auto error = validateSpec(check);
        if (!error.empty()) {
            QMessageBox::warning(&dialog, dialog.windowTitle(), qs(error));
            return;
        }
        dialog.saving = true;
        for (auto* child : dialog.findChildren<QWidget*>())
            child->setEnabled(false);
        ui::watchFuture(
            service.configureLimits(concurrent->value(), static_cast<std::int64_t>(limit->value()) * 1024,
                                    static_cast<std::int64_t>(upload->value()) * 1024),
            &dialog,
            [&] {
                settings.setValue("downloadDirectory", folder->text());
                settings.setValue("concurrentTasks", concurrent->value());
                settings.setValue("connections", connections->value());
                settings.setValue("categorySubfolders", categoryFolders->isChecked());
                settings.setValue("categoryRules", categoryRules->toPlainText());
                settings.setValue("globalLimit", limit->value());
                settings.setValue("globalUploadLimit", upload->value());
                settings.setValue("proxy", proxy->text());
                settings.setValue("language", language->currentData());
                settings.setValue("closeToTray", trayOption->isChecked());
                settings.setValue("completionAction", completeAction->currentIndex());
                settings.sync();
                dialog.saving = false;
                dialog.accept();
            },
            [&](QString failure) {
                dialog.saving = false;
                for (auto* child : dialog.findChildren<QWidget*>())
                    child->setEnabled(true);
                QMessageBox::warning(&dialog, dialog.windowTitle(), failure);
            });
    });
    dialog.exec();
}
} // namespace maskpilot
