#include "../windows/main_window_p.h"
#include "async_ui.h"
#include <QTimeEdit>
#include <QTreeWidget>
#include <array>
#include <limits>

namespace maskpilot {
namespace {
QString queueName(const DownloadQueue& queue, bool chinese) {
    return queue.id == defaultQueueId && queue.name == "Default"
               ? QString::fromUtf8(chinese ? "默认队列" : "Default")
               : qs(queue.name);
}
class QueueManagerDialog final : public QDialog {
    DownloadService& service_;
    bool chinese_, busy_ = false;
    std::vector<DownloadQueue> queues_;
    std::string selected_;
    QTreeWidget* list_;
    QLineEdit* name_;
    QSpinBox *active_, *download_, *upload_;
    QCheckBox* enabled_;
    QCheckBox* weekly_;
    std::array<QCheckBox*, 7> days_{};
    QTimeEdit *start_, *end_;
    QLineEdit* zone_;
    QLabel* status_;
    QWidget* editor_;
    QPushButton *save_, *create_, *pause_, *enable_, *remove_;
    QString tr(const char* en, const char* zh) const { return QString::fromUtf8(chinese_ ? zh : en); }
    const DownloadQueue* current() const {
        const auto found = std::find_if(queues_.begin(), queues_.end(),
                                        [&](const auto& value) { return value.id == selected_; });
        return found == queues_.end() ? nullptr : &*found;
    }
    void busy(bool value) {
        busy_ = value;
        list_->setEnabled(!value);
        editor_->setEnabled(!value);
        create_->setEnabled(!value);
        save_->setEnabled(!value);
        const auto* policy = current();
        pause_->setEnabled(!value && policy && policy->enabled);
        enable_->setEnabled(!value && policy && !policy->enabled);
        remove_->setEnabled(!value && policy && policy->id != defaultQueueId);
    }
    void failure(QString error) {
        busy(false);
        status_->setText(error);
    }
    void showCurrent() {
        const auto* value = current();
        if (!value)
            return;
        name_->setText(qs(value->name));
        active_->setValue(value->maxActive);
        download_->setValue(static_cast<int>(
            std::min<std::int64_t>(value->downloadLimit / 1024, std::numeric_limits<int>::max())));
        upload_->setValue(static_cast<int>(
            std::min<std::int64_t>(value->uploadLimit / 1024, std::numeric_limits<int>::max())));
        enabled_->setChecked(value->enabled);
        weekly_->setChecked(value->schedule.enabled);
        for (int day = 0; day < 7; ++day)
            days_[day]->setChecked((value->schedule.weekdays & (1 << day)) != 0);
        start_->setTime(QTime(value->schedule.startMinute / 60, value->schedule.startMinute % 60));
        end_->setTime(QTime(value->schedule.endMinute / 60, value->schedule.endMinute % 60));
        zone_->setText(qs(value->schedule.timeZone));
        status_->setText(
            value->enabled && value->schedule.enabled
                ? tr("Weekly window configured. Eligible tasks wait outside its local hours.",
                     "已设置每周运行窗口；窗口外，待运行任务会等待。")
            : value->enabled
                ? tr("Available. Global and task limits also apply.", "队列可用，同时遵守全局与任务限制。")
                : tr("Paused. Enabling this queue keeps manually paused tasks stopped.",
                     "队列已暂停；启用后，手动暂停的任务仍保持停止。"));
        busy(false);
    }
    void newQueue() {
        selected_.clear();
        list_->clearSelection();
        list_->setCurrentItem(nullptr);
        name_->clear();
        active_->setValue(3);
        download_->setValue(0);
        upload_->setValue(0);
        enabled_->setChecked(true);
        weekly_->setChecked(false);
        for (auto* day : days_)
            day->setChecked(true);
        start_->setTime(QTime(0, 0));
        end_->setTime(QTime(0, 0));
        zone_->setText("UTC");
        status_->setText(tr("Create a queue for its own concurrency and bandwidth limits.",
                            "新建队列，可分别设置任务并发与带宽。"));
        busy(false);
        name_->setFocus();
    }
    void load() {
        busy(true);
        ui::watchFuture(
            service_.queues(), this,
            [this](std::vector<DownloadQueue> values) {
                queues_ = std::move(values);
                const QSignalBlocker blocker(list_);
                list_->clear();
                const auto tasks = service_.snapshot();
                for (const auto& value : queues_) {
                    const auto members = std::count_if(tasks.begin(), tasks.end(), [&](const auto& task) {
                        return task.queueId == value.id;
                    });
                    auto* item =
                        new QTreeWidgetItem(list_, {queueName(value, chinese_),
                                                    !value.enabled           ? tr("Paused", "已暂停")
                                                    : value.schedule.enabled ? tr("Weekly window", "每周窗口")
                                                                             : tr("Available", "可用"),
                                                    QString::number(members)});
                    item->setData(0, Qt::UserRole, qs(value.id));
                    if (value.id == selected_)
                        list_->setCurrentItem(item);
                }
                if (current())
                    showCurrent();
                else
                    newQueue();
                busy(false);
            },
            [this](QString error) { failure(error); });
    }
    void save(std::optional<bool> enabled = {}) {
        if (busy_)
            return;
        QueueConfiguration draft;
        const auto* previous = current();
        if (previous) {
            draft.id = previous->id;
            draft.expectedRevision = previous->revision;
        }
        // Enable/pause changes only the accepted policy, never unsaved form values.
        draft.name = enabled && previous ? previous->name : name_->text().trimmed().toStdString();
        draft.maxActive = enabled && previous ? previous->maxActive : active_->value();
        draft.downloadLimit = enabled && previous ? previous->downloadLimit
                                                  : static_cast<std::int64_t>(download_->value()) * 1024;
        draft.uploadLimit =
            enabled && previous ? previous->uploadLimit : static_cast<std::int64_t>(upload_->value()) * 1024;
        draft.enabled = enabled.value_or(enabled_->isChecked());
        if (enabled && previous)
            draft.schedule = previous->schedule;
        else {
            draft.schedule.enabled = weekly_->isChecked();
            draft.schedule.weekdays = 0;
            for (int day = 0; day < 7; ++day)
                if (days_[day]->isChecked())
                    draft.schedule.weekdays |= 1 << day;
            draft.schedule.startMinute = start_->time().hour() * 60 + start_->time().minute();
            draft.schedule.endMinute = end_->time().hour() * 60 + end_->time().minute();
            draft.schedule.timeZone = zone_->text().trimmed().toStdString();
        }
        busy(true);
        status_->setText(tr("Saving queue…", "正在保存队列…"));
        ui::watchFuture(
            service_.configureQueue(std::move(draft)), this,
            [this](DownloadQueue value) {
                selected_ = value.id;
                load();
            },
            [this](QString error) { failure(error); });
    }

  public:
    QueueManagerDialog(DownloadService& service, bool chinese, QWidget* parent)
        : QDialog(parent), service_(service), chinese_(chinese) {
        setObjectName("queueManagerDialog");
        setWindowTitle(tr("Download queues", "下载队列"));
        resize(920, 720);
        auto* outer = new QVBoxLayout(this);
        auto* note = new QLabel(
            tr("Queues share the global connection and bandwidth limits. Moving a task keeps its profile, "
               "proxy and files. Pause active tasks before moving them.",
               "队列共同遵守全局并发与带宽限制。移动任务不会改变环境、代理或文件；请先暂停活动任务再移动。"));
        note->setWordWrap(true);
        outer->addWidget(note);
        auto* body = new QHBoxLayout;
        outer->addLayout(body, 1);
        list_ = new QTreeWidget;
        list_->setObjectName("queueList");
        list_->setHeaderLabels({tr("Queue", "队列"), tr("State", "状态"), tr("Tasks", "任务")});
        list_->setRootIsDecorated(false);
        list_->setMinimumWidth(320);
        body->addWidget(list_, 1);
        editor_ = new QWidget;
        auto* form = new QFormLayout(editor_);
        form->setSpacing(14);
        body->addWidget(editor_, 2);
        name_ = new QLineEdit;
        name_->setObjectName("queueName");
        name_->setMaxLength(64);
        form->addRow(tr("Name", "名称"), name_);
        active_ = new QSpinBox;
        active_->setObjectName("queueMaxActive");
        active_->setRange(1, 8);
        form->addRow(tr("Active tasks", "活动任务上限"), active_);
        const auto rate = [&](const char* objectName, const QString& label) {
            auto* value = new QSpinBox;
            value->setObjectName(objectName);
            value->setRange(0, std::numeric_limits<int>::max());
            value->setSpecialValueText(tr("Unlimited", "不限速"));
            value->setSuffix(" KiB/s");
            form->addRow(label, value);
            return value;
        };
        download_ = rate("queueDownloadLimit", tr("Queue download", "队列下载限速"));
        upload_ = rate("queueUploadLimit", tr("Queue BT upload", "队列 BT 上传限速"));
        enabled_ = new QCheckBox(tr("Allow this queue to run", "允许此队列运行"));
        enabled_->setObjectName("queueEnabled");
        form->addRow(enabled_);
        weekly_ = new QCheckBox(tr("Use a weekly running window", "按每周窗口运行"));
        weekly_->setObjectName("queueWeeklyEnabled");
        form->addRow(weekly_);
        auto* days = new QWidget;
        auto* dayLayout = new QHBoxLayout(days);
        dayLayout->setContentsMargins(0, 0, 0, 0);
        const std::array<const char*, 7> english{"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        const std::array<const char*, 7> chineseDays{"一", "二", "三", "四", "五", "六", "日"};
        for (int day = 0; day < 7; ++day) {
            days_[day] = new QCheckBox(tr(english[day], chineseDays[day]));
            days_[day]->setObjectName("queueWeekday" + QString::number(day));
            dayLayout->addWidget(days_[day]);
        }
        form->addRow(tr("Starting days", "开始日期"), days);
        auto* times = new QWidget;
        auto* timeLayout = new QHBoxLayout(times);
        timeLayout->setContentsMargins(0, 0, 0, 0);
        start_ = new QTimeEdit;
        end_ = new QTimeEdit;
        start_->setObjectName("queueWindowStart");
        end_->setObjectName("queueWindowEnd");
        start_->setDisplayFormat("HH:mm");
        end_->setDisplayFormat("HH:mm");
        timeLayout->addWidget(start_);
        timeLayout->addWidget(new QLabel(tr("to", "至")));
        timeLayout->addWidget(end_);
        form->addRow(tr("Local hours", "当地时间"), times);
        zone_ = new QLineEdit("UTC");
        zone_->setObjectName("queueTimeZone");
        zone_->setMaxLength(128);
        zone_->setPlaceholderText("UTC / Asia/Shanghai / America/Los_Angeles");
        form->addRow(tr("IANA time zone", "IANA 时区"), zone_);
        auto* scheduleNote = new QLabel(
            tr("End at or before start crosses midnight. Equal times mean 24 hours. Days select the "
               "window's start; only unfinished tasks with running intent reopen.",
               "结束时间不晚于开始时间时跨午夜；时间相同表示24小时。星期选择窗口开始的日期；"
               "仅待运行且未完成的任务会在窗口内继续。"));
        scheduleNote->setWordWrap(true);
        form->addRow(scheduleNote);
        auto* media = new QLabel(
            tr("Pause running media downloads before reducing limits that their current rate cannot meet.",
               "降低限速时，若活动媒体任务的当前速度无法满足新限制，请先暂停这些任务。"));
        media->setWordWrap(true);
        form->addRow(media);
        status_ = new QLabel;
        status_->setObjectName("queueStatus");
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        form->addRow(status_);
        auto* actions = new QHBoxLayout;
        outer->addLayout(actions);
        const auto button = [&](const char* objectName, const QString& text) {
            auto* value = new QPushButton(text);
            value->setObjectName(objectName);
            actions->addWidget(value);
            return value;
        };
        create_ = button("queueNew", tr("New queue", "新建队列"));
        save_ = button("queueSave", tr("Save", "保存"));
        pause_ = button("queuePause", tr("Pause queue", "暂停队列"));
        enable_ = button("queueEnable", tr("Enable queue", "启用队列"));
        remove_ = button("queueRemove", tr("Remove empty queue", "删除空队列"));
        auto* close = button("queueClose", tr("Close", "关闭"));
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
        connect(create_, &QPushButton::clicked, this, [this] { newQueue(); });
        connect(save_, &QPushButton::clicked, this, [this] { save(); });
        connect(pause_, &QPushButton::clicked, this, [this] { save(false); });
        connect(enable_, &QPushButton::clicked, this, [this] { save(true); });
        connect(remove_, &QPushButton::clicked, this, [this] {
            const auto* value = current();
            if (!value || busy_)
                return;
            busy(true);
            ui::watchFuture(
                service_.removeQueue(value->id, value->revision), this,
                [this] {
                    selected_.clear();
                    load();
                },
                [this](QString error) { failure(error); });
        });
        connect(list_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            if (item) {
                selected_ = item->data(0, Qt::UserRole).toString().toStdString();
                showCurrent();
            }
        });
        newQueue();
        load();
    }
    void reject() override {
        if (!busy_)
            QDialog::reject();
    }
};
class MoveDialog final : public QDialog {
  public:
    using QDialog::QDialog;
    bool saving = false;
    void reject() override {
        if (!saving)
            QDialog::reject();
    }
};
} // namespace

std::string MainWindow::Impl::selectedQueueId() const {
    const auto selected = queueFilter ? queueFilter->currentData().toString().toStdString() : std::string{};
    return selected.empty() ? std::string(defaultQueueId) : selected;
}
void MainWindow::Impl::refreshQueues() {
    if (queueRequest && queueRequest->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            knownQueues = queueRequest->get();
            const auto selected = queueFilter->currentData().toString();
            const QSignalBlocker blocker(queueFilter);
            queueFilter->clear();
            queueFilter->addItem(t("All queues", "全部队列"), QString{});
            model->queueLabels.clear();
            for (const auto& value : knownQueues) {
                const auto name = queueName(value, chinese);
                model->queueLabels[value.id] = name;
                queueFilter->addItem(name + (value.enabled ? QString{} : t(" (paused)", "（已暂停）")),
                                     qs(value.id));
            }
            const auto index = queueFilter->findData(selected);
            queueFilter->setCurrentIndex(index >= 0 ? index : 0);
        } catch (const std::exception&) { /* Core/storage errors are surfaced through service.lastError(). */
        }
        queueRequest.reset();
    }
    if (!queueRequest && unixTime() != lastQueueRefresh) {
        lastQueueRefresh = unixTime();
        queueRequest.emplace(service.queues());
    }
}
void MainWindow::Impl::queuesDialog() {
    QueueManagerDialog dialog(service, chinese, window);
    dialog.exec();
    lastQueueRefresh = 0;
    refreshQueues();
}
void MainWindow::Impl::moveToQueue() {
    const auto tasks = selected();
    if (tasks.empty())
        return;
    MoveDialog dialog(window);
    dialog.setObjectName("moveQueueDialog");
    dialog.setWindowTitle(t("Move selected tasks", "移动选中任务"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(
        t("Moves every selected inactive task together. Files, profile and proxy stay with the task.",
          "一次移动全部选中的非活动任务，文件、环境和代理保持原配置。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* choice = new QComboBox;
    choice->setObjectName("moveQueueTarget");
    for (const auto& value : knownQueues)
        choice->addItem(queueName(value, chinese), qs(value.id));
    if (choice->count() == 0)
        choice->addItem(t("Default", "默认队列"), defaultQueueId);
    layout->addWidget(choice);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* confirm = buttons->addButton(t("Move tasks", "移动任务"), QDialogButtonBox::AcceptRole);
    confirm->setObjectName("confirmQueueMove");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(confirm, &QPushButton::clicked, &dialog, [&] {
        if (dialog.saving)
            return;
        std::vector<std::string> ids;
        for (const auto& task : tasks)
            ids.push_back(task.id);
        const auto target = choice->currentData().toString().toStdString();
        dialog.saving = true;
        buttons->setEnabled(false);
        choice->setEnabled(false);
        ui::watchFuture(
            service.moveToQueue(std::move(ids), target), &dialog,
            [&, target] {
                queueFilter->setCurrentIndex(queueFilter->findData(qs(target)));
                dialog.accept();
                refresh();
            },
            [&](QString error) {
                dialog.saving = false;
                buttons->setEnabled(true);
                choice->setEnabled(true);
                QMessageBox::warning(&dialog, "MaskPilot", error);
            });
    });
    dialog.exec();
}
} // namespace maskpilot
