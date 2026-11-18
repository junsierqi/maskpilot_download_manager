#include "../models/task_list_codec.h"
#include "../windows/main_window_p.h"
#include <QHeaderView>
#include <QTreeWidget>
#include <cmath>
namespace maskpilot {
void MainWindow::Impl::details() {
    auto tasks = selected();
    if (tasks.size() != 1)
        return;
    auto task = tasks.front();
    if (task.spec.kind == TransferKind::BitTorrent) {
        torrentDetails(task);
        return;
    }
    auto* dialog = new QDialog(window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(640, 420);
    dialog->setWindowTitle(t("Download details", "任务详情"));
    auto* layout = new QVBoxLayout(dialog);
    auto* text = new QPlainTextEdit;
    text->setReadOnly(true);
    layout->addWidget(text);
    auto update = [this, text, id = task.id] {
        auto list = service.snapshot();
        auto it = std::find_if(list.begin(), list.end(), [&](const auto& value) { return value.id == id; });
        if (it == list.end())
            return;
        auto safe = QUrl(qs(it->spec.url));
        safe.setQuery(QString());
        safe.setFragment({});
        text->setPlainText(
            t("File: ", "文件：") + pathString(it->spec.destination) + "\n\n" + t("Source: ", "来源：") +
            safe.toDisplayString() + "\n" + t("Status: ", "状态：") + stateText(it->state, chinese) + "\n" +
            t("Transferred: ", "已下载：") + bytes(it->downloaded) + " / " + bytes(it->total) + "\n" +
            t("Connections: ", "连接数：") + QString::number(it->spec.connections) + "\n" +
            t("Resume identity: ", "续传身份验证：") +
            (it->resumable ? t("Validated", "已验证") : t("Not available", "不可用")) + "\n" +
            t("Retries: ", "重试次数：") + QString::number(it->retries) + "\n\n" + qs(it->error));
    };
    update();
    auto* tick = new QTimer(dialog);
    QObject::connect(tick, &QTimer::timeout, dialog, update);
    tick->start(1000);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(close, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(close);
    dialog->show();
}

struct MainWindow::Impl::ImportBatch {
    std::vector<DownloadSpec> specs;
    std::vector<std::string> requestIds;
    std::size_t next = 0, contextBound = 0;
    int version = 0;
    bool busy = false;
};
namespace {
enum class ImportReview { Cancel, Confirm, Discard };
ImportReview reviewImport(QWidget* parent, const std::vector<DownloadSpec>& specs, std::size_t completed,
                          int version, std::size_t contextBound, bool chinese, bool retained = false) {
    const auto t = [chinese](const char* en, const char* zh) { return QString::fromUtf8(chinese ? zh : en); };
    QDialog dialog(parent);
    dialog.setObjectName("taskImportPreview");
    dialog.setWindowTitle(t("Review standalone import", "检查独立任务导入"));
    dialog.resize(960, 520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* summary = new QLabel(&dialog);
    summary->setObjectName("taskImportSummary");
    summary->setWordWrap(true);
    auto message = t("%1 tasks; %2 already imported; %3 remaining. Imported tasks belong to "
                     "standalone/default queue and remain "
                     "paused. "
                     "Context bindings and credentials are not restored.",
                     "%1 个任务；已导入 %2 个；剩余 %3 "
                     "个。导入任务属于独立下载区／默认队列并保持暂停；不会恢复环境绑定和凭据。")
                       .arg(specs.size())
                       .arg(completed)
                       .arg(specs.size() - completed);
    if (contextBound)
        message += "\n" + t("%1 context-bound sources will be imported as standalone.",
                            "%1 个上下文来源将导入为独立任务。")
                              .arg(contextBound);
    if (version == 1)
        message +=
            "\n" +
            t("Version 1 did not record source-login requirements. Review login needs before resuming.",
              "版本 1 未记录来源登录要求。恢复前请检查登录需求。");
    if (retained)
        message += "\n" + t("Cancel keeps the remaining requests for later. Discard remaining leaves "
                            "imported tasks untouched.",
                            "取消会保留剩余请求。丢弃剩余请求不会更改已导入的任务。");
    summary->setText(message);
    layout->addWidget(summary);
    auto* tree = new QTreeWidget(&dialog);
    tree->setObjectName("taskImportPreviewTasks");
    tree->setHeaderLabels({t("Source", "来源"), t("Destination", "目标"), t("Proxy route", "代理路径"),
                           t("Source login", "来源登录"), t("Proxy login", "代理登录")});
    for (std::size_t n = completed; n < specs.size(); ++n) {
        const auto& spec = specs[n];
        const auto sourceAuth = requiresOriginAuthentication(spec)
                                    ? t("Required; reenter credentials", "需要；请重新输入凭据")
                                : version == 1 ? t("Unknown (version 1)", "未知（版本 1）")
                                               : t("Not required", "不需要");
        new QTreeWidgetItem(tree, {qs(spec.url), pathString(spec.destination),
                                   spec.proxy.empty() ? t("Direct", "直连") : qs(spec.proxy), sourceAuth,
                                   spec.proxyAuthenticationRequired
                                       ? t("Required; reenter credentials", "需要；请重新输入凭据")
                                       : t("Not required", "不需要")});
    }
    tree->setRootIsDecorated(false);
    tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tree->header()->setStretchLastSection(true);
    layout->addWidget(tree);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* confirm = buttons->addButton(retained ? t("Retry remaining into standalone", "重试剩余独立任务")
                                                : t("Import into standalone", "导入为独立任务"),
                                       QDialogButtonBox::AcceptRole);
    confirm->setObjectName("confirmTaskImport");
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (retained) {
        auto* discard =
            buttons->addButton(t("Discard remaining", "丢弃剩余请求"), QDialogButtonBox::DestructiveRole);
        discard->setObjectName("discardTaskImport");
        QObject::connect(discard, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(2); });
    }
    layout->addWidget(buttons);
    const auto result = dialog.exec();
    return result == 2                   ? ImportReview::Discard
           : result == QDialog::Accepted ? ImportReview::Confirm
                                         : ImportReview::Cancel;
}
} // namespace
void MainWindow::Impl::exportTasks() {
    const auto path = QFileDialog::getSaveFileName(window,
                                                   t("Export task list — contains URLs and torrent metadata",
                                                     "导出任务列表（包含原始地址与种子元数据）"),
                                                   "downloads.json", "JSON (*.json)");
    if (path.isEmpty())
        return;
    try {
        const auto output = ui::encodeTaskList(all);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(output) != output.size() || !file.commit())
            throw std::runtime_error("Cannot save the exported task list.");
    } catch (const std::exception& error) {
        QMessageBox::warning(window, t("Export failed", "导出失败"), qs(error.what()));
    }
}
void MainWindow::Impl::importTasks() {
    if (pendingImport) {
        if (pendingImport->busy) {
            notify("MaskPilot", t("An import is still being saved.", "导入仍在保存。"));
            return;
        }
        const auto decision =
            reviewImport(window, pendingImport->specs, pendingImport->next, pendingImport->version,
                         pendingImport->contextBound, chinese, true);
        if (decision == ImportReview::Confirm)
            continueImport();
        else if (decision == ImportReview::Discard)
            pendingImport.reset();
        return;
    }
    const auto path =
        QFileDialog::getOpenFileName(window, t("Import task list", "导入任务列表"), {}, "JSON (*.json)");
    if (path.isEmpty())
        return;
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024)
            throw std::runtime_error("Cannot read this task list (maximum 32 MiB).");
        auto decoded = ui::decodeTaskList(file.readAll(),
                                          [this](const QString& url) { return window->defaultSpec(url); });
        auto batch = std::make_shared<ImportBatch>();
        batch->version = decoded.version;
        batch->contextBound = decoded.contextBoundCount;
        auto future = std::make_shared<std::future<std::vector<DownloadSpec>>>(
            service.prepareImport(std::move(decoded.downloads)));
        batch->busy = true;
        pendingImport = batch;
        auto* poll = new QTimer(window);
        QObject::connect(poll, &QTimer::timeout, window, [this, poll, future, batch] {
            if (future->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            poll->stop();
            poll->deleteLater();
            try {
                batch->specs = future->get();
                if (pendingImport != batch)
                    throw std::runtime_error("Another import is already pending.");
                batch->busy = false;
                if (reviewImport(window, batch->specs, 0, batch->version, batch->contextBound, chinese) !=
                    ImportReview::Confirm) {
                    pendingImport.reset();
                    return;
                }
                for (std::size_t n = 0; n < batch->specs.size(); ++n)
                    batch->requestIds.push_back(newTaskId());
                pendingImport = batch;
                continueImport();
            } catch (const std::exception& error) {
                if (pendingImport == batch)
                    pendingImport.reset();
                QMessageBox::warning(window, t("Import failed", "导入失败"), qs(error.what()));
            }
        });
        poll->start(50);
    } catch (const std::exception& error) {
        QMessageBox::warning(window, t("Import failed", "导入失败"), qs(error.what()));
    }
}
void MainWindow::Impl::continueImport() {
    const auto batch = pendingImport;
    if (!batch || batch->busy)
        return;
    if (batch->next == batch->specs.size()) {
        const auto count = batch->next;
        pendingImport.reset();
        contextFilter->setCurrentIndex(contextFilter->findData("standalone"));
        queueFilter->setCurrentIndex(queueFilter->findData(defaultQueueId));
        notify("MaskPilot",
               t("Imported %1 paused standalone tasks.", "已导入 %1 个暂停的独立任务。").arg(count));
        return;
    }
    batch->busy = true;
    auto result = std::make_shared<std::future<std::string>>(
        service.add(batch->specs[batch->next], batch->requestIds[batch->next], false));
    auto* poll = new QTimer(window);
    QObject::connect(poll, &QTimer::timeout, window, [this, poll, batch, result] {
        if (result->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        poll->stop();
        poll->deleteLater();
        batch->busy = false;
        try {
            result->get();
            ++batch->next;
            continueImport();
        } catch (const std::exception&) {
            QMessageBox::warning(
                window, t("Import paused", "导入已暂停"),
                t("%1 tasks were imported; %2 remain. Existing tasks were preserved. "
                  "Choose Import tasks again to review and retry only the remaining tasks.",
                  "已导入 %1 个任务；剩余 %2 个。现有任务已保留。再次选择导入任务可检查并仅重试剩余任务。")
                    .arg(batch->next)
                    .arg(batch->specs.size() - batch->next));
        }
    });
    poll->start(50);
}

void MainWindow::Impl::batch() {
    QDialog dialog(window);
    const auto targetQueue = selectedQueueId();
    dialog.resize(650, 510);
    dialog.setWindowTitle(t("Batch standalone downloads", "批量独立下载"));
    auto* layout = new QVBoxLayout(&dialog);
    const auto foundQueue = std::find_if(knownQueues.begin(), knownQueues.end(),
                                         [&](const auto& value) { return value.id == targetQueue; });
    const auto queueName = foundQueue != knownQueues.end() ? qs(foundQueue->name) : t("Default", "默认队列");
    auto* target = new QLabel(t("Adds paused tasks to %1, using standalone request settings.",
                                "使用独立下载区的默认请求设置，加入“%1”并保持暂停。")
                                  .arg(queueName));
    target->setTextFormat(Qt::PlainText);
    target->setWordWrap(true);
    layout->addWidget(target);
    auto* label =
        new QLabel(t("One URL per line, or paste text containing links. A numeric template such as "
                     "file{001..010}.zip is supported (up to 1000 links).",
                     "每行一个地址，或粘贴含链接的文本。支持 file{001..010}.zip 数字模板（最多 1000 条）。"));
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* input = new QPlainTextEdit;
    layout->addWidget(input);
    auto* preview = new QPushButton(t("Extract & preview", "提取并预览"));
    layout->addWidget(preview);
    auto* output = new QListWidget;
    layout->addWidget(output);
    auto* buttons = new QDialogButtonBox;
    auto* add = buttons->addButton(t("Add paused", "添加为暂停任务"), QDialogButtonBox::AcceptRole);
    add->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(preview, &QPushButton::clicked, &dialog, [&] {
        output->clear();
        QStringList links;
        auto text = input->toPlainText();
        QRegularExpression templateExpression(R"(\{(\d+)\.\.(\d+)\})");
        for (auto line : text.split('\n', Qt::SkipEmptyParts)) {
            auto match = templateExpression.match(line);
            if (match.hasMatch()) {
                auto begin = match.captured(1).toInt(), end = match.captured(2).toInt();
                if (end < begin || end - begin >= 1000)
                    continue;
                for (int n = begin; n <= end && links.size() < 1000; ++n) {
                    auto expanded = line;
                    expanded.replace(match.capturedStart(), match.capturedLength(),
                                     QString::number(n).rightJustified(match.captured(1).size(), '0'));
                    links.append(expanded.trimmed());
                }
            } else {
                auto matches = QRegularExpression(R"((?:https?|ftps?)://[^\s<>"']+)").globalMatch(line);
                while (matches.hasNext() && links.size() < 1000)
                    links.append(matches.next().captured());
            }
        }
        links.removeDuplicates();
        for (auto link : links)
            if (QUrl(link).isValid())
                output->addItem(link);
        add->setEnabled(output->count() > 0);
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        contextFilter->setCurrentIndex(contextFilter->findData("standalone"));
        queueFilter->setCurrentIndex(queueFilter->findData(qs(targetQueue)));
        for (int i = 0; i < output->count(); ++i) {
            const auto url = output->item(i)->text();
            auto spec = window->defaultSpec(url);
            spec.startPaused = true;
            window->enqueue(std::move(spec), {}, {}, false, targetQueue);
        }
        dialog.accept();
    });
    dialog.exec();
}
} // namespace maskpilot
