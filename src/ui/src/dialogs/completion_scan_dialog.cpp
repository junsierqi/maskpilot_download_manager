#include "../windows/main_window_p.h"
#include "async_ui.h"
#include <maskpilot/ui/completion_scan_settings.h>
namespace maskpilot {
namespace {
class ScanDialog final : public QDialog {
  public:
    using QDialog::QDialog;
    bool saving = false;
    void reject() override {
        if (!saving)
            QDialog::reject();
    }
};
} // namespace
void MainWindow::Impl::completionScanDialog() {
    ScanDialog dialog(window);
    dialog.setObjectName("completionScanDialog");
    dialog.setWindowTitle(t("Completion scanning", "完成后扫描"));
    dialog.resize(680, 530);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(
        t("Scan verified temporary files before publishing them. Choose a trusted scanner that reports "
          "success with exit code 0. This optional integration does not include an antivirus engine.",
          "在发布文件前扫描已校验的临时文件。请选择以退出码 0 "
          "表示通过的可信扫描程序。此可选功能不包含杀毒引擎。"));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* enabled = new QCheckBox(t("Enable completion scanning", "启用完成后扫描"));
    enabled->setObjectName("scanEnabled");
    layout->addWidget(enabled);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto* program = new QLineEdit;
    program->setObjectName("scanProgram");
    auto* row = new QWidget;
    auto* horizontal = new QHBoxLayout(row);
    horizontal->setContentsMargins(0, 0, 0, 0);
    horizontal->addWidget(program);
    auto* browse = new QPushButton(t("Browse…", "浏览…"));
    horizontal->addWidget(browse);
    form->addRow(t("Trusted executable", "可信程序"), row);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        auto path =
            QFileDialog::getOpenFileName(&dialog, dialog.windowTitle(), program->text(), "Programs (*.exe)");
        if (!path.isEmpty())
            program->setText(path);
    });
    auto* arguments = new QPlainTextEdit;
    arguments->setObjectName("scanArguments");
    arguments->setPlaceholderText("{file}");
    form->addRow(t("Arguments (one per line)", "参数（每行一个）"), arguments);
    auto* timeout = new QSpinBox;
    timeout->setObjectName("scanTimeout");
    timeout->setRange(1, 3600);
    timeout->setSuffix(t(" seconds", " 秒"));
    form->addRow(t("Timeout", "超时"), timeout);
    auto* notice =
        new QLabel(t("Include exactly one complete {file} argument. Arguments are literal: do not add shell "
                     "quotes. BT scans a temporary directory. Pause active tasks before changing settings. "
                     "Disabling does not waive existing task requirements; use the task menu explicitly.",
                     "必须有且仅有一行完整的 {file}。参数按原样传入，无需添加命令行引号。BT "
                     "扫描临时目录。修改前请暂停活动任务。关闭功能不会解除已有任务的扫描要求；如需跳过，请在"
                     "任务菜单中明确操作。"));
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto* status = new QLabel;
    status->setObjectName("scanSettingsStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setStyleSheet("color:#b45309;");
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto* save = buttons->button(QDialogButtonBox::Save);
    save->setObjectName("saveCompletionScan");
    save->setEnabled(false);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    ui::watchFuture(
        service.completionScanOptions(), &dialog,
        [&](CompletionScanOptions value) {
            enabled->setChecked(value.enabled);
            program->setText(pathString(value.program));
            QStringList lines;
            for (const auto& argument : value.arguments)
                lines.push_back(qs(argument));
            arguments->setPlainText(lines.join('\n'));
            timeout->setValue(value.timeoutSeconds);
            save->setEnabled(true);
        },
        [status](QString issue) { status->setText(issue); });
    ui::watchFuture(
        service.completionScanIssue(), &dialog, [status](std::string issue) { status->setText(qs(issue)); },
        [status](QString issue) { status->setText(issue); });
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] {
        if (dialog.saving)
            return;
        CompletionScanOptions value;
        value.enabled = enabled->isChecked();
        value.program = std::filesystem::path(program->text().toStdWString());
        value.arguments.clear();
        for (const auto& line : arguments->toPlainText().split('\n', Qt::KeepEmptyParts))
            value.arguments.push_back(line.toUtf8().toStdString());
        value.timeoutSeconds = timeout->value();
        dialog.saving = true;
        buttons->setEnabled(false);
        ui::watchFuture(
            service.configureCompletionScan(value), &dialog,
            [&, value] {
                try {
                    ui::saveCompletionScanSettings(settings, value);
                    dialog.saving = false;
                    dialog.accept();
                } catch (const std::exception& error) {
                    status->setText(QString::fromUtf8(error.what()));
                    dialog.saving = false;
                    buttons->setEnabled(true);
                }
            },
            [&](QString issue) {
                status->setText(issue);
                dialog.saving = false;
                buttons->setEnabled(true);
            });
    });
    dialog.exec();
}
} // namespace maskpilot
