#include "../windows/main_window_p.h"
#include "async_ui.h"
#include "download_name.h"
#include <QHBoxLayout>
#include <QTreeWidget>

namespace maskpilot {
namespace {
class SubmissionDialog final : public QDialog {
  public:
    using QDialog::QDialog;
    bool submitted = false;
    void reject() override {
        if (!submitted)
            QDialog::reject();
    }
};
class ContextManagerDialog final : public QDialog {
    DownloadService& service_;
    bool chinese_, busy_ = false;
    std::vector<DownloadContext> contexts_;
    std::string selected_;
    QTreeWidget* list_;
    QLineEdit *label_, *root_, *proxy_, *username_, *password_;
    QCheckBox *proxyRequired_, *originRequired_;
    QComboBox* storage_;
    QLabel* status_;
    QWidget* editor_ = nullptr;
    QPushButton *save_, *create_, *revoke_, *reactivate_, *cleanup_;
    QString tr(const char* en, const char* zh) const { return QString::fromUtf8(chinese_ ? zh : en); }
    const DownloadContext* current() const {
        const auto found = std::find_if(contexts_.begin(), contexts_.end(),
                                        [&](const auto& value) { return value.id == selected_; });
        return found == contexts_.end() ? nullptr : &*found;
    }
    void busy(bool value) {
        busy_ = value;
        list_->setEnabled(!value);
        create_->setEnabled(!value);
        save_->setEnabled(!value);
        cleanup_->setEnabled(!value);
        if (editor_)
            editor_->setEnabled(!value);
        const auto* context = current();
        revoke_->setEnabled(!value && context);
        reactivate_->setEnabled(!value && context && !context->enabled);
    }
    void failure(QString error) {
        busy(false);
        status_->setText(error);
    }
    void load(QString message = {}) {
        busy(true);
        ui::watchFuture(
            service_.contexts(), this,
            [this, message](std::vector<DownloadContext> values) {
                contexts_ = std::move(values);
                const QSignalBlocker blocker(list_);
                list_->clear();
                for (const auto& value : contexts_) {
                    auto* item = new QTreeWidgetItem(
                        list_,
                        {qs(value.label), value.enabled ? tr("Available", "可用") : tr("Revoked", "已撤销")});
                    item->setData(0, Qt::UserRole, qs(value.id));
                    item->setToolTip(0, pathString(value.downloadRoot));
                    if (value.id == selected_)
                        list_->setCurrentItem(item);
                }
                if (current())
                    showCurrent();
                busy(false);
                if (!message.isEmpty())
                    status_->setText(message);
            },
            [this](QString error) { failure(error); });
    }
    void showCurrent() {
        const auto* value = current();
        if (!value)
            return;
        label_->setText(qs(value->label));
        root_->setText(pathString(value->downloadRoot));
        root_->setModified(false);
        proxy_->setText(qs(value->proxyEndpoint));
        username_->clear();
        password_->clear();
        proxyRequired_->setChecked(value->proxyAuthenticationRequired);
        originRequired_->setChecked(value->originAuthenticationRequired);
        storage_->setCurrentIndex(0);
        status_->setText(
            !value->enabled ? tr("Revoked. Accepted files are preserved. Reactivate explicitly to use this "
                                 "profile again.",
                                 "环境已撤销，最终文件已保留。再次使用前请明确恢复环境。")
            : !value->proxyCredentialRef.empty()
                ? tr("Proxy credentials are saved in Windows. Leave fields empty to retain them.",
                     "代理凭据已由 Windows 安全保存；保留配置时可不填写账号密码。")
                : tr("Proxy credentials are not saved. Source sign-in is always provided by the browser.",
                     "代理凭据未安全保存。网页登录授权由对应浏览器提供。"));
        busy(false);
    }
    void newProfile() {
        selected_.clear();
        list_->clearSelection();
        list_->setCurrentItem(nullptr);
        label_->clear();
        root_->setText(
            QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath("MaskPilot"));
        root_->setModified(false);
        proxy_->clear();
        username_->clear();
        password_->clear();
        proxyRequired_->setChecked(false);
        originRequired_->setChecked(false);
        storage_->setCurrentIndex(1);
        status_->setText(tr("New profile. Saving credentials is optional and off by default.",
                            "新环境默认不保存凭据；可按需选择 Windows 安全保存。"));
        busy(false);
        label_->setFocus();
    }
    void save(bool reactivate) {
        if (busy_)
            return;
        const auto* previous = current();
        ContextConfiguration draft;
        draft.id = selected_;
        draft.expectedRevision = previous ? previous->revision : 0;
        draft.externalKey = previous ? previous->externalKey : "desktop:" + newTaskId();
        draft.label = label_->text().trimmed().toStdString();
        draft.downloadRoot = std::filesystem::path(root_->text().trimmed().toStdWString());
        draft.proxyEndpoint = proxy_->text().trimmed().toStdString();
        draft.proxyAuthenticationRequired = proxyRequired_->isChecked();
        draft.originAuthenticationRequired = originRequired_->isChecked();
        draft.reactivate = reactivate;
        ProxyCredentials credentials{username_->text().toStdString(), password_->text().toStdString()};
        const auto choice = storage_->currentData().toString();
        const bool supplied = !credentials.username.empty() || !credentials.password.empty();
        const bool saved = previous && !previous->proxyCredentialRef.empty();
        if (choice == "session" && saved && !supplied && !reactivate) {
            failure(tr("Enter credentials for this session, or choose Keep current. Revoke the profile to "
                       "clear all its saved credentials.",
                       "仅当前会话使用时请填写凭据，或选择保留现有配置；撤销环境可清理其全部已保存凭据。"));
            return;
        }
        const bool remember = choice == "windows" || (choice == "keep" && saved && supplied);
        busy(true);
        status_->setText(tr("Saving…", "正在保存…"));
        ui::watchFuture(
            service_.configureContext(std::move(draft), std::move(credentials), remember), this,
            [this](DownloadContext value) {
                selected_ = value.id;
                username_->clear();
                password_->clear();
                load();
            },
            [this](QString error) { failure(error); });
    }

  public:
    ContextManagerDialog(DownloadService& service, bool chinese, QWidget* parent)
        : QDialog(parent), service_(service), chinese_(chinese) {
        setObjectName("contextManagerDialog");
        setWindowTitle(tr("Profiles & credentials", "环境与凭据"));
        resize(940, 620);
        auto* outer = new QVBoxLayout(this);
        auto* note =
            new QLabel(tr("Each profile owns its tasks and route. Accepted tasks keep their settings until "
                          "explicitly rebound. Revocation pauses work and clears saved credentials.",
                          "每个环境独立管理任务与代理。已接受任务保留原配置，需明确重新绑定才更新；撤销会暂停"
                          "任务并清理已保存凭据。"));
        note->setTextFormat(Qt::PlainText);
        note->setWordWrap(true);
        outer->addWidget(note);
        auto* body = new QHBoxLayout;
        outer->addLayout(body, 1);
        list_ = new QTreeWidget;
        list_->setObjectName("contextList");
        list_->setHeaderLabels({tr("Profile", "环境"), tr("State", "状态")});
        list_->setRootIsDecorated(false);
        list_->setMinimumWidth(260);
        body->addWidget(list_, 1);
        editor_ = new QWidget;
        auto* form = new QFormLayout(editor_);
        form->setSpacing(12);
        body->addWidget(editor_, 2);
        const auto line = [&](const char* objectName, const QString& caption) {
            auto* value = new QLineEdit;
            value->setObjectName(objectName);
            form->addRow(caption, value);
            return value;
        };
        label_ = line("contextLabel", tr("Name", "名称"));
        root_ = new QLineEdit;
        root_->setObjectName("contextDownloadRoot");
        auto* directoryRow = new QWidget;
        auto* directoryLayout = new QHBoxLayout(directoryRow);
        directoryLayout->setContentsMargins(0, 0, 0, 0);
        auto* browse = new QPushButton(tr("Browse…", "浏览…"));
        directoryLayout->addWidget(root_, 1);
        directoryLayout->addWidget(browse);
        form->addRow(tr("Download folder", "下载目录"), directoryRow);
        proxy_ = line("contextProxyEndpoint", tr("Proxy endpoint", "代理地址"));
        proxy_->setPlaceholderText(
            tr("Empty: direct; http:// or socks5://host:port", "留空直连；http:// 或 socks5://主机:端口"));
        username_ = line("contextProxyUsername", tr("Proxy username", "代理账号"));
        password_ = line("contextProxyPassword", tr("Proxy password", "代理密码"));
        password_->setEchoMode(QLineEdit::Password);
        proxyRequired_ = new QCheckBox(tr("Require proxy credentials", "代理必须提供凭据"));
        proxyRequired_->setObjectName("contextProxyAuthenticationRequired");
        form->addRow(proxyRequired_);
        originRequired_ =
            new QCheckBox(tr("Require browser sign-in authorization", "下载必须由浏览器提供登录授权"));
        originRequired_->setObjectName("contextOriginAuthenticationRequired");
        form->addRow(originRequired_);
        storage_ = new QComboBox;
        storage_->setObjectName("contextCredentialStorage");
        storage_->addItem(tr("Keep current", "保留现有配置"), "keep");
        storage_->addItem(tr("This session only", "仅当前会话"), "session");
        storage_->addItem(tr("Save securely in Windows", "由 Windows 安全保存"), "windows");
        form->addRow(tr("Proxy credentials", "代理凭据"), storage_);
        status_ = new QLabel;
        status_->setObjectName("contextStatus");
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        form->addRow(status_);
        auto* actions = new QHBoxLayout;
        outer->addLayout(actions);
        const auto button = [&](const char* name, const QString& caption) {
            auto* value = new QPushButton(caption);
            value->setObjectName(name);
            actions->addWidget(value);
            return value;
        };
        create_ = button("contextNew", tr("New profile", "新建环境"));
        save_ = button("contextSave", tr("Save", "保存"));
        revoke_ = button("contextRevoke", tr("Revoke / retry", "撤销 / 重试清理"));
        reactivate_ = button("contextReactivate", tr("Reactivate", "恢复环境"));
        cleanup_ = button("contextCleanupRetry", tr("Retry credential cleanup", "重试凭据清理"));
        auto* close = button("contextClose", tr("Close", "关闭"));
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
        connect(create_, &QPushButton::clicked, this, [this] { newProfile(); });
        connect(save_, &QPushButton::clicked, this, [this] { save(false); });
        connect(reactivate_, &QPushButton::clicked, this, [this] { save(true); });
        connect(list_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            if (item) {
                selected_ = item->data(0, Qt::UserRole).toString().toStdString();
                showCurrent();
            }
        });
        connect(proxy_, &QLineEdit::textEdited, this, [this] {
            username_->clear();
            password_->clear();
            storage_->setCurrentIndex(1);
            if (proxy_->text().trimmed().isEmpty())
                proxyRequired_->setChecked(false);
        });
        const auto markRequired = [this] {
            if (!username_->text().isEmpty() || !password_->text().isEmpty())
                proxyRequired_->setChecked(true);
        };
        connect(username_, &QLineEdit::textChanged, this, markRequired);
        connect(password_, &QLineEdit::textChanged, this, markRequired);
        connect(browse, &QPushButton::clicked, this, [this] {
            const auto directory =
                QFileDialog::getExistingDirectory(this, tr("Download folder", "下载目录"), root_->text());
            if (!directory.isEmpty()) {
                root_->setText(directory);
                root_->setModified(true);
            }
        });
        connect(revoke_, &QPushButton::clicked, this, [this] {
            if (!current() || busy_)
                return;
            busy(true);
            status_->setText(tr("Stopping this profile's work and clearing credentials…",
                                "正在停止此环境任务并清理凭据…"));
            ui::watchFuture(
                service_.revokeContext(selected_), this, [this] { load(); },
                [this](QString error) { load(error); });
        });
        connect(cleanup_, &QPushButton::clicked, this, [this] {
            busy(true);
            ui::watchFuture(
                service_.retryCredentialCleanup(), this,
                [this] {
                    status_->setText(tr("Credential cleanup finished.", "凭据清理完成。"));
                    busy(false);
                },
                [this](QString error) { failure(error); });
        });
        newProfile();
        load();
    }
    void reject() override {
        if (!busy_)
            QDialog::reject();
    }
};
} // namespace

void MainWindow::Impl::refreshContexts() {
    if (contextRequest && contextRequest->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            knownContexts = contextRequest->get();
            const auto selected = contextFilter->currentData().toString();
            const QSignalBlocker blocker(contextFilter);
            contextFilter->clear();
            contextFilter->addItem(t("All profiles", "全部环境"), QString{});
            contextFilter->addItem(t("Standalone", "独立下载区"), "standalone");
            model->contextLabels.clear();
            for (const auto& value : knownContexts) {
                model->contextLabels[value.id] = qs(value.label);
                contextFilter->addItem(qs(value.label) +
                                           (value.enabled ? QString{} : t(" (revoked)", "（已撤销）")),
                                       qs(value.id));
            }
            const auto index = contextFilter->findData(selected);
            contextFilter->setCurrentIndex(index >= 0 ? index : 0);
        } catch (const std::exception&) {
            notify("MaskPilot", t("Unable to refresh profile settings.", "无法刷新环境配置。"));
        }
        contextRequest.reset();
    }
    if (!contextRequest && unixTime() - lastContextRefresh >= 1) {
        contextRequest = service.contexts();
        lastContextRefresh = unixTime();
    }
}
void MainWindow::Impl::contextsDialog() {
    ContextManagerDialog dialog(service, chinese, window);
    dialog.exec();
    lastContextRefresh = 0;
    refresh();
}
void MainWindow::Impl::newContextDownload(const QString& initialUrl) {
    const auto owner = contextFilter->currentData().toString().toStdString();
    const auto found = std::find_if(knownContexts.begin(), knownContexts.end(),
                                    [&](const auto& value) { return value.id == owner; });
    if (found == knownContexts.end() || !found->enabled) {
        QMessageBox::information(
            window, "MaskPilot",
            t("Reactivate this profile before adding downloads.", "添加下载前请先恢复此环境。"));
        return;
    }
    if (found->originAuthenticationRequired) {
        QMessageBox::information(window, "MaskPilot",
                                 t("This profile requires browser sign-in authorization. Send the download "
                                   "through its trusted browser connection.",
                                   "此环境要求浏览器登录授权，请通过对应浏览器连接发送下载。"));
        return;
    }
    const auto profile = *found;
    SubmissionDialog dialog(window);
    dialog.setObjectName("contextDownloadDialog");
    dialog.setWindowTitle(t("New profile download", "添加环境下载"));
    dialog.resize(620, 300);
    auto* layout = new QVBoxLayout(&dialog);
    auto* profileHeading = new QLabel(qs(profile.label));
    profileHeading->setTextFormat(Qt::PlainText);
    layout->addWidget(profileHeading);
    auto* note =
        new QLabel(t("Web files and magnet links use this profile's fixed folder and proxy, in the default "
                     "queue. Pause a task before moving it to another queue.",
                     "网页文件与磁力任务使用此环境的固定目录、代理和默认队列；暂停任务后可移动到其他队列。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto* url = new QLineEdit(initialUrl);
    url->setObjectName("contextDownloadUrl");
    auto* filename = new QLineEdit(ui::defaultDownloadName(initialUrl));
    filename->setObjectName("contextDownloadFilename");
    auto* paused = new QCheckBox(t("Add paused", "添加后暂停"));
    paused->setObjectName("contextDownloadPaused");
    form->addRow(t("URL", "地址"), url);
    form->addRow(t("Filename", "文件名"), filename);
    auto* directory = new QLabel(pathString(profile.downloadRoot));
    directory->setTextFormat(Qt::PlainText);
    directory->setWordWrap(true);
    form->addRow(t("Folder", "目录"), directory);
    form->addRow(paused);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* confirm = buttons->addButton(t("Add download", "添加下载"), QDialogButtonBox::AcceptRole);
    confirm->setObjectName("contextDownloadConfirm");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, [&] { dialog.reject(); });
    QObject::connect(url, &QLineEdit::textEdited, &dialog,
                     [filename](const QString& value) { filename->setText(ui::defaultDownloadName(value)); });
    QObject::connect(confirm, &QPushButton::clicked, &dialog, [&] {
        if (dialog.submitted)
            return;
        dialog.submitted = true;
        buttons->setEnabled(false);
        ScopedDownloadDraft draft{url->text().trimmed().toStdString(),
                                  filename->text().trimmed().toStdString(),
                                  {},
                                  paused->isChecked()};
        auto fail = [&](QString error) {
            dialog.submitted = false;
            buttons->setEnabled(true);
            QMessageBox::warning(&dialog, "MaskPilot", error);
        };
        ui::watchFuture(
            service.issueClient(profile.id), &dialog,
            [&, draft, fail](std::string token) {
                ui::watchFuture(
                    service.addScoped(std::move(token), draft, newTaskId()), &dialog,
                    [&](std::string) {
                        window->cancelCompletionCountdown();
                        queueFilter->setCurrentIndex(queueFilter->findData(defaultQueueId));
                        dialog.accept();
                    },
                    fail);
            },
            fail);
    });
    dialog.exec();
}
} // namespace maskpilot
