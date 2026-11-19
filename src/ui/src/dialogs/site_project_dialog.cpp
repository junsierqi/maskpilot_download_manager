#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <maskpilot/application/site_project_service.h>
#include <maskpilot/ui/site_project_dialog.h>
#include <type_traits>
namespace maskpilot {
namespace {
QString text(const std::string& s) { return QString::fromUtf8(s); }
std::filesystem::path native(const QString& s) { return std::filesystem::path(s.toStdWString()); }
QString pathText(const std::filesystem::path& p) { return QString::fromStdWString(p.wstring()); }
struct Editor : QWidget {
    QLineEdit *name, *root, *output, *proxy, *user, *password;
    QComboBox* queue;
    QPushButton* browse;
    QSpinBox *depth, *pages, *resources, *pageBytes, *assetBytes, *stored, *exported;
    QPlainTextEdit *include, *exclude;
    QCheckBox *styles, *images, *fonts, *media, *required;
    explicit Editor(bool chinese, QWidget* parent = nullptr) : QWidget(parent) {
        auto t = [&](const char* en, const char* zh) { return QString::fromUtf8(chinese ? zh : en); };
        auto* form = new QFormLayout(this);
        auto line = [&](const QString& label, const char* object) {
            auto* w = new QLineEdit;
            w->setObjectName(object);
            form->addRow(label, w);
            return w;
        };
        name = line(t("Name", "名称"), "siteProjectName");
        root = line(t("Public root URL", "公开入口地址"), "siteProjectRootUrl");
        auto* outrow = new QWidget;
        auto* outlayout = new QHBoxLayout(outrow);
        outlayout->setContentsMargins(0, 0, 0, 0);
        output = new QLineEdit;
        output->setObjectName("siteProjectOutputDirectory");
        browse = new QPushButton(t("Browse…", "选择…"));
        outlayout->addWidget(output);
        outlayout->addWidget(browse);
        form->addRow(t("Export directory", "导出目录"), outrow);
        QObject::connect(browse, &QPushButton::clicked, this, [this] {
            auto selected = QFileDialog::getExistingDirectory(this, "MaskPilot");
            if (!selected.isEmpty())
                output->setText(selected);
        });
        queue = new QComboBox;
        queue->setObjectName("siteProjectQueue");
        form->addRow(t("Queue", "队列"), queue);
        auto spin = [&](const QString& label, const char* object, int low, int high, int value) {
            auto* w = new QSpinBox;
            w->setObjectName(object);
            w->setRange(low, high);
            w->setValue(value);
            form->addRow(label, w);
            return w;
        };
        depth = spin(t("Link depth", "链接深度"), "siteProjectDepth", 0, 8, 2);
        pages = spin(t("Maximum pages", "最大页面数"), "siteProjectPages", 1, 128, 32);
        resources = spin(t("Maximum resources", "最大资源数"), "siteProjectResources", 1, 1000, 256);
        pageBytes = spin(t("Page/CSS limit (bytes)", "页面／样式上限（字节）"), "siteProjectPageBytes", 1,
                         8 * 1024 * 1024, 2 * 1024 * 1024);
        assetBytes = spin(t("Asset limit (bytes)", "资源上限（字节）"), "siteProjectAssetBytes", 1,
                          8 * 1024 * 1024, 8 * 1024 * 1024);
        stored = spin(t("Revision body limit (bytes)", "本次原始内容上限（字节）"), "siteProjectStoredBytes",
                      1, 512 * 1024 * 1024, 128 * 1024 * 1024);
        exported = spin(t("Export limit (bytes)", "导出上限（字节）"), "siteProjectOutputBytes", 1,
                        512 * 1024 * 1024, 256 * 1024 * 1024);
        auto prefixes = [&](const QString& label, const char* object) {
            auto* w = new QPlainTextEdit;
            w->setObjectName(object);
            w->setMaximumHeight(55);
            form->addRow(label, w);
            return w;
        };
        include = prefixes(t("Include path prefixes (one per line)", "包含路径前缀（每行一条）"),
                           "siteProjectInclude");
        exclude = prefixes(t("Exclude path prefixes", "排除路径前缀"), "siteProjectExclude");
        auto* types = new QWidget;
        auto* typesLayout = new QHBoxLayout(types);
        typesLayout->setContentsMargins(0, 0, 0, 0);
        auto check = [&](const QString& label, const char* object, bool value) {
            auto* w = new QCheckBox(label);
            w->setObjectName(object);
            w->setChecked(value);
            typesLayout->addWidget(w);
            return w;
        };
        styles = check(t("CSS", "样式"), "siteProjectStyles", true);
        images = check(t("Images", "图片"), "siteProjectImages", true);
        fonts = check(t("Fonts", "字体"), "siteProjectFonts", true);
        media = check(t("Media", "媒体"), "siteProjectMedia", false);
        form->addRow(types);
        proxy = line(t("Proxy endpoint (empty = direct)", "代理地址（留空为直连）"), "siteProjectProxy");
        required = new QCheckBox(t("Proxy credentials required", "代理需要凭据"));
        required->setObjectName("siteProjectProxyRequired");
        form->addRow(required);
        user = line(t("Proxy username (this process only)", "代理用户名（仅当前进程）"),
                    "siteProjectProxyUsername");
        password = line(t("Proxy password (this process only)", "代理密码（仅当前进程）"),
                        "siteProjectProxyPassword");
        password->setEchoMode(QLineEdit::Password);
    }
    void queues(const std::vector<DownloadQueue>& values) {
        auto old = queue->currentData().toString();
        queue->clear();
        for (auto& q : values)
            queue->addItem(text(q.name), text(q.id));
        auto index = queue->findData(old);
        if (index >= 0)
            queue->setCurrentIndex(index);
    }
    void set(const SiteProjectOptions& o, bool immutable) {
        name->setText(text(o.name));
        root->setText(text(o.rootUrl));
        output->setText(pathText(o.outputDirectory));
        root->setReadOnly(immutable);
        output->setReadOnly(immutable);
        browse->setEnabled(!immutable);
        queue->setCurrentIndex(std::max(0, queue->findData(text(o.queueId))));
        depth->setValue(o.maxDepth);
        pages->setValue(o.maxPages);
        resources->setValue(o.maxResources);
        pageBytes->setValue(static_cast<int>(o.maxPageBytes));
        assetBytes->setValue(static_cast<int>(o.maxAssetBytes));
        stored->setValue(static_cast<int>(o.maxStoredBytes));
        exported->setValue(static_cast<int>(o.maxOutputBytes));
        QStringList a, b;
        for (auto& s : o.includePrefixes)
            a << text(s);
        for (auto& s : o.excludePrefixes)
            b << text(s);
        include->setPlainText(a.join('\n'));
        exclude->setPlainText(b.join('\n'));
        styles->setChecked(o.styles);
        images->setChecked(o.images);
        fonts->setChecked(o.fonts);
        media->setChecked(o.media);
        proxy->setText(text(o.proxy));
        required->setChecked(o.proxyAuthenticationRequired);
        user->clear();
        password->clear();
    }
    SiteProjectOptions get() const {
        SiteProjectOptions o;
        o.name = name->text().toUtf8().toStdString();
        o.rootUrl = root->text().toUtf8().toStdString();
        o.outputDirectory = native(output->text());
        o.queueId = queue->currentData().toString().toStdString();
        o.maxDepth = depth->value();
        o.maxPages = pages->value();
        o.maxResources = resources->value();
        o.maxPageBytes = pageBytes->value();
        o.maxAssetBytes = assetBytes->value();
        o.maxStoredBytes = stored->value();
        o.maxOutputBytes = exported->value();
        for (auto& s : include->toPlainText().split('\n', Qt::SkipEmptyParts))
            o.includePrefixes.push_back(s.trimmed().toUtf8().toStdString());
        for (auto& s : exclude->toPlainText().split('\n', Qt::SkipEmptyParts))
            o.excludePrefixes.push_back(s.trimmed().toUtf8().toStdString());
        o.styles = styles->isChecked();
        o.images = images->isChecked();
        o.fonts = fonts->isChecked();
        o.media = media->isChecked();
        o.proxy = proxy->text().toUtf8().toStdString();
        o.proxyAuthenticationRequired = required->isChecked();
        return o;
    }
    ProxyCredentials credentials() const {
        return {user->text().toUtf8().toStdString(), password->text().toUtf8().toStdString()};
    }
};
} // namespace
struct SiteProjectDialog::Impl {
    SiteProjectDialog* window;
    SiteProjectService& service;
    DownloadService& downloads;
    bool chinese;
    std::function<void(const std::filesystem::path&)> opener;
    QTableWidget *projects, *plan;
    QLabel *status, *summary;
    Editor* editor;
    QSpinBox *syncInterval, *syncRuns;
    QPushButton *syncEnable, *syncDisable;
    QTimer* timer;
    std::string selected;
    std::vector<DownloadQueue> policies{DownloadQueue{}};
    std::optional<std::future<std::vector<DownloadQueue>>> queueFuture;
    bool busy = false, loading = false;
    std::vector<QPushButton*> buttons;
    QString t(const char* en, const char* zh) const { return QString::fromUtf8(chinese ? zh : en); }
    Impl(SiteProjectDialog* w, SiteProjectService& s, DownloadService& d, bool zh,
         std::function<void(const std::filesystem::path&)> open)
        : window(w), service(s), downloads(d), chinese(zh), opener(std::move(open)) {
        if (!opener)
            opener = [](const auto& path) { QDesktopServices::openUrl(QUrl::fromLocalFile(pathText(path))); };
        window->setWindowTitle(t("Static site projects", "静态站点项目"));
        window->resize(1040, 760);
        auto* layout = new QVBoxLayout(window);
        auto* note =
            new QLabel(t("Public same-origin static pages only. Preview fetches HTML/CSS; Start confirms "
                         "binary assets. Updates create immutable revisions; earlier exports remain.",
                         "仅支持公开、同源的静态页面。预览下载页面和样式，开始后下载已识别资源。更新创建独立"
                         "版本，保留以前的导出。"));
        note->setWordWrap(true);
        layout->addWidget(note);
        projects = new QTableWidget(0, 5);
        projects->setObjectName("siteProjectList");
        projects->setHorizontalHeaderLabels({t("Project", "项目"), t("State", "状态"), t("Revision", "版本"),
                                             t("Progress", "进度"), t("Published", "已导出")});
        projects->setSelectionBehavior(QAbstractItemView::SelectRows);
        projects->setSelectionMode(QAbstractItemView::SingleSelection);
        projects->setEditTriggers(QAbstractItemView::NoEditTriggers);
        projects->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        projects->setMaximumHeight(170);
        layout->addWidget(projects);
        auto* split = new QSplitter;
        editor = new Editor(chinese);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(editor);
        split->addWidget(scroll);
        auto* right = new QWidget;
        auto* rightLayout = new QVBoxLayout(right);
        summary = new QLabel;
        summary->setObjectName("siteProjectSummary");
        summary->setWordWrap(true);
        rightLayout->addWidget(summary);
        plan = new QTableWidget(0, 4);
        plan->setObjectName("siteProjectPlan");
        plan->setHorizontalHeaderLabels({"URL", t("Kind", "类型"), t("State", "状态"), t("Bytes", "字节")});
        plan->setEditTriggers(QAbstractItemView::NoEditTriggers);
        plan->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        plan->horizontalHeader()->setStretchLastSection(true);
        rightLayout->addWidget(plan);
        split->addWidget(right);
        layout->addWidget(split, 1);
        auto* recurrence = new QHBoxLayout;
        auto* consent = new QLabel(
            t("Auto-sync full revisions (keeps older exports):", "自动同步完整版本（保留旧导出）："));
        consent->setWordWrap(true);
        recurrence->addWidget(consent);
        syncInterval = new QSpinBox;
        syncInterval->setObjectName("siteProjectSyncInterval");
        syncInterval->setRange(5, 10080);
        syncInterval->setValue(60);
        syncInterval->setSuffix(t(" min", " 分钟"));
        recurrence->addWidget(syncInterval);
        syncRuns = new QSpinBox;
        syncRuns->setObjectName("siteProjectSyncRuns");
        syncRuns->setRange(1, 100);
        syncRuns->setValue(10);
        syncRuns->setSuffix(t(" runs", " 次"));
        recurrence->addWidget(syncRuns);
        syncEnable = new QPushButton(t("Enable sync", "启用同步"));
        syncEnable->setObjectName("siteProjectSyncEnable");
        syncEnable->setToolTip(t("After the first completed export, automatically fetch pages and assets "
                                 "within the saved scope. Runs only while this app is open.",
                                 "首次导出成功后，按已保存的范围自动下载页面与资源。仅程序运行时执行。"));
        syncDisable = new QPushButton(t("Stop recurrence", "停止周期"));
        syncDisable->setObjectName("siteProjectSyncDisable");
        syncDisable->setToolTip(t("Keeps accepted work running; use Pause to stop it.",
                                  "已接受的本次工作继续；要停止下载请使用暂停。"));
        for (auto* button : {syncEnable, syncDisable}) {
            recurrence->addWidget(button);
            buttons.push_back(button);
        }
        QObject::connect(syncEnable, &QPushButton::clicked, window, [this] { command("sync"); });
        QObject::connect(syncDisable, &QPushButton::clicked, window, [this] { command("unsync"); });
        layout->addLayout(recurrence);
        status = new QLabel;
        status->setObjectName("siteProjectStatus");
        status->setTextFormat(Qt::PlainText);
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* actions = new QHBoxLayout;
        layout->addLayout(actions);
        auto action = [&](const char* label, const char* zh, const char* object, auto fn) {
            auto* b = new QPushButton(t(label, zh));
            b->setObjectName(object);
            actions->addWidget(b);
            buttons.push_back(b);
            QObject::connect(b, &QPushButton::clicked, window, fn);
        };
        action("New", "新建", "siteProjectNew", [this] { newProject(); });
        action("Preview", "预览", "siteProjectPreview", [this] { command("preview"); });
        action("Start", "开始", "siteProjectStart", [this] { command("start"); });
        action("Pause", "暂停", "siteProjectPause", [this] { command("pause"); });
        action("Resume", "继续", "siteProjectResume", [this] { command("resume"); });
        action("Cancel", "取消", "siteProjectCancel", [this] { command("cancel"); });
        action("Update", "更新", "siteProjectUpdate", [this] { command("update"); });
        action("Open", "打开", "siteProjectOpen", [this] { command("open"); });
        action("Remove", "移除历史", "siteProjectRemove", [this] { command("remove"); });
        QObject::connect(projects, &QTableWidget::itemSelectionChanged, window, [this] {
            if (loading)
                return;
            auto items = projects->selectedItems();
            selected =
                items.isEmpty() ? std::string{} : items.front()->data(Qt::UserRole).toString().toStdString();
            loadSelection();
        });
        timer = new QTimer(window);
        QObject::connect(timer, &QTimer::timeout, window, [this] { refresh(); });
        timer->start(200);
        editor->queues(policies);
        queueFuture = downloads.queues();
        refresh();
    }
    void fail(const std::exception& e) { status->setText(QString::fromUtf8(e.what())); }
    template <class T, class Done> void await(std::future<T> future, Done done) {
        busy = true;
        for (auto* b : buttons)
            b->setEnabled(false);
        auto shared = std::make_shared<std::future<T>>(std::move(future));
        auto* poll = new QTimer(window);
        QObject::connect(poll, &QTimer::timeout, window, [this, poll, shared, done] {
            if (shared->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            poll->stop();
            poll->deleteLater();
            try {
                if constexpr (std::is_void_v<T>) {
                    shared->get();
                    done();
                } else
                    done(shared->get());
                status->setText(t("Action accepted.", "操作已接受。"));
            } catch (const std::exception& e) {
                fail(e);
            }
            busy = false;
            for (auto* b : buttons)
                b->setEnabled(true);
            refresh();
        });
        poll->start(30);
    }
    void loadSelection() {
        if (auto value = service.inspect(selected)) {
            editor->set(value->options, true);
            if (value->syncIntervalMinutes != 0) {
                syncInterval->setValue(value->syncIntervalMinutes);
                syncRuns->setValue(std::max(1, value->syncRemainingRuns));
            }
        }
        refresh();
    }
    ProxyCredentials credentials() {
        auto result = editor->credentials();
        auto summaries = service.snapshot();
        auto found =
            std::find_if(summaries.begin(), summaries.end(), [&](const auto& s) { return s.id == selected; });
        if (found != summaries.end() && found->needsProxyCredentials &&
            (result.username.empty() || result.password.empty())) {
            QDialog dialog(window);
            dialog.setWindowTitle(t("Proxy credentials", "代理凭据"));
            auto* layout = new QFormLayout(&dialog);
            auto* user = new QLineEdit;
            user->setObjectName("siteProjectPromptUsername");
            auto* pass = new QLineEdit;
            pass->setObjectName("siteProjectPromptPassword");
            pass->setEchoMode(QLineEdit::Password);
            layout->addRow(t("Username", "用户名"), user);
            layout->addRow(t("Password", "密码"), pass);
            auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            layout->addRow(box);
            QObject::connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() != QDialog::Accepted)
                throw std::invalid_argument("Proxy credential entry cancelled.");
            result = {user->text().toUtf8().toStdString(), pass->text().toUtf8().toStdString()};
        }
        return result;
    }
    void newProject() {
        if (busy)
            return;
        auto* dialog = new QDialog(window);
        dialog->setObjectName("siteProjectCreateDialog");
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setWindowTitle(t("New static site project", "新建静态站点项目"));
        auto* layout = new QVBoxLayout(dialog);
        auto* fields = new Editor(chinese);
        fields->queues(policies);
        fields->set(SiteProjectOptions{}, false);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(fields);
        layout->addWidget(scroll);
        auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        box->button(QDialogButtonBox::Ok)->setObjectName("siteProjectCreate");
        layout->addWidget(box);
        QObject::connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        QObject::connect(box, &QDialogButtonBox::accepted, dialog, [this, dialog, fields] {
            try {
                auto options = fields->get();
                if (auto error = validateSiteProjectOptions(options); !error.empty())
                    throw std::invalid_argument(error);
                auto secret = fields->credentials();
                await(service.create(std::move(options), std::move(secret)), [this](std::string id) {
                    selected = std::move(id);
                    loadSelection();
                });
                dialog->accept();
            } catch (const std::exception& e) {
                fail(e);
            }
        });
        dialog->resize(600, 720);
        dialog->show();
    }
    void command(const std::string& action) {
        if (busy || selected.empty())
            return;
        auto detail = service.inspect(selected);
        if (!detail)
            return;
        try {
            const auto id = selected;
            const auto revision = detail->revision;
            if (action == "unsync") {
                await(service.setSynchronization(id, revision, 0), [] {});
                return;
            }
            if (action == "open") {
                await(service.openPath(id), [this](const auto& path) { opener(path); });
                return;
            }
            if (action == "remove") {
                await(service.removeHistory(id, revision), [this] {
                    const auto remaining = service.snapshot();
                    selected = remaining.empty() ? std::string{} : remaining.front().id;
                    loadSelection();
                });
                return;
            }
            if (action == "pause") {
                await(service.pause(id, revision), [] {});
                return;
            }
            if (action == "cancel") {
                await(service.cancel(id, revision), [] {});
                return;
            }
            auto secret = credentials();
            editor->password->clear();
            editor->user->clear();
            if (action == "preview")
                await(service.preview(id, revision, std::move(secret)), [] {});
            else if (action == "start")
                await(service.start(id, revision, std::move(secret)), [] {});
            else if (action == "resume")
                await(service.resume(id, revision, std::move(secret)), [] {});
            else if (action == "sync")
                await(service.setSynchronization(id, revision, syncInterval->value(), syncRuns->value(),
                                                 std::move(secret)),
                      [] {});
            else if (action == "update") {
                auto options = editor->get();
                options.rootUrl = detail->options.rootUrl;
                options.outputDirectory = detail->options.outputDirectory;
                await(service.update(id, revision, std::move(options), std::move(secret)),
                      [this] { loadSelection(); });
            }
        } catch (const std::exception& e) {
            fail(e);
        }
    }
    void refresh() {
        if (queueFuture && queueFuture->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                policies = queueFuture->get();
                editor->queues(policies);
            } catch (const std::exception& e) {
                fail(e);
            }
            queueFuture.reset();
        }
        auto values = service.snapshot();
        loading = true;
        projects->clearSelection();
        projects->setRowCount(static_cast<int>(values.size()));
        for (int row = 0; row < static_cast<int>(values.size()); ++row) {
            const auto& s = values[row];
            QStringList labels{text(s.name), QString::fromUtf8(siteProjectStateName(s.state)),
                               QString::number(s.revision),
                               QString("%1 / %2").arg(s.ready).arg(s.documents + s.resources),
                               QString::number(s.publishedRevision)};
            for (int col = 0; col < labels.size(); ++col) {
                auto* item = new QTableWidgetItem(labels[col]);
                item->setData(Qt::UserRole, text(s.id));
                projects->setItem(row, col, item);
            }
            if (s.id == selected)
                projects->selectRow(row);
        }
        loading = false;
        const auto detail = service.inspect(selected);
        if (detail) {
            editor->setEnabled(!busy);
            summary->setText(t("Revision %1 · published %2 · %3 entries · excluded %4",
                               "版本 %1 · 已导出 %2 · %3 条目 · 已排除 %4")
                                 .arg(detail->revision)
                                 .arg(detail->published ? detail->published->revision : 0)
                                 .arg(detail->entries.size())
                                 .arg(detail->excludedCount));
            if (detail->syncIntervalMinutes != 0)
                summary->setText(
                    summary->text() +
                    t(" · sync %1 min · %2 runs left · next %3", " · 同步间隔 %1 分钟 · 剩余 %2 次 · 下次 %3")
                        .arg(detail->syncIntervalMinutes)
                        .arg(detail->syncRemainingRuns)
                        .arg(detail->syncNextAt
                                 ? QDateTime::fromSecsSinceEpoch(detail->syncNextAt, QTimeZone::UTC)
                                       .toString(Qt::ISODate)
                                 : t("after this revision", "本版本完成后")));
            plan->setRowCount(static_cast<int>(detail->entries.size()));
            for (int i = 0; i < static_cast<int>(detail->entries.size()); ++i) {
                auto& e = detail->entries[i];
                QStringList labels{text(e.url), QString::number(static_cast<int>(e.kind)),
                                   QString::fromUtf8(siteProjectEntryStateName(e.state)),
                                   QString::number(e.bytes)};
                for (int j = 0; j < labels.size(); ++j)
                    plan->setItem(i, j, new QTableWidgetItem(labels[j]));
            }
            if (!detail->error.empty())
                status->setText(text(detail->error));
        } else {
            editor->setEnabled(false);
            plan->setRowCount(0);
            summary->clear();
        }
        for (auto* button : buttons)
            button->setEnabled(!busy && (button->objectName() == "siteProjectNew" || detail.has_value()));
        syncEnable->setEnabled(!busy && detail && detail->state == SiteProjectState::Completed);
        syncDisable->setEnabled(!busy && detail && detail->syncIntervalMinutes != 0);
        if (!service.startupError().empty())
            status->setText(text(service.startupError()));
    }
};
SiteProjectDialog::SiteProjectDialog(SiteProjectService& s, DownloadService& d, bool zh,
                                     std::function<void(const std::filesystem::path&)> open, QWidget* p)
    : QDialog(p), impl_(std::make_unique<Impl>(this, s, d, zh, std::move(open))) {}
SiteProjectDialog::~SiteProjectDialog() = default;
} // namespace maskpilot
