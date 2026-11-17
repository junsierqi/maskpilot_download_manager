#include "../windows/main_window_p.h"
namespace maskpilot {
void MainWindow::Impl::details() {
    auto tasks = selected();
    if (tasks.size() != 1)
        return;
    auto task = tasks.front();
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
        safe.setQuery({});
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

void MainWindow::Impl::exportTasks() {
    auto path = QFileDialog::getSaveFileName(
        window, t("Export task list — contains original URLs", "导出任务列表（包含原始下载地址）"),
        "downloads.json", "JSON (*.json)");
    if (path.isEmpty())
        return;
    QJsonArray array;
    for (const auto& task : all)
        array.append(QJsonObject{{"url", qs(task.spec.url)},
                                 {"destination", pathString(task.spec.destination)},
                                 {"category", qs(task.spec.category)},
                                 {"connections", task.spec.connections},
                                 {"media", task.spec.media},
                                 {"audioOnly", task.spec.audioOnly},
                                 {"mediaFormat", qs(task.spec.mediaFormat)},
                                 {"sha256", qs(task.spec.expectedSha256)}});
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(QJsonObject{{"version", 1}, {"downloads", array}}).toJson()) < 0 ||
        !file.commit())
        QMessageBox::warning(window, t("Export failed", "导出失败"), file.errorString());
}

void MainWindow::Impl::importTasks() {
    auto path =
        QFileDialog::getOpenFileName(window, t("Import task list", "导入任务列表"), {}, "JSON (*.json)");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 5 * 1024 * 1024) {
        QMessageBox::warning(window, "MaskPilot",
                             t("Cannot read this file (maximum 5 MiB).", "无法读取文件（最大 5 MiB）。"));
        return;
    }
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    auto object = doc.object();
    auto array = object.value("downloads").toArray();
    std::vector<DownloadSpec> specs;
    if (error.error != QJsonParseError::NoError || object.value("version").toInt() != 1 ||
        !object.value("downloads").isArray() || array.size() > 10000) {
        QMessageBox::warning(
            window, "MaskPilot",
            t("Invalid task list. Existing tasks were preserved.", "任务列表无效，现有任务未修改。"));
        return;
    }
    for (const auto& entry : array) {
        auto value = entry.toObject();
        auto spec = window->defaultSpec(value.value("url").toString());
        spec.destination = std::filesystem::path(value.value("destination").toString().toStdWString());
        spec.category = value.value("category").toString("General").toStdString();
        spec.connections = value.value("connections").toInt(4);
        spec.proxy = settings.value("proxy").toString().toStdString();
        spec.expectedSha256 = value.value("sha256").toString().toStdString();
        const bool invalidMediaFields =
            (value.contains("media") && !value.value("media").isBool()) ||
            (value.contains("audioOnly") && !value.value("audioOnly").isBool()) ||
            (value.contains("mediaFormat") && !value.value("mediaFormat").isString());
        spec.media = value.value("media").toBool();
        spec.audioOnly = value.value("audioOnly").toBool();
        spec.mediaFormat = value.value("mediaFormat").toString(qs(spec.mediaFormat)).toStdString();
        spec.startPaused = true;
        if (invalidMediaFields || !validateSpec(spec).empty()) {
            QMessageBox::warning(window, "MaskPilot",
                                 t("Task list validation failed. Nothing was imported.",
                                   "任务列表校验失败，未导入任何任务。"));
            return;
        }
        specs.push_back(std::move(spec));
    }
    for (auto& spec : specs)
        window->enqueue(std::move(spec));
    notify("MaskPilot", t("Imported tasks are paused for review.", "导入任务已暂停，可检查后开始。"));
}

void MainWindow::Impl::batch() {
    QDialog dialog(window);
    dialog.resize(650, 510);
    dialog.setWindowTitle(t("Batch downloads", "批量下载"));
    auto* layout = new QVBoxLayout(&dialog);
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
                auto matches = QRegularExpression(R"(https?://[^\s<>"']+)").globalMatch(line);
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
        for (int i = 0; i < output->count(); ++i) {
            const auto url = output->item(i)->text();
            auto spec = window->defaultSpec(url);
            spec.startPaused = true;
            window->enqueue(std::move(spec));
        }
        dialog.accept();
    });
    dialog.exec();
}
} // namespace maskpilot
