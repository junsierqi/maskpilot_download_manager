#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <maskpilot/ui/add_download_dialog.h>

namespace maskpilot {
namespace {
std::filesystem::path nativePath(const QString& s) { return std::filesystem::path(s.toStdWString()); }
QString safeName(const QString& url) {
    auto name = QFileInfo(QUrl(url).path()).fileName();
    name.replace(QRegularExpression(R"([<>:"/\\|?*\x00-\x1f])"), "_");
    while (name.endsWith('.') || name.endsWith(' '))
        name.chop(1);
    if (name.isEmpty() || name == "." || name == "..")
        name = "download.bin";
    if (QRegularExpression(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$))",
                           QRegularExpression::CaseInsensitiveOption)
            .match(name)
            .hasMatch())
        name.prepend('_');
    return name.left(180);
}
} // namespace
struct AddDownloadDialog::Impl {
    QLineEdit *url, *destination, *referrer, *agent, *cookie, *authorization, *proxy, *checksum;
    QComboBox* category;
    QSpinBox *connections, *speed, *priority;
    QCheckBox *paused, *schedule, *stop;
    QDateTimeEdit *startTime, *stopTime;
    QPlainTextEdit* headers;
    QCheckBox* media;
    QComboBox *mode, *quality;
    QLineEdit *downloader, *muxer;
};
AddDownloadDialog::AddDownloadDialog(bool chinese, QWidget* parent, QString initialUrl)
    : QDialog(parent), impl_(std::make_unique<Impl>()) {
    auto t = [chinese](const char* en, const char* zh) { return QString::fromUtf8(chinese ? zh : en); };
    setWindowTitle(t("New download", "新建下载"));
    resize(660, 510);
    setMinimumWidth(560);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(16);
    auto* heading = new QLabel(t("Add a download", "添加一个下载任务"));
    heading->setStyleSheet("font-size:21px;font-weight:600;");
    layout->addWidget(heading);
    auto* tabs = new QTabWidget;
    layout->addWidget(tabs);
    auto* basic = new QWidget;
    auto* form = new QFormLayout(basic);
    form->setSpacing(14);
    impl_->url = new QLineEdit(initialUrl);
    impl_->url->setObjectName("downloadUrl");
    impl_->url->setPlaceholderText("https://example.com/file.zip");
    form->addRow(t("URL", "下载地址"), impl_->url);
    QSettings settings;
    const auto folder =
        settings
            .value("downloadDirectory", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
            .toString();
    impl_->destination = new QLineEdit(QDir(folder).filePath(safeName(initialUrl)));
    impl_->destination->setObjectName("downloadDestination");
    auto* pathRow = new QWidget;
    auto* pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->addWidget(impl_->destination);
    auto* browse = new QPushButton(t("Browse…", "浏览…"));
    pathLayout->addWidget(browse);
    form->addRow(t("Save as", "保存为"), pathRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getSaveFileName(this, windowTitle(), impl_->destination->text());
        if (!path.isEmpty())
            impl_->destination->setText(path);
    });
    connect(impl_->url, &QLineEdit::textEdited, this, [this] {
        auto path = QFileInfo(impl_->destination->text()).absolutePath();
        impl_->destination->setText(QDir(path).filePath(safeName(impl_->url->text())));
    });
    impl_->category = new QComboBox;
    impl_->category->setEditable(true);
    impl_->category->addItems({"General", "Documents", "Software", "Video", "Music", "Archives"});
    const auto extension = QFileInfo(safeName(initialUrl)).suffix().toLower();
    const auto rules =
        settings
            .value("categoryRules",
                   "Documents=pdf,doc,docx,xls,xlsx,ppt,pptx,txt\nSoftware=exe,msi,msix\nVideo=mp4,mkv,webm,"
                   "mov\nMusic=mp3,flac,wav,m4a\nArchives=zip,7z,rar,tar,gz")
            .toString();
    for (const auto& rule : rules.split('\n', Qt::SkipEmptyParts)) {
        auto equal = rule.indexOf('=');
        if (equal > 0 && rule.mid(equal + 1).toLower().split(',', Qt::SkipEmptyParts).contains(extension))
            impl_->category->setCurrentText(rule.left(equal).trimmed());
    }
    auto applyCategoryFolder = [this, folder] {
        QSettings current;
        if (!current.value("categorySubfolders", false).toBool() || impl_->destination->isModified())
            return;
        auto name = impl_->category->currentText();
        name.replace(QRegularExpression(R"([<>:"/\\|?*\x00-\x1f])"), "_");
        if (name == "." || name == ".." || name.isEmpty())
            name = "General";
        impl_->destination->setText(QDir(QDir(folder).filePath(name)).filePath(safeName(impl_->url->text())));
    };
    connect(impl_->category, &QComboBox::currentTextChanged, this,
            [applyCategoryFolder](const QString&) { applyCategoryFolder(); });
    applyCategoryFolder();
    form->addRow(t("Category", "分类"), impl_->category);
    impl_->connections = new QSpinBox;
    impl_->connections->setRange(1, 8);
    impl_->connections->setValue(settings.value("connections", 4).toInt());
    form->addRow(t("Connections", "连接数"), impl_->connections);
    impl_->speed = new QSpinBox;
    impl_->speed->setRange(0, 1000000);
    impl_->speed->setSuffix(" KiB/s");
    impl_->speed->setSpecialValueText(t("Unlimited", "不限速"));
    form->addRow(t("Speed limit", "任务限速"), impl_->speed);
    impl_->paused = new QCheckBox(t("Add paused", "添加后暂停"));
    impl_->paused->setObjectName("addPaused");
    form->addRow(impl_->paused);
    tabs->addTab(basic, t("Download", "下载"));
    auto* planning = new QWidget;
    auto* plan = new QFormLayout(planning);
    plan->setSpacing(14);
    impl_->priority = new QSpinBox;
    impl_->priority->setRange(-10, 10);
    plan->addRow(t("Priority", "优先级"), impl_->priority);
    impl_->schedule = new QCheckBox(t("Start at a specific time", "定时开始"));
    plan->addRow(impl_->schedule);
    impl_->startTime = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(3600));
    impl_->startTime->setCalendarPopup(true);
    impl_->startTime->setDisplayFormat("yyyy-MM-dd HH:mm:ss t");
    impl_->startTime->setEnabled(false);
    plan->addRow(t("Start", "开始时间"), impl_->startTime);
    connect(impl_->schedule, &QCheckBox::toggled, impl_->startTime, &QWidget::setEnabled);
    impl_->stop = new QCheckBox(t("Pause at a specific time", "定时暂停"));
    plan->addRow(impl_->stop);
    impl_->stopTime = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(7200));
    impl_->stopTime->setCalendarPopup(true);
    impl_->stopTime->setDisplayFormat("yyyy-MM-dd HH:mm:ss t");
    impl_->stopTime->setEnabled(false);
    plan->addRow(t("Stop", "暂停时间"), impl_->stopTime);
    connect(impl_->stop, &QCheckBox::toggled, impl_->stopTime, &QWidget::setEnabled);
    tabs->addTab(planning, t("Queue & schedule", "队列与计划"));
    auto* advanced = new QWidget;
    auto* options = new QFormLayout(advanced);
    options->setSpacing(10);
    auto line = [&](QLineEdit*& field, const QString& label) {
        field = new QLineEdit;
        options->addRow(label, field);
    };
    line(impl_->referrer, t("Referrer", "来源页面"));
    line(impl_->agent, t("User agent", "用户代理"));
    impl_->agent->setText("MaskPilot/0.1");
    line(impl_->proxy, t("Proxy URL", "代理地址"));
    impl_->proxy->setText(settings.value("proxy").toString());
    impl_->proxy->setPlaceholderText("http://127.0.0.1:7890");
    line(impl_->cookie, "Cookie");
    impl_->cookie->setEchoMode(QLineEdit::Password);
    line(impl_->authorization, t("Authorization", "认证头"));
    impl_->authorization->setEchoMode(QLineEdit::Password);
    line(impl_->checksum, "SHA-256");
    impl_->checksum->setPlaceholderText(t("Optional expected checksum", "可选：预期文件摘要"));
    impl_->headers = new QPlainTextEdit;
    impl_->headers->setPlaceholderText("X-Header: value");
    impl_->headers->setMaximumHeight(70);
    options->addRow(t("Extra headers", "附加请求头"), impl_->headers);
    tabs->addTab(advanced, t("Advanced", "高级"));
    auto* mediaTab = new QWidget;
    auto* mediaForm = new QFormLayout(mediaTab);
    mediaForm->setSpacing(12);
    impl_->media =
        new QCheckBox(t("Download webpage video, audio, HLS or DASH", "下载网页视频、音频、HLS 或 DASH"));
    impl_->media->setObjectName("mediaDownload");
    mediaForm->addRow(impl_->media);
    impl_->mode = new QComboBox;
    impl_->mode->addItems({t("Video (MKV)", "视频（MKV）"), t("Audio only (M4A)", "仅音频（M4A）")});
    mediaForm->addRow(t("Output", "输出类型"), impl_->mode);
    impl_->quality = new QComboBox;
    impl_->quality->addItem(t("Best available", "最佳画质"), "bestvideo+bestaudio/best");
    for (int height : {2160, 1440, 1080, 720, 480})
        impl_->quality->addItem(QString::number(height) + "p",
                                QString("bestvideo[height<=%1]+bestaudio/best[height<=%1]").arg(height));
    mediaForm->addRow(t("Quality", "画质上限"), impl_->quality);
    auto tool = [&](QLineEdit*& field, const QString& label, const QString& key, const QString& program) {
        auto* row = new QWidget;
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        field = new QLineEdit(settings.value(key, QStandardPaths::findExecutable(program)).toString());
        line->addWidget(field);
        auto* pick = new QPushButton(t("Locate…", "选择…"));
        line->addWidget(pick);
        connect(pick, &QPushButton::clicked, this, [this, field] {
            auto file = QFileDialog::getOpenFileName(this, windowTitle(), {}, "Executable (*.exe)");
            if (!file.isEmpty())
                field->setText(file);
        });
        mediaForm->addRow(label, row);
    };
    tool(impl_->downloader, t("Media parser", "媒体解析组件"), "mediaDownloader", "yt-dlp");
    tool(impl_->muxer, t("Media processor", "媒体处理组件"), "mediaMuxer", "ffmpeg");
    auto* note = new QLabel(
        t("Missing components? Open Tools → Media support. Media uses public URLs; resuming or changing "
          "bandwidth restarts media safely. Direct files support authenticated resumable transfers.",
          "缺少组件时，可从“工具 → "
          "媒体支持”安装。媒体模式使用公开地址，继续任务或调整限速时会安全地重新下载；直接文件支持登录信息与"
          "断点续传。"));
    note->setWordWrap(true);
    mediaForm->addRow(note);
    tabs->addTab(mediaTab, t("Media", "媒体"));
    auto updateMedia = [this] {
        if (!impl_->media->isChecked())
            return;
        auto path = QFileInfo(impl_->destination->text());
        impl_->destination->setText(
            QDir(path.absolutePath())
                .filePath(path.completeBaseName() + (impl_->mode->currentIndex() == 1 ? ".m4a" : ".mkv")));
        if (impl_->category->currentText() == "General")
            impl_->category->setCurrentText(impl_->mode->currentIndex() == 1 ? "Music" : "Video");
    };
    connect(impl_->media, &QCheckBox::toggled, this, [updateMedia](bool) { updateMedia(); });
    connect(impl_->mode, &QComboBox::currentIndexChanged, this, [updateMedia](int) { updateMedia(); });
    auto* buttons = new QDialogButtonBox;
    auto* add = buttons->addButton(t("Add download", "添加下载"), QDialogButtonBox::AcceptRole);
    add->setDefault(true);
    add->setObjectName("confirmDownload");
    buttons->addButton(t("Cancel", "取消"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, t] {
        auto spec = specification();
        auto error = validateSpec(spec);
        if (impl_->stop->isChecked() && spec.stopAt <= std::max<std::int64_t>(unixTime(), spec.scheduledAt))
            error = "Stop time must be after the start time.";
        if (!error.empty()) {
            QMessageBox::warning(this, t("Check download", "请检查下载信息"), QString::fromStdString(error));
            return;
        }
        if (spec.media && (!QFileInfo(impl_->downloader->text()).isExecutable() ||
                           !QFileInfo(impl_->muxer->text()).isExecutable())) {
            QMessageBox::warning(this, t("Media support", "媒体支持"),
                                 t("Locate both components, or install them from Tools → Media support.",
                                   "请选择两个媒体组件，或从“工具 → 媒体支持”安装。"));
            return;
        }
        if (spec.media) {
            QSettings current;
            current.setValue("mediaDownloader", impl_->downloader->text());
            current.setValue("mediaMuxer", impl_->muxer->text());
        }
        accept();
    });
    impl_->url->setFocus();
}
AddDownloadDialog::~AddDownloadDialog() = default;
DownloadSpec AddDownloadDialog::specification() const {
    DownloadSpec s;
    s.url = impl_->url->text().trimmed().toStdString();
    s.destination = nativePath(impl_->destination->text().trimmed());
    s.category = impl_->category->currentText().trimmed().toStdString();
    s.connections = impl_->connections->value();
    s.speedLimit = static_cast<std::int64_t>(impl_->speed->value()) * 1024;
    s.priority = impl_->priority->value();
    s.startPaused = impl_->paused->isChecked();
    s.scheduledAt = impl_->schedule->isChecked() ? impl_->startTime->dateTime().toSecsSinceEpoch() : 0;
    s.stopAt = impl_->stop->isChecked() ? impl_->stopTime->dateTime().toSecsSinceEpoch() : 0;
    s.referrer = impl_->referrer->text().toStdString();
    s.userAgent = impl_->agent->text().toStdString();
    s.proxy = impl_->proxy->text().toStdString();
    s.cookie = impl_->cookie->text().toStdString();
    s.authorization = impl_->authorization->text().toStdString();
    s.expectedSha256 = impl_->checksum->text().trimmed().toStdString();
    s.media = impl_->media->isChecked();
    s.audioOnly = impl_->mode->currentIndex() == 1;
    s.mediaFormat = impl_->quality->currentData().toString().toStdString();
    s.mediaDownloader = nativePath(impl_->downloader->text());
    s.mediaMuxer = nativePath(impl_->muxer->text());
    for (const auto& line : impl_->headers->toPlainText().split('\n', Qt::SkipEmptyParts)) {
        auto colon = line.indexOf(':');
        if (colon <= 0) {
            s.headers.emplace_back("Invalid header name", "");
            continue;
        }
        s.headers.emplace_back(line.left(colon).trimmed().toStdString(),
                               line.mid(colon + 1).trimmed().toStdString());
    }
    return s;
}
void AddDownloadDialog::setSpecification(const DownloadSpec& spec) {
    impl_->url->setText(QString::fromStdString(spec.url));
    impl_->category->setCurrentText(QString::fromStdString(spec.category));
    impl_->connections->setValue(spec.connections);
    impl_->speed->setValue(static_cast<int>(std::min<std::int64_t>(1000000, spec.speedLimit / 1024)));
    impl_->priority->setValue(spec.priority);
    impl_->paused->setChecked(spec.startPaused);
    impl_->schedule->setChecked(spec.scheduledAt > 0);
    impl_->stop->setChecked(spec.stopAt > 0);
    if (spec.scheduledAt > 0)
        impl_->startTime->setDateTime(QDateTime::fromSecsSinceEpoch(spec.scheduledAt));
    if (spec.stopAt > 0)
        impl_->stopTime->setDateTime(QDateTime::fromSecsSinceEpoch(spec.stopAt));
    impl_->referrer->setText(QString::fromStdString(spec.referrer));
    impl_->agent->setText(QString::fromStdString(spec.userAgent));
    impl_->proxy->setText(QString::fromStdString(spec.proxy));
    impl_->cookie->setText(QString::fromStdString(spec.cookie));
    impl_->authorization->setText(QString::fromStdString(spec.authorization));
    impl_->checksum->setText(QString::fromStdString(spec.expectedSha256));
    QStringList headers;
    for (const auto& [name, value] : spec.headers)
        headers << QString::fromStdString(name + ": " + value);
    impl_->headers->setPlainText(headers.join('\n'));
    impl_->media->setChecked(spec.media);
    impl_->mode->setCurrentIndex(spec.audioOnly ? 1 : 0);
    auto quality = impl_->quality->findData(QString::fromStdString(spec.mediaFormat));
    if (quality >= 0)
        impl_->quality->setCurrentIndex(quality);
    if (!spec.mediaDownloader.empty())
        impl_->downloader->setText(QString::fromStdWString(spec.mediaDownloader.wstring()));
    if (!spec.mediaMuxer.empty())
        impl_->muxer->setText(QString::fromStdWString(spec.mediaMuxer.wstring()));
    impl_->destination->setText(QString::fromStdWString(spec.destination.wstring()));
}
} // namespace maskpilot
