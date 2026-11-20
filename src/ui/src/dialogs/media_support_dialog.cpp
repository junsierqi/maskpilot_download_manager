#include "../windows/main_window_p.h"
namespace maskpilot {
void MainWindow::Impl::mediaSupport() {
    auto* dialog = new QDialog(window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(t("Media support", "媒体支持"));
    dialog->resize(630, 300);
    auto* layout = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto addPath = [&](const QString& label, const QString& key) {
        auto* row = new QWidget;
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto* field = new QLineEdit(settings.value(key).toString());
        line->addWidget(field);
        auto* browse = new QPushButton(t("Locate…", "选择…"));
        line->addWidget(browse);
        QObject::connect(browse, &QPushButton::clicked, dialog, [dialog, field] {
            auto path = QFileDialog::getOpenFileName(dialog, "MaskPilot", {},
#ifdef _WIN32
                                                     "Executable (*.exe)"
#else
                                                    "Executable (*)"
#endif
            );
            if (!path.isEmpty())
                field->setText(path);
        });
        form->addRow(label, row);
        return QPointer<QLineEdit>(field);
    };
    auto parser = addPath(t("Media parser", "媒体解析组件"), "mediaDownloader");
    auto muxer = addPath(t("Media processor", "媒体处理组件"), "mediaMuxer");
    auto* information = new QLabel(
        t("Locate existing components, or download the supported versions from their distributors. "
          "Downloads are verified before use and appear in the task list.",
          "可选择已安装组件，或从组件分发源下载支持的固定版本。下载完成后会校验，进度显示在任务列表中。"));
    information->setWordWrap(true);
    layout->addWidget(information);
    auto label = QPointer<QLabel>(information);
    auto* buttons = new QDialogButtonBox;
    auto* save = buttons->addButton(t("Save paths", "保存路径"), QDialogButtonBox::ActionRole);
    auto* install =
        buttons->addButton(t("Download components", "下载支持组件"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(save, &QPushButton::clicked, dialog, [this, parser, muxer, label] {
        if (!QFileInfo(parser->text()).isExecutable() || !QFileInfo(muxer->text()).isExecutable()) {
            label->setText(t("Select both executable files.", "请选择两个可执行组件文件。"));
            return;
        }
        settings.setValue("mediaDownloader", parser->text());
        settings.setValue("mediaMuxer", muxer->text());
        label->setText(t("Media support is ready.", "媒体支持已就绪。"));
    });
#ifndef _WIN32
    install->hide();
    information->setText(t("Locate trusted native components installed through your system package manager. "
                           "Native live recording does not require these components.",
                           "请选择通过系统包管理器安装的可信媒体组件。原生直播录制无需这些组件。"));
#else
    QObject::connect(install, &QPushButton::clicked, dialog, [this, parser, muxer, label] {
        if (installingMedia) {
            if (label)
                label->setText(
                    t("Components are already downloading in the task list.", "组件已在任务列表中下载。"));
            return;
        }
        const bool needParser = !QFileInfo(parser->text()).isExecutable(),
                   needMuxer = !QFileInfo(muxer->text()).isExecutable();
        if (!needParser)
            settings.setValue("mediaDownloader", parser->text());
        if (!needMuxer)
            settings.setValue("mediaMuxer", muxer->text());
        if (!needParser && !needMuxer) {
            label->setText(t("Media support is ready.", "媒体支持已就绪。"));
            return;
        }
        installingMedia = true;
        const auto tools = QDir(QFileInfo(settings.fileName()).absolutePath()).filePath("tools");
        QDir().mkpath(tools);
        struct InstallState {
            std::map<QString, QString> ids;
            int acknowledgements = 0, expected = 0;
            bool failed = false;
        };
        auto state = std::make_shared<InstallState>();
        auto add = [&, state](const QString& kind, const QString& url, const QString& filename,
                              const std::string& sha) {
            auto spec = window->defaultSpec(url);
            spec.destination = std::filesystem::path(QDir(tools).filePath(filename).toStdU16String());
            spec.expectedSha256 = sha;
            spec.category = "Components";
            ++state->expected;
            window->enqueue(std::move(spec), {}, [state, kind, label](QString id, QString error) {
                ++state->acknowledgements;
                if (!error.isEmpty()) {
                    state->failed = true;
                    if (label)
                        label->setText(error);
                } else
                    state->ids[kind] = id;
            });
        };
        if (needParser)
            add("parser", "https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.26/yt-dlp.exe",
                "parser-2026.10.26/yt-dlp.exe",
                "f5389d3698ecde4ef06f4e6b1ce0294fe9ac1396228c8990b8d3416d6d177a7d");
        if (needMuxer)
            add("muxer",
                "https://github.com/GyanD/codexffmpeg/releases/download/8.1.1/"
                "ffmpeg-8.1.1-full_build.zip",
                "ffmpeg-8.1.1-full_build.zip",
                "49b28c5f16addd40239a66949973458769b7056fb7752c30ac0d53389d09a552");
        auto* poll = new QTimer(window);
        QObject::connect(
            poll, &QTimer::timeout, window, [this, poll, state, label, parser, muxer, tools, needMuxer] {
                if (state->acknowledgements < state->expected)
                    return;
                if (state->failed) {
                    installingMedia = false;
                    poll->deleteLater();
                    return;
                }
                auto tasks = service.snapshot();
                std::map<QString, Task> completed;
                for (const auto& [kind, id] : state->ids) {
                    auto it = std::find_if(tasks.begin(), tasks.end(),
                                           [&](const auto& task) { return qs(task.id) == id; });
                    if (it == tasks.end())
                        return;
                    if (it->state == TaskState::Failed || it->state == TaskState::NeedsAttention ||
                        it->state == TaskState::Cancelled) {
                        if (label)
                            label->setText(qs(it->error));
                        installingMedia = false;
                        poll->deleteLater();
                        return;
                    }
                    if (it->state != TaskState::Completed) {
                        if (label)
                            label->setText(
                                t("Downloading components in the task list…", "正在任务列表中下载组件…"));
                        return;
                    }
                    completed[kind] = *it;
                }
                if (completed.contains("parser")) {
                    const auto path = pathString(completed.at("parser").spec.destination);
                    settings.setValue("mediaDownloader", path);
                    if (parser)
                        parser->setText(path);
                }
                poll->deleteLater();
                if (!needMuxer) {
                    installingMedia = false;
                    if (label)
                        label->setText(t("Media support is ready.", "媒体支持已就绪。"));
                    notify("MaskPilot", t("Media support installed.", "媒体支持安装完成。"));
                    return;
                }
                const auto extraction = QDir(tools).filePath("processor-8.1.1-" + qs(newTaskId()));
                QDir().mkpath(extraction);
                auto* unpack = new QProcess(window);
                unpack->setProgram(systemExecutable(L"tar.exe"));
                unpack->setArguments(
                    {"-xf", pathString(completed.at("muxer").spec.destination), "-C", extraction});
                if (label)
                    label->setText(t("Installing verified components…", "正在安装已校验的组件…"));
                QObject::connect(
                    unpack, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), window,
                    [this, unpack, extraction, label, muxer](int code, QProcess::ExitStatus status) {
                        installingMedia = false;
                        const auto path = QDir(extraction).filePath("ffmpeg-8.1.1-full_build/bin/ffmpeg.exe");
                        if (code == 0 && status == QProcess::NormalExit && QFileInfo(path).isExecutable()) {
                            settings.setValue("mediaMuxer", path);
                            if (muxer)
                                muxer->setText(path);
                            if (label)
                                label->setText(t("Media support is ready.", "媒体支持已就绪。"));
                            notify("MaskPilot", t("Media support installed.", "媒体支持安装完成。"));
                        } else if (label)
                            label->setText(t("Extraction failed. Select an installed component or retry.",
                                             "解压失败，请选择已安装组件或重试。"));
                        unpack->deleteLater();
                    });
                QObject::connect(unpack, &QProcess::errorOccurred, window,
                                 [this, unpack, label](QProcess::ProcessError) {
                                     installingMedia = false;
                                     if (label)
                                         label->setText(t("Could not start the component extractor.",
                                                          "无法启动组件解压工具。"));
                                     unpack->deleteLater();
                                 });
                unpack->start();
            });
        poll->start(500);
    });
#endif
    dialog->show();
}
} // namespace maskpilot
