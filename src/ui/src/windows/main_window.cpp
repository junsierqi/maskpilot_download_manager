#include "../dialogs/async_ui.h"
#include "main_window_p.h"
#include <QResizeEvent>
#include <maskpilot/ui/site_project_dialog.h>
namespace maskpilot {
namespace {
class TaskFilterBar final : public QWidget {
    QBoxLayout* layout_;
    QLineEdit* search_;

  public:
    TaskFilterBar(QLineEdit* search, QComboBox* profiles, QComboBox* queues) : search_(search) {
        setObjectName("taskFilterBar");
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        auto* group = new QWidget;
        auto* filters = new QHBoxLayout(group);
        filters->setContentsMargins(0, 0, 0, 0);
        filters->setSpacing(10);
        filters->addWidget(profiles);
        filters->addWidget(queues);
        layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(12);
        layout_->addWidget(search, 1);
        layout_->addWidget(group, 0, Qt::AlignRight);
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        const bool compact = width() < 720;
        layout_->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        search_->setMinimumWidth(compact ? std::min(180, width()) : 180);
    }
};

class TaskActionBar final : public QToolBar {
  protected:
    void resizeEvent(QResizeEvent* event) override {
        QToolBar::resizeEvent(event);
        // Keep every action available in narrow windows; QAction text remains its tooltip.
        setToolButtonStyle(width() < 720 ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    }
};
} // namespace
MainWindow::Impl::Impl(MainWindow* w, DownloadService& s)
    : window(w), service(s), chinese(settings.value("language", "en").toString() == "zh") {}

QString MainWindow::Impl::t(const char* en, const char* zh) const {
    return QString::fromUtf8(chinese ? zh : en);
}

std::vector<Task> MainWindow::Impl::selected() const {
    std::vector<Task> result;
    for (const auto& index : table->selectionModel()->selectedRows())
        if (index.row() < static_cast<int>(model->rows.size()))
            result.push_back(model->rows[static_cast<size_t>(index.row())]);
    return result;
}

void MainWindow::Impl::notify(const QString& title, const QString& text) {
    window->statusBar()->showMessage(text, 8000);
    if (tray && tray->isVisible())
        tray->showMessage(title, text, QSystemTrayIcon::Information, 4000);
}

void MainWindow::Impl::updateActions() {
    auto tasks = selected();
    bool any = !tasks.empty();
    resumeAction->setEnabled(std::any_of(tasks.begin(), tasks.end(), [](const auto& task) {
        return !isActive(task.state) && !isTerminal(task.state);
    }));
    pauseAction->setEnabled(std::any_of(tasks.begin(), tasks.end(), [](const auto& task) {
        return isActive(task.state) || task.state == TaskState::Queued ||
               task.state == TaskState::Scheduled || task.state == TaskState::RetryWaiting ||
               task.state == TaskState::PolicyWaiting;
    }));
    cancelAction->setEnabled(
        std::any_of(tasks.begin(), tasks.end(), [](const auto& task) { return !isTerminal(task.state); }));
    removeAction->setEnabled(any && std::none_of(tasks.begin(), tasks.end(),
                                                 [](const auto& task) { return isActive(task.state); }));
    detailAction->setEnabled(tasks.size() == 1);
}

void MainWindow::Impl::updateColumns() {
    const auto width = table->viewport()->width();
    const bool hasTorrent = std::any_of(
        all.begin(), all.end(), [](const auto& task) { return task.spec.kind == TransferKind::BitTorrent; });
    auto* header = table->horizontalHeader();
    const QSignalBlocker block(header);
    for (int column = 1; column < model->columnCount(); ++column) {
        const auto key = QStringLiteral("taskColumns/visible%1").arg(column);
        bool visible = column == 7                  ? hasTorrent && width >= 740
                       : column == 5 || column == 6 ? width >= 1000
                       : column == 1                ? width >= 740
                                                    : true;
        if (settings.contains(key))
            visible = settings.value(key).toBool();
        table->setColumnHidden(column, !visible);
    }
    int used = 0;
    for (int column = 1; column < model->columnCount(); ++column)
        if (!table->isColumnHidden(column))
            used += table->columnWidth(column);
    // Keep filenames readable; explicit extra columns may use horizontal scrolling.
    table->setColumnWidth(
        0, std::clamp(settings.value("taskColumns/nameWidth", width - used).toInt(), 220, 2400));
}

void MainWindow::Impl::columnMenu(const QPoint& point) {
    QMenu menu(window);
    for (int column = 1; column < model->columnCount(); ++column) {
        auto* action = menu.addAction(model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
        action->setCheckable(true);
        action->setChecked(!table->isColumnHidden(column));
        QObject::connect(action, &QAction::toggled, window, [this, column](bool visible) {
            settings.setValue(QStringLiteral("taskColumns/visible%1").arg(column), visible);
            updateColumns();
        });
    }
    menu.addSeparator();
    menu.addAction(t("Automatic column layout", "恢复自动列布局"), window, [this] {
        settings.remove("taskColumns");
        const QSignalBlocker block(table->horizontalHeader());
        const int widths[] = {0, 80, 150, 90, 132, 96, 85, 90};
        for (int column = 1; column < model->columnCount(); ++column)
            table->setColumnWidth(column, widths[column]);
        updateColumns();
    });
    menu.exec(table->horizontalHeader()->mapToGlobal(point));
}

void MainWindow::Impl::refresh() {
    if (refreshing)
        return;
    QScopedValueRollback<bool> refreshGuard(refreshing, true);
    refreshContexts();
    refreshQueues();
    std::vector<Pending> ready;
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        ready.push_back(std::move(*it));
        it = pending.erase(it);
    }
    for (auto& item : ready) {
        QString id, error;
        try {
            id = qs(item.result.get());
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        if (item.completion)
            item.completion(id, error);
        else if (!error.isEmpty())
            QMessageBox::warning(window, t("Unable to add download", "无法添加下载"), error);
    }
    const auto previous = selected();
    all = service.snapshot();
    std::set<std::string> existingIds;
    for (const auto& task : all)
        existingIds.insert(task.id);
    std::erase_if(seenCompleted, [&](const auto& id) { return !existingIds.contains(id); });
    std::vector<Task> visible;
    const auto filter =
        sidebar->currentItem() ? sidebar->currentItem()->data(Qt::UserRole).toString() : "all";
    auto query = search->text().trimmed();
    int active = 0, complete = 0;
    std::int64_t speed = 0, upload = 0;
    bool busy = false;
    const auto projectRows = projects ? projects->snapshot() : std::vector<SiteProjectSummary>{};
    for (const auto& project : projectRows)
        if (!siteProjectTerminal(project.state))
            projectBatchIds.insert(project.id);
    busy = projectsUnavailable || (projects && projects->hasUnfinished());
    for (const auto& task : all) {
        if (isActive(task.state)) {
            ++active;
            speed += task.bytesPerSecond;
            upload += task.uploadSpeed;
        }
        if (task.state == TaskState::Completed)
            ++complete;
        if (task.state == TaskState::Completed && seenCompleted.insert(task.id).second && completionPrimed) {
            batchIds.insert(task.id);
            wasBusy = true;
        }
        busy = busy || isActive(task.state) || task.state == TaskState::Queued ||
               task.state == TaskState::Scheduled || task.state == TaskState::RetryWaiting ||
               task.state == TaskState::AwaitingSelection || task.state == TaskState::PolicyWaiting;
        if (isActive(task.state) || task.state == TaskState::Queued || task.state == TaskState::Scheduled ||
            task.state == TaskState::RetryWaiting || task.state == TaskState::AwaitingSelection ||
            task.state == TaskState::PolicyWaiting)
            batchIds.insert(task.id);
        bool match = filter == "all" ||
                     (filter == "active" && (isActive(task.state) || task.state == TaskState::Queued ||
                                             task.state == TaskState::RetryWaiting)) ||
                     (filter == "paused" &&
                      (task.state == TaskState::Paused || task.state == TaskState::PolicyWaiting)) ||
                     (filter == "scheduled" && task.state == TaskState::Scheduled) ||
                     (filter == "completed" && task.state == TaskState::Completed) ||
                     (filter == "attention" &&
                      (task.state == TaskState::Failed || task.state == TaskState::NeedsAttention ||
                       task.state == TaskState::AwaitingSelection)) ||
                     (filter == "torrent" && task.spec.kind == TransferKind::BitTorrent) ||
                     (filter == "ed2k" && task.spec.kind == TransferKind::Ed2k) ||
                     (filter == "seeding" && task.state == TaskState::Seeding) ||
                     (filter.startsWith("category:") && qs(task.spec.category) == filter.mid(9));
        const auto owner = contextFilter ? contextFilter->currentData().toString() : QString{};
        match = match && (owner.isEmpty() || owner.toStdString() == task.ownerId);
        const auto queue = queueFilter ? queueFilter->currentData().toString() : QString{};
        match = match && (queue.isEmpty() || queue.toStdString() == task.queueId);
        if (match && (query.isEmpty() ||
                      pathString(task.spec.destination.filename()).contains(query, Qt::CaseInsensitive) ||
                      qs(task.torrent.name).contains(query, Qt::CaseInsensitive) ||
                      qs(task.spec.category).contains(query, Qt::CaseInsensitive)))
            visible.push_back(task);
    }
    const auto scroll = table->verticalScrollBar()->value();
    completionPrimed = true;
    model->setRows(std::move(visible));
    for (int row = 0; row < model->rowCount(); ++row)
        if (std::any_of(previous.begin(), previous.end(), [&](const auto& task) {
                return task.id == model->rows[static_cast<size_t>(row)].id;
            }))
            table->selectionModel()->select(model->index(row, 0),
                                            QItemSelectionModel::Select | QItemSelectionModel::Rows);
    table->verticalScrollBar()->setValue(scroll);
    empty->setVisible(model->rows.empty());
    table->setVisible(!model->rows.empty());
    updateColumns();
    heading->setText(sidebar->currentItem() ? sidebar->currentItem()->text()
                                            : t("All downloads", "全部下载"));
    summary->setText(
        t("%1 shown / %2 total · %3 active · %4 completed", "显示 %1 / 共 %2 项 · %3 项活跃 · %4 项完成")
            .arg(model->rows.size())
            .arg(all.size())
            .arg(active)
            .arg(complete));
    throughput->setText(t("↓ Download  ", "↓ 下载  ") + bytes(speed) + "/s    " +
                        t("↑ Upload  ", "↑ 上传  ") + bytes(upload) + "/s");
    updateActions();
    const auto storageError = qs(service.lastError());
    if (!storageError.isEmpty()) {
        timer->stop();
        QMessageBox::critical(window, t("Storage error", "存储错误"), storageError);
    }
    if (completionDialog && (busy || !pending.empty()))
        completionDialog->reject();
    if (wasBusy && !busy && (!batchIds.empty() || !projectBatchIds.empty())) {
        auto stateFor = [&](const std::string& id) {
            auto it = std::find_if(all.begin(), all.end(), [&](const auto& task) { return task.id == id; });
            return it == all.end() ? TaskState::Cancelled : it->state;
        };
        const bool projectSuccess =
            std::all_of(projectBatchIds.begin(), projectBatchIds.end(), [&](const auto& id) {
                return std::any_of(projectRows.begin(), projectRows.end(), [&](const auto& project) {
                    return project.id == id && project.state == SiteProjectState::Completed;
                });
            });
        const bool successful =
            projectSuccess && std::all_of(batchIds.begin(), batchIds.end(), [&](const auto& id) {
                return stateFor(id) == TaskState::Completed;
            });
        if (successful)
            completion();
        if (std::all_of(batchIds.begin(), batchIds.end(),
                        [&](const auto& id) { return isTerminal(stateFor(id)); }))
            batchIds.clear();
        projectBatchIds.clear();
    }
    wasBusy = busy;
}

void MainWindow::Impl::completion() {
    const auto current = service.snapshot();
    if (projectsUnavailable || (projects && projects->hasUnfinished()) || !pending.empty() ||
        std::any_of(current.begin(), current.end(), [](const auto& task) { return !isTerminal(task.state); }))
        return;
    notify("MaskPilot", t("All downloads are complete.", "所有下载任务已完成。"));
    const auto action = settings.value("completionAction", 0).toInt();
    if (action != 1 && action != 2)
        return;
    auto* dialog = new QDialog(window);
    completionDialog = dialog;
    dialog->setObjectName("completionCountdown");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(t("Queue complete", "队列完成"));
    auto* layout = new QVBoxLayout(dialog);
    auto* label = new QLabel;
    layout->addWidget(label);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    auto remaining = std::make_shared<int>(60);
    auto* countdown = new QTimer(dialog);
    QObject::connect(countdown, &QTimer::timeout, dialog, [this, dialog, label, remaining, action] {
        if (!dialog->isVisible())
            return;
        const auto current = service.snapshot();
        if (projectsUnavailable || (projects && projects->hasUnfinished()) || !pending.empty() ||
            std::any_of(current.begin(), current.end(),
                        [](const auto& task) { return !isTerminal(task.state); })) {
            dialog->reject();
            return;
        }
        label->setText(
            t("%1 in %2 seconds. Cancel to keep downloading.", "将在 %2 秒后%1。可取消此操作。")
                .arg(action == 1 ? t("Exit MaskPilot", "退出程序") : t("Shut down computer", "关闭计算机"))
                .arg(*remaining));
        if (--*remaining < 0) {
            dialog->accept();
            if (action == 1)
                window->requestQuit();
            else {
#ifdef _WIN32
                QProcess::startDetached(systemExecutable(L"shutdown.exe"), {"/s", "/t", "0"});
#else
                QProcess::startDetached("/usr/bin/systemctl", {"poweroff"});
#endif
            }
        }
    });
    countdown->start(1000);
    dialog->show();
}

MainWindow::MainWindow(DownloadService& service, QWidget* parent)
    : QMainWindow(parent), impl_(std::make_unique<Impl>(this, service)) {
    auto& p = *impl_;
    setWindowTitle("MaskPilot Download Manager");
    setWindowIcon(QIcon(":/icons/app.svg"));
    resize(1180, 750);
    setMinimumSize(850, 560);
    setAcceptDrops(true);
    setStyleSheet(ui::styleSheet());
    auto* container = new QWidget;
    auto* outer = new QVBoxLayout(container);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    setCentralWidget(container);
    p.search = new QLineEdit;
    p.search->setObjectName("taskSearch");
    p.search->setPlaceholderText(p.t("Search downloads…", "搜索下载任务…"));
    p.search->setAccessibleName(p.t("Search downloads", "搜索下载任务"));
    p.search->setClearButtonEnabled(true);
    p.search->setMinimumWidth(180);
    p.contextFilter = new QComboBox;
    p.contextFilter->setObjectName("contextFilter");
    p.contextFilter->setMinimumWidth(140);
    p.contextFilter->setMaximumWidth(220);
    p.contextFilter->setAccessibleName(p.t("Download profile", "下载环境"));
    p.contextFilter->addItem(p.t("All profiles", "全部环境"), QString{});
    p.contextFilter->addItem(p.t("Standalone", "独立下载区"), "standalone");
    auto* newButton = new QPushButton(p.t("＋  New download", "＋  新建下载"));
    newButton->setDefault(true);
    newButton->setObjectName("newDownloadButton");
    auto* toolbar = new TaskActionBar;
    toolbar->setIconSize(QSize(18, 18));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->setObjectName("taskToolbar");
    toolbar->addWidget(newButton);
    toolbar->addSeparator();
    auto addAction = [&](const QString& text, ui::Icon icon) {
        return toolbar->addAction(ui::icon(icon), text);
    };
    p.resumeAction = addAction(p.t("Start", "开始"), ui::Icon::Start);
    p.resumeAction->setObjectName("resumeTasksAction");
    p.pauseAction = addAction(p.t("Pause", "暂停"), ui::Icon::Pause);
    p.pauseAction->setObjectName("pauseTasksAction");
    p.cancelAction = addAction(p.t("Cancel", "取消"), ui::Icon::Cancel);
    toolbar->addSeparator();
    p.removeAction = addAction(p.t("Remove", "移除"), ui::Icon::Remove);
    p.detailAction = addAction(p.t("Details", "详情"), ui::Icon::Details);
    auto* batch = new QAction(ui::icon(ui::Icon::Batch), p.t("Batch downloads…", "批量下载…"), this);
    auto* settings = new QAction(ui::icon(ui::Icon::Settings), p.t("Settings…", "设置…"), this);
    settings->setObjectName("globalSettingsAction");
    auto* contexts =
        new QAction(ui::icon(ui::Icon::Settings), p.t("Profiles & credentials…", "环境与凭据…"), this);
    contexts->setToolTip(p.t("Manage profiles and saved proxy credentials", "管理环境与安全保存的代理凭据"));
    contexts->setObjectName("manageContextsAction");
    connect(contexts, &QAction::triggered, this, [&p] { p.contextsDialog(); });
    auto* queues = new QAction(ui::icon(ui::Icon::Batch), p.t("Download queues…", "下载队列…"), this);
    queues->setObjectName("manageQueuesAction");
    connect(queues, &QAction::triggered, this, [&p] { p.queuesDialog(); });
    connect(p.contextFilter, &QComboBox::currentIndexChanged, this, [&p] { p.refresh(); });
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    outer->addLayout(body, 1);
    p.sidebar = new QListWidget;
    p.sidebar->setObjectName("downloadSidebar");
    p.sidebar->setFixedWidth(192);
    p.sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    p.sidebar->setTextElideMode(Qt::ElideRight);
    p.sidebar->setResizeMode(QListView::Adjust);
    p.sidebar->setIconSize(QSize(18, 18));
    body->addWidget(p.sidebar);
    auto addGroup = [&](const QString& text) {
        auto* item = new QListWidgetItem(text, p.sidebar);
        item->setFlags(Qt::NoItemFlags);
        item->setSizeHint(QSize(0, 30));
        item->setForeground(QColor("#64748b"));
        auto font = item->font();
        font.setPointSize(9);
        font.setBold(true);
        item->setFont(font);
    };
    auto addFilter = [&](QString label, QString key, ui::Icon icon = ui::Icon::Folder) {
        auto* item = new QListWidgetItem(ui::icon(icon), label, p.sidebar);
        item->setToolTip(label);
        item->setData(Qt::UserRole, key);
        item->setSizeHint(QSize(0, 36));
    };
    addFilter(p.t("All downloads", "全部下载"), "all", ui::Icon::Download);
    addGroup(p.t("TASK STATUS", "任务状态"));
    addFilter(p.t("Active", "进行中"), "active", ui::Icon::Start);
    addFilter(p.t("Paused", "已暂停"), "paused", ui::Icon::Pause);
    addFilter(p.t("Scheduled", "计划任务"), "scheduled", ui::Icon::Clock);
    addFilter(p.t("Completed", "已完成"), "completed", ui::Icon::Check);
    addFilter(p.t("Needs attention", "需要处理"), "attention", ui::Icon::Alert);
    addGroup(p.t("BITTORRENT", "种子传输"));
    addFilter(p.t("BT / Magnet", "BT / 磁力"), "torrent", ui::Icon::Torrent);
    addFilter("ED2K", "ed2k", ui::Icon::Download);
    addFilter(p.t("Seeding", "正在做种"), "seeding", ui::Icon::Upload);
    addGroup(p.t("FILE TYPES", "文件分类"));
    for (const auto& [key, label] : {std::pair{"Documents", p.t("Documents", "文档")},
                                     {"Software", p.t("Software", "软件")},
                                     {"Video", p.t("Video", "视频")},
                                     {"Music", p.t("Music", "音乐")},
                                     {"Archives", p.t("Archives", "压缩包")}})
        addFilter(label, "category:" + QString(key));
    p.sidebar->setCurrentRow(0);
    auto* content = new QWidget;
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(20, 18, 20, 18);
    contentLayout->setSpacing(12);
    body->addWidget(content, 1);
    auto* section = new QHBoxLayout;
    p.heading = new QLabel;
    p.heading->setObjectName("sectionTitle");
    p.heading->setTextFormat(Qt::PlainText);
    section->addWidget(p.heading);
    p.queueFilter = new QComboBox;
    p.queueFilter->setObjectName("queueFilter");
    p.queueFilter->setMinimumWidth(130);
    p.queueFilter->setMaximumWidth(220);
    p.queueFilter->setAccessibleName(p.t("Download queue", "下载队列"));
    p.queueFilter->addItem(p.t("All queues", "全部队列"), QString{});
    p.queueFilter->addItem(p.t("Default", "默认队列"), defaultQueueId);
    connect(p.queueFilter, &QComboBox::currentIndexChanged, this, [&p] { p.refresh(); });
    section->addStretch();
    p.summary = new QLabel;
    p.summary->setObjectName("taskSummary");
    section->addWidget(p.summary);
    contentLayout->addLayout(section);
    contentLayout->addWidget(new TaskFilterBar(p.search, p.contextFilter, p.queueFilter));
    contentLayout->addWidget(toolbar);
    p.model = new TaskModel(this);
    p.model->chinese = p.chinese;
    p.table = new QTableView;
    p.table->setObjectName("downloadTable");
    p.table->setModel(p.model);
    p.table->setItemDelegate(new TaskDelegate(p.table));
    p.table->setWordWrap(false);
    p.table->setSelectionBehavior(QAbstractItemView::SelectRows);
    p.table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    p.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    p.table->setShowGrid(false);
    p.table->verticalHeader()->hide();
    p.table->verticalHeader()->setDefaultSectionSize(52);
    p.table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    p.table->horizontalHeader()->setMinimumSectionSize(60);
    p.table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    p.table->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    p.table->horizontalHeader()->setToolTip(p.t("Right-click to choose columns; drag borders to resize.",
                                                "右键选择显示列，拖动列边界调整宽度。"));
    connect(p.table->horizontalHeader(), &QHeaderView::customContextMenuRequested, this,
            [&p](const QPoint& point) { p.columnMenu(point); });
    const int widths[] = {0, 80, 150, 90, 132, 96, 85, 90};
    for (int col = 1; col < p.model->columnCount(); ++col)
        p.table->setColumnWidth(
            col,
            std::clamp(p.settings.value(QStringLiteral("taskColumns/width%1").arg(col), widths[col]).toInt(),
                       60, 1200));
    connect(p.table->horizontalHeader(), &QHeaderView::sectionResized, this,
            [&p](int column, int, int width) {
                if (column == 0)
                    p.settings.setValue("taskColumns/nameWidth", std::clamp(width, 220, 2400));
                else
                    p.settings.setValue(QStringLiteral("taskColumns/width%1").arg(column), width);
                p.updateColumns();
            });
    p.table->setContextMenuPolicy(Qt::CustomContextMenu);
    contentLayout->addWidget(p.table, 1);
    p.empty = new QLabel;
    p.empty->setObjectName("emptyStateFrame");
    auto* emptyLayout = new QVBoxLayout(p.empty);
    emptyLayout->setContentsMargins(32, 32, 32, 32);
    emptyLayout->setSpacing(14);
    emptyLayout->addStretch();
    auto* emptyIcon = new QLabel;
    emptyIcon->setPixmap(ui::icon(ui::Icon::Download).pixmap(48, 48));
    emptyLayout->addWidget(emptyIcon, 0, Qt::AlignHCenter);
    auto* emptyHeading = new QLabel(p.t("Ready for your next download", "准备好开始下一次下载"));
    emptyHeading->setObjectName("emptyHeading");
    emptyLayout->addWidget(emptyHeading, 0, Qt::AlignHCenter);
    auto* emptyHint =
        new QLabel(p.t("Add a link or torrent, paste a batch,\nor drop a download address here.",
                       "添加链接或种子、批量导入，\n或将下载地址拖到这里。"));
    emptyHint->setObjectName("emptyHint");
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setWordWrap(true);
    emptyHint->setMaximumWidth(450);
    emptyHint->setMinimumHeight(44);
    emptyLayout->addWidget(emptyHint, 0, Qt::AlignHCenter);
    auto* emptyAdd = new QPushButton(p.t("＋  New download", "＋  新建下载"));
    emptyAdd->setObjectName("emptyStateAddButton");
    emptyAdd->setProperty("primary", true);
    emptyLayout->addWidget(emptyAdd, 0, Qt::AlignHCenter);
    connect(emptyAdd, &QPushButton::clicked, newButton, &QPushButton::click);
    emptyLayout->addStretch();
    contentLayout->addWidget(p.empty, 1);
    p.throughput = new QLabel;
    p.throughput->setObjectName("throughput");
    statusBar()->addPermanentWidget(p.throughput);
    auto* file = menuBar()->addMenu(p.t("File", "文件"));
    auto* add = file->addAction(p.t("New download…", "新建下载…"));
    add->setShortcut(QKeySequence("Ctrl+N"));
    connect(add, &QAction::triggered, this, [this] { newDownload(); });
    file->addAction(p.t("Import tasks…", "导入任务…"), this, [&p] { p.importTasks(); });
    file->addAction(p.t("Export tasks…", "导出任务…"), this, [&p] { p.exportTasks(); });
    file->addSeparator();
    file->addAction(p.t("Quit", "退出"), QKeySequence("Ctrl+Q"), this, &MainWindow::requestQuit);
    auto* tools = menuBar()->addMenu(p.t("Tools", "工具"));
    tools->addAction(batch);
    tools->addAction(queues);
    tools->addAction(contexts);
    tools->addSeparator();
    p.projectsAction = tools->addAction(p.t("Static site projects…", "静态站点项目…"), this, [this, &p] {
        if (!p.projects)
            return;
        cancelCompletionCountdown();
        SiteProjectDialog dialog(*p.projects, p.service, p.chinese, p.projectOpener, this);
        dialog.exec();
    });
    p.projectsAction->setObjectName("siteProjectsAction");
    p.projectsAction->setEnabled(false);
    tools->addAction(p.t("Open torrent…", "打开种子文件…"), this, [this, &p] {
        const auto file =
            QFileDialog::getOpenFileName(this, p.t("Open torrent", "打开种子"), {}, "Torrent (*.torrent)");
        if (!file.isEmpty())
            newDownload("torrent:" + QDir::fromNativeSeparators(file));
    });
    tools->addAction(p.t("Add magnet link…", "添加磁力链接…"), this, [this] { newDownload("magnet:?"); });
    tools->addAction(p.t("New standalone media download…", "新建独立媒体下载…"), this, [this, &p] {
        AddDownloadDialog dialog(p.chinese, this);
        dialog.setMediaInspector(p.mediaInspector);
        auto spec = defaultSpec("https://");
        spec.kind = maskpilot::TransferKind::Media;
        spec.destination.replace_extension(L".mkv");
        dialog.setSpecification(spec);
        dialog.setQueues(p.knownQueues, p.selectedQueueId());
        if (dialog.exec() == QDialog::Accepted) {
            p.contextFilter->setCurrentIndex(p.contextFilter->findData("standalone"));
            p.queueFilter->setCurrentIndex(p.queueFilter->findData(qs(dialog.queueId())));
            enqueue(dialog.specification(), {}, {}, dialog.rememberProxyCredentials(), dialog.queueId());
        }
    });
    tools->addAction(p.t("Media support…", "媒体支持…"), this, [&p] { p.mediaSupport(); });
    tools->addAction(p.t("Start all", "全部开始"), this, [&p] {
        for (const auto& task : p.service.snapshot())
            if (!isTerminal(task.state) && task.state != TaskState::Scheduled)
                p.service.resume(task.id);
    });
    tools->addAction(p.t("Pause all", "全部暂停"), this, [&p] {
        if (p.projects)
            p.projects->pauseAll();
        for (const auto& task : p.service.snapshot())
            p.service.pause(task.id);
    });
    tools->addSeparator();
    tools->addAction(settings);
    auto* scanSettings = tools->addAction(p.t("Completion scanning…", "完成后扫描…"), this, [this, &p] {
        cancelCompletionCountdown();
        p.completionScanDialog();
    });
    scanSettings->setObjectName("completionScanSettingsAction");
    ui::watchFuture(
        p.service.completionScanIssue(), this,
        [&p](std::string issue) {
            if (!issue.empty())
                p.notify("MaskPilot",
                         p.t("Completion scanner unavailable. Repair it in Tools → Completion scanning.",
                             "完成后扫描程序不可用。请在“工具 → 完成后扫描”中修复。"));
        },
        [&p](QString issue) { p.notify("MaskPilot", issue); });
    auto* help = menuBar()->addMenu(p.t("Help", "帮助"));
    help->addAction(
        p.t("About MaskPilot Download Manager", "关于 MaskPilot Download Manager"), this, [this, &p] {
            QMessageBox::about(
                this, "MaskPilot Download Manager",
                p.t("MaskPilot Download Manager %1\nOpen-source native download manager.\nC++ · Qt\n\nSource "
                    "and "
                    "runtime support are documented with this build.",
                    "MaskPilot Download Manager %1\n开源原生下载管理器\nC++ · Qt\n\n实际支持范围见随包文档。")
                    .arg(QString::fromLatin1(applicationVersion)));
        });
    connect(newButton, &QPushButton::clicked, this, [this] { newDownload(); });
    connect(batch, &QAction::triggered, this, [&p] { p.batch(); });
    connect(settings, &QAction::triggered, this, [&p] { p.settingsDialog(); });
    connect(p.resumeAction, &QAction::triggered, this, [&p] {
        const auto tasks = p.selected();
        if (tasks.size() == 1 && tasks[0].state == TaskState::AwaitingSelection) {
            p.torrentDetails(tasks[0]);
            return;
        }
        for (const auto& task : tasks)
            if (task.state != TaskState::AwaitingSelection)
                p.service.resume(task.id);
    });
    connect(p.pauseAction, &QAction::triggered, this, [&p] {
        for (const auto& task : p.selected())
            p.service.pause(task.id);
    });
    connect(p.cancelAction, &QAction::triggered, this, [&p] {
        for (const auto& task : p.selected())
            p.service.cancel(task.id);
    });
    connect(p.removeAction, &QAction::triggered, this, [this, &p] {
        if (QMessageBox::question(this, p.t("Remove task records", "移除任务记录"),
                                  p.t("Remove selected records? Downloaded files will be preserved.",
                                      "移除选中的任务记录？已下载的文件将保留。")) == QMessageBox::Yes)
            for (const auto& task : p.selected())
                p.service.remove(task.id);
    });
    connect(p.detailAction, &QAction::triggered, this, [&p] { p.details(); });
    p.detailAction->setObjectName("taskDetailsAction");
    connect(p.table, &QTableView::doubleClicked, this, [&p](const QModelIndex&) { p.details(); });
    connect(p.table->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [&p] { p.updateActions(); });
    connect(p.sidebar, &QListWidget::currentRowChanged, this, [&p] { p.refresh(); });
    connect(p.search, &QLineEdit::textChanged, this, [&p] { p.refresh(); });
    connect(p.table, &QTableView::customContextMenuRequested, this, [this, &p](const QPoint& point) {
        auto tasks = p.selected();
        if (tasks.empty())
            return;
        QMenu menu;
        menu.addAction(p.resumeAction);
        menu.addAction(p.pauseAction);
        menu.addAction(p.cancelAction);
        auto* move = menu.addAction(p.t("Move to queue…", "移动到队列…"), this, [&p] { p.moveToQueue(); });
        move->setObjectName("moveToQueueAction");
        move->setEnabled(std::none_of(tasks.begin(), tasks.end(),
                                      [](const auto& value) { return isActive(value.state); }));
        move->setToolTip(
            p.t("Pause active tasks before moving the selection.", "请先暂停选中的活动任务再移动。"));
        menu.addSeparator();
        auto task = tasks.front();
        if (task.spec.kind == TransferKind::Live) {
            auto* finish = menu.addAction(p.t("Finish and save recording", "结束并保存录制"), this,
                                          [&p, task] { p.service.finishRecording(task.id); });
            finish->setObjectName("finishRecordingAction");
            finish->setEnabled(task.state == TaskState::Downloading || task.state == TaskState::Probing);
        }
        menu.addAction(p.t("Finish seeding", "结束做种"), this,
                       [&p, task] { p.service.stopSeeding(task.id); })
            ->setEnabled(task.state == TaskState::Seeding);
        menu.addAction(p.t("Open file", "打开文件"), this,
                       [task] {
                           if (task.state == TaskState::Completed || task.torrentPublished)
                               QDesktopServices::openUrl(
                                   QUrl::fromLocalFile(pathString(task.spec.destination)));
                       })
            ->setEnabled(task.state == TaskState::Completed || task.torrentPublished);
        menu.addAction(p.t("Open folder", "打开目录"), this, [task] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(pathString(
                task.torrentPublished ? task.spec.destination : task.spec.destination.parent_path())));
        });
        menu.addAction(p.detailAction);
        if (task.scanRequired || task.scanState == ScanState::Waived) {
            auto* requirement = menu.addAction(
                task.scanRequired ? p.t("Explicitly skip scanning for this task", "明确跳过此任务的扫描")
                                  : p.t("Require scanning for this task", "重新要求扫描此任务"),
                this, [this, &p, task] {
                    cancelCompletionCountdown();
                    ui::watchFuture(
                        p.service.setCompletionScanRequired(task.id, !task.scanRequired), this,
                        [&p] { p.refresh(); },
                        [this](QString issue) { QMessageBox::warning(this, "MaskPilot", issue); });
                });
            requirement->setObjectName("scanRequirementAction");
            requirement->setEnabled(!isActive(task.state) && !isTerminal(task.state) &&
                                    (task.scanRequired || !task.torrentPublished));
        }
        menu.addSeparator();
        menu.addAction(p.t("Edit request / sign-in…", "编辑请求 / 登录信息…"), this,
                       [this, &p, task] {
                           AddDownloadDialog dialog(p.chinese, this);
                           dialog.setMediaInspector(p.mediaInspector, task.id);
                           dialog.setSpecification(task.spec);
                           dialog.setQueues(p.knownQueues, task.queueId, false);
                           dialog.setWindowTitle(p.t("Edit request", "编辑下载请求"));
                           if (auto* destination = dialog.findChild<QLineEdit*>("downloadDestination"))
                               destination->setReadOnly(true);
                           if (task.spec.kind == TransferKind::BitTorrent) {
                               dialog.findChild<QLineEdit*>("downloadUrl")->setReadOnly(true);
                               dialog.findChild<QPushButton*>("chooseTorrentFile")->setEnabled(false);
                               if (task.torrentPublished)
                                   dialog.findChild<QCheckBox*>("torrentAllFiles")->setEnabled(false);
                           }
                           if (auto* button = dialog.findChild<QPushButton*>("confirmDownload"))
                               button->setText(p.t("Save request", "保存请求"));
                           dialog.setStoredProxyCredentials(!task.proxyCredentialRef.empty());
                           if (dialog.exec() == QDialog::Accepted) {
                               ui::watchFuture(
                                   p.service.updateRequestWithCredentials(task.id, dialog.specification(),
                                                                          dialog.rememberProxyCredentials()),
                                   this, [&p] { p.refresh(); },
                                   [this](QString error) { QMessageBox::warning(this, "MaskPilot", error); });
                           }
                       })
            ->setEnabled(task.ownerId == "standalone" && !isActive(task.state) && !isTerminal(task.state));
        if (task.ownerId == "standalone") {
            if (task.spec.kind == TransferKind::Http && task.spec.httpRequest.method == HttpMethod::Get &&
                task.projectId.empty()) {
                auto* recover = menu.addAction(
                    p.t("Recover link / sign-in in browser…", "在浏览器恢复链接／登录…"), this,
                    [this, &p, task] {
                        cancelCompletionCountdown();
                        p.service.pause(task.id);
                        ui::watchFuture(
                            p.service.beginBrowserRecovery(task.id), this,
                            [&p](BrowserRecoverySession session) {
                                if (!QDesktopServices::openUrl(
                                        QUrl::fromEncoded(qs(session.pageUrl).toUtf8())))
                                    p.notify(
                                        "MaskPilot",
                                        p.t("Open the source page, then choose this task in the browser "
                                            "extension.",
                                            "请打开来源页，再在浏览器扩展的“桌面任务”中选择此任务恢复。"));
                                else
                                    p.notify(
                                        "MaskPilot",
                                        p.t("Sign in on the source page, then choose this task in the "
                                            "browser extension to recover it.",
                                            "请在来源页登录，再在浏览器扩展的“桌面任务”中选择此任务恢复。"));
                            },
                            [&p](QString) {
                                p.notify("MaskPilot", p.t("Pause this HTTP task before browser recovery.",
                                                          "请先暂停此 HTTP 任务，再使用浏览器辅助恢复。"));
                            });
                    });
                recover->setObjectName("browserRecoverySourceAction");
                recover->setEnabled(!isActive(task.state) && !isTerminal(task.state));
            }
            auto* forget = menu.addAction(
                p.t("Forget saved proxy credentials", "清除已保存代理凭据"), this, [this, &p, task] {
                    ui::watchFuture(
                        p.service.forgetSavedProxyCredentials(task.id), this, [&p] { p.refresh(); },
                        [this](QString error) { QMessageBox::warning(this, "MaskPilot", error); });
                });
            forget->setObjectName("forgetProxyCredentialsAction");
            forget->setEnabled(!isActive(task.state) && !task.proxyCredentialRef.empty());
        } else {
            auto* rebind = menu.addAction(
                p.t("Rebind to current context", "使用当前上下文配置重新绑定"), this, [this, &p, task] {
                    ui::watchFuture(
                        p.service.rebindTask(task.id), this, [&p] { p.refresh(); },
                        [this](QString error) { QMessageBox::warning(this, "MaskPilot", error); });
                });
            rebind->setObjectName("rebindContextAction");
            rebind->setEnabled(!isActive(task.state) && !isTerminal(task.state));
        }
        menu.addAction(p.t("Update URL…", "更新下载地址…"), this,
                       [this, &p, task] {
                           bool ok;
                           auto url =
                               QInputDialog::getText(this, p.t("Update URL", "更新下载地址"),
                                                     p.t("New URL (request credentials will be cleared)",
                                                         "新地址（将清除旧请求凭据）"),
                                                     QLineEdit::Normal, qs(task.spec.url), &ok);
                           if (ok && !url.isEmpty())
                               p.service.updateUrl(task.id, url.toStdString());
                       })
            ->setEnabled(!isActive(task.state) && !isTerminal(task.state) &&
                         task.spec.kind != TransferKind::BitTorrent);
        menu.addAction(
                p.t("Restart from beginning", "从头重新下载"), this,
                [this, &p, task] {
                    if (QMessageBox::question(this, "MaskPilot",
                                              p.t("Discard this task's cached parts and download again?",
                                                  "丢弃此任务缓存的分段，并重新下载？")) == QMessageBox::Yes)
                        p.service.restart(task.id);
                })
            ->setEnabled(!isActive(task.state) && !isTerminal(task.state) && !task.torrentPublished);
        menu.addAction(p.t("Set priority…", "设置优先级…"), this, [this, &p, task] {
            bool ok;
            auto priority =
                QInputDialog::getInt(this, "MaskPilot", p.t("Priority (-10 to 10)", "优先级（-10 至 10）"),
                                     task.spec.priority, -10, 10, 1, &ok);
            if (ok)
                p.service.setPriority(task.id, priority);
        });
        menu.addAction(p.t("Schedule…", "计划时间…"), this,
                       [this, &p, task] {
                           QDialog dialog(this);
                           dialog.setWindowTitle(p.t("Schedule download", "计划下载"));
                           auto* layout = new QVBoxLayout(&dialog);
                           auto* time = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(3600));
                           time->setCalendarPopup(true);
                           time->setDisplayFormat("yyyy-MM-dd HH:mm:ss t");
                           layout->addWidget(time);
                           auto* buttons =
                               new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
                           layout->addWidget(buttons);
                           connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
                           connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                           if (dialog.exec() == QDialog::Accepted)
                               p.service.schedule(task.id, time->dateTime().toSecsSinceEpoch(), 0);
                       })
            ->setEnabled(!isActive(task.state) && !isTerminal(task.state));
        menu.exec(p.table->viewport()->mapToGlobal(point));
    });
    p.tray = new QSystemTrayIcon(windowIcon(), this);
    auto* trayMenu = new QMenu(this);
    trayMenu->addAction(p.t("Show MaskPilot", "显示 MaskPilot"), this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    trayMenu->addAction(p.t("New download…", "新建下载…"), this, [this] {
        showNormal();
        newDownload();
    });
    trayMenu->addAction(p.t("Quit", "退出"), this, &MainWindow::requestQuit);
    p.tray->setContextMenu(trayMenu);
    p.tray->setToolTip("MaskPilot Download Manager");
    if (QSystemTrayIcon::isSystemTrayAvailable())
        p.tray->show();
    connect(p.tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick) {
            showNormal();
            raise();
            activateWindow();
        }
    });
    p.timer = new QTimer(this);
    connect(p.timer, &QTimer::timeout, this, [&p] { p.refresh(); });
    p.timer->start(400);
    restoreGeometry(p.settings.value("geometry").toByteArray());
    p.refresh();
}
MainWindow::~MainWindow() = default;
void MainWindow::setMediaInspector(MediaInspectionFunction inspector) {
    impl_->mediaInspector = std::move(inspector);
}
DownloadSpec MainWindow::defaultSpec(const QString& url) const {
    DownloadSpec spec;
    spec.url = url.toStdString();
    spec.kind = sourceKind(spec);
    spec.connections = impl_->settings.value("connections", 4).toInt();
    spec.proxy = impl_->settings.value("proxy").toString().toStdString();
    spec.mediaDownloader =
        std::filesystem::path(impl_->settings.value("mediaDownloader").toString().toStdU16String());
    spec.mediaMuxer = std::filesystem::path(impl_->settings.value("mediaMuxer").toString().toStdU16String());
    auto folder =
        impl_->settings
            .value("downloadDirectory", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
            .toString();
    const auto extension = QFileInfo(filenameFor(url)).suffix().toLower();
    const auto rules =
        impl_->settings
            .value("categoryRules",
                   "Documents=pdf,doc,docx,xls,xlsx,ppt,pptx,txt\nSoftware=exe,msi,msix\nVideo=mp4,mkv,webm,"
                   "mov\nMusic=mp3,flac,wav,m4a\nArchives=zip,7z,rar,tar,gz")
            .toString();
    for (const auto& rule : rules.split('\n', Qt::SkipEmptyParts)) {
        auto equal = rule.indexOf('=');
        if (equal > 0 && rule.mid(equal + 1).toLower().split(',', Qt::SkipEmptyParts).contains(extension))
            spec.category = rule.left(equal).trimmed().toStdString();
    }
    if (impl_->settings.value("categorySubfolders", false).toBool()) {
        auto category = qs(spec.category);
        category.replace(QRegularExpression(R"([<>:"/\\|?*\x00-\x1f])"), "_");
        if (category == "." || category == ".." || category.isEmpty())
            category = "General";
        folder = QDir(folder).filePath(category);
    }
    spec.destination = std::filesystem::path(QDir(folder).filePath(filenameFor(url)).toStdU16String());
    return spec;
}
void MainWindow::cancelCompletionCountdown() {
    if (impl_->completionDialog)
        impl_->completionDialog->reject();
}
void MainWindow::setSiteProjectService(SiteProjectService& projects,
                                       std::function<void(const std::filesystem::path&)> opener) {
    impl_->projects = &projects;
    impl_->projectOpener = std::move(opener);
    if (impl_->projectsAction)
        impl_->projectsAction->setEnabled(true);
    cancelCompletionCountdown();
}
void MainWindow::setSiteProjectUnavailable() {
    impl_->projectsUnavailable = true;
    const auto message = impl_->t("Site project storage is unavailable. Ordinary downloads remain available.",
                                  "站点项目存储不可用，普通下载仍可使用。");
    if (impl_->projectsAction)
        impl_->projectsAction->setToolTip(message);
    statusBar()->showMessage(message);
    cancelCompletionCountdown();
}
void MainWindow::newDownload(QString url) {
    if (url.isEmpty()) {
        auto clipboard = QApplication::clipboard()->text().trimmed();
        if (clipboard.startsWith("http://") || clipboard.startsWith("https://") ||
            isFtpUrl(clipboard.toStdString()) || isTorrentUrl(clipboard.toStdString()) ||
            isEd2kUrl(clipboard.toStdString()))
            url = clipboard;
    }
    const auto owner = impl_->contextFilter->currentData().toString();
    if (!owner.isEmpty() && owner != "standalone" && !isEd2kUrl(url.toStdString())) {
        impl_->newContextDownload(url);
        return;
    }
    AddDownloadDialog dialog(impl_->chinese, this, url);
    dialog.setMediaInspector(impl_->mediaInspector);
    dialog.setQueues(impl_->knownQueues, impl_->selectedQueueId());
    if (dialog.exec() == QDialog::Accepted) {
        if (isEd2kUrl(dialog.specification().url))
            impl_->contextFilter->setCurrentIndex(impl_->contextFilter->findData("standalone"));
        impl_->queueFilter->setCurrentIndex(impl_->queueFilter->findData(qs(dialog.queueId())));
        enqueue(dialog.specification(), {}, {}, dialog.rememberProxyCredentials(), dialog.queueId());
    }
}
void MainWindow::enqueue(DownloadSpec spec, QString requestId,
                         std::function<void(QString, QString)> completion, bool rememberProxy,
                         std::string queueId) {
    if (impl_->completionDialog)
        impl_->completionDialog->reject();
    impl_->pending.push_back(
        {impl_->service.add(std::move(spec), requestId.toStdString(), rememberProxy, std::move(queueId)),
         std::move(completion)});
}
void MainWindow::requestQuit() {
    impl_->quitting = true;
    close();
}
void MainWindow::closeEvent(QCloseEvent* event) {
    impl_->settings.setValue("geometry", saveGeometry());
    if (!impl_->quitting && impl_->settings.value("closeToTray", true).toBool() && impl_->tray->isVisible()) {
        hide();
        event->ignore();
        return;
    }
    event->accept();
    if (impl_->projects)
        impl_->projects->pauseAll(false);
    QApplication::quit();
}
void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasText())
        event->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent* event) {
    auto text = event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty()
                    ? event->mimeData()->urls().first().toString()
                    : event->mimeData()->text().trimmed();
    if (text.startsWith("https://") || text.startsWith("http://") || isFtpUrl(text.toStdString()) ||
        isTorrentUrl(text.toStdString()) || isEd2kUrl(text.toStdString())) {
        newDownload(text);
        event->acceptProposedAction();
    }
}
} // namespace maskpilot
