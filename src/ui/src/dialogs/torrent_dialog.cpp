#include "../windows/main_window_p.h"
#include <QTreeWidget>

namespace maskpilot {
void MainWindow::Impl::torrentDetails(const Task& initial) {
    auto* dialog = new QDialog(window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(t("Torrent details & files", "种子详情与文件"));
    dialog->resize(790, 590);
    auto* layout = new QVBoxLayout(dialog);
    auto* title = new QLabel;
    title->setTextFormat(Qt::PlainText);
    title->setStyleSheet("font-size:18px;font-weight:600;");
    auto* status = new QLabel;
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    auto* destination = new QLabel(pathString(initial.spec.destination));
    destination->setTextFormat(Qt::PlainText);
    destination->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);
    layout->addWidget(status);
    layout->addWidget(destination);
    auto* files = new QTreeWidget;
    files->setObjectName("torrentFiles");
    files->setHeaderLabels({t("Files to download", "下载文件"), t("Size", "大小")});
    files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    files->setRootIsDecorated(false);
    files->setAlternatingRowColors(true);
    layout->addWidget(files, 1);
    auto* controls = new QHBoxLayout;
    auto* selectAll = new QPushButton(t("Select all", "全选"));
    auto* none = new QPushButton(t("Clear selection", "取消全选"));
    controls->addWidget(selectAll);
    controls->addWidget(none);
    controls->addStretch();
    auto* apply = new QPushButton(t("Save selection", "保存选择"));
    apply->setObjectName("applyTorrentSelection");
    auto* start = new QPushButton(t("Save & start", "保存并开始"));
    start->setObjectName("startTorrentSelection");
    start->setDefault(true);
    controls->addWidget(apply);
    controls->addWidget(start);
    layout->addLayout(controls);
    auto* note = new QLabel(t("Pause before changing files. Published files are kept when removing a task.",
                              "修改文件选择前请先暂停任务。移除任务会保留已发布的文件。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(close, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(close);
    for (const auto& [button, check] : {std::pair{selectAll, Qt::Checked}, std::pair{none, Qt::Unchecked}})
        QObject::connect(button, &QPushButton::clicked, dialog, [files, check] {
            for (int n = 0; n < files->topLevelItemCount(); ++n)
                files->topLevelItem(n)->setCheckState(0, check);
        });
    struct View {
        std::shared_ptr<const std::vector<TorrentFile>> files;
        bool saving = false;
    };
    auto view = std::make_shared<View>();
    auto refresh = [this, id = initial.id, view, files, title, status, apply, start, selectAll, none] {
        const auto tasks = service.snapshot();
        const auto it =
            std::find_if(tasks.begin(), tasks.end(), [&](const auto& task) { return task.id == id; });
        if (it == tasks.end()) {
            apply->setEnabled(false);
            start->setEnabled(false);
            files->setEnabled(false);
            status->setText(t("Task record was removed.", "任务记录已移除。"));
            return;
        }
        title->setText(it->torrent.name.empty() ? pathString(it->spec.destination.filename())
                                                : qs(it->torrent.name));
        status->setText(stateText(it->state, chinese) + "  ·  " + t("Downloaded ", "已下载 ") +
                        bytes(it->downloaded) + " / " + bytes(it->total) + "  ·  " +
                        t("Uploaded ", "已上传 ") + bytes(it->uploaded) + "\n" + t("Download ", "下载 ") +
                        bytes(it->bytesPerSecond) + "/s  ·  " + t("Upload ", "上传 ") +
                        bytes(it->uploadSpeed) + "/s  ·  " + t("Peers / seeds ", "连接 / 种子 ") +
                        QString::number(it->peers) + " / " + QString::number(it->seeds) + "\n" +
                        qs(it->torrent.infoHash) + (it->error.empty() ? QString() : "\n" + qs(it->error)));
        if (it->torrent.files != view->files) {
            files->clear();
            view->files = it->torrent.files;
            if (view->files)
                for (const auto& file : *view->files) {
                    if (file.padding)
                        continue;
                    auto* item = new QTreeWidgetItem(files, {qs(file.path), bytes(file.size)});
                    item->setData(0, Qt::UserRole, file.index);
                    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                    item->setCheckState(0, torrentFileWanted(it->spec.torrent, file) ? Qt::Checked
                                                                                     : Qt::Unchecked);
                }
        }
        const bool editable = !view->saving && view->files && !isActive(it->state) &&
                              !isTerminal(it->state) && !it->torrentPublished;
        files->setEnabled(true);
        for (int n = 0; n < files->topLevelItemCount(); ++n) {
            auto* item = files->topLevelItem(n);
            item->setFlags(editable ? item->flags() | Qt::ItemIsUserCheckable
                                    : item->flags() & ~Qt::ItemIsUserCheckable);
        }
        selectAll->setEnabled(editable);
        none->setEnabled(editable);
        apply->setEnabled(editable);
        start->setEnabled(editable);
    };
    auto save = [this, id = initial.id, view, files, refresh,
                 dialog = QPointer<QDialog>(dialog)](bool resume) {
        std::vector<int> indices;
        for (int n = 0; n < files->topLevelItemCount(); ++n) {
            const auto* item = files->topLevelItem(n);
            if (item->checkState(0) == Qt::Checked)
                indices.push_back(item->data(0, Qt::UserRole).toInt());
        }
        if (indices.empty()) {
            QMessageBox::information(dialog, "MaskPilot",
                                     t("Select at least one file.", "请至少选择一个文件。"));
            return;
        }
        view->saving = true;
        refresh();
        pending.push_back({service.selectTorrentFiles(id, std::move(indices)),
                           [this, view, refresh, dialog, resume](QString saved, QString error) {
                               view->saving = false;
                               if (!dialog)
                                   return;
                               if (!error.isEmpty())
                                   QMessageBox::warning(dialog, "MaskPilot", error);
                               else if (resume) {
                                   if (completionDialog)
                                       completionDialog->reject();
                                   service.resume(saved.toStdString());
                               }
                               refresh();
                           }});
    };
    QObject::connect(apply, &QPushButton::clicked, dialog, [save] { save(false); });
    QObject::connect(start, &QPushButton::clicked, dialog, [save] { save(true); });
    auto* poll = new QTimer(dialog);
    QObject::connect(poll, &QTimer::timeout, dialog, refresh);
    poll->start(400);
    refresh();
    dialog->show();
}
} // namespace maskpilot
