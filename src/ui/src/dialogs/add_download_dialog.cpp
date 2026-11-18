#include "download_name.h"
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
#include <QUrlQuery>
#include <QVBoxLayout>
#include <maskpilot/ui/add_download_dialog.h>

namespace maskpilot {
namespace {
std::filesystem::path nativePath(const QString& s) { return std::filesystem::path(s.toStdWString()); }
QString safeName(const QString& url) { return ui::defaultDownloadName(url); }
} // namespace
struct AddDownloadDialog::Impl {
    QLineEdit *url, *destination, *referrer, *agent, *cookie, *authorization, *proxy, *checksum;
    QLineEdit *proxyUsername, *proxyPassword;
    QCheckBox* proxyAuthenticationRequired;
    QCheckBox *originAuthenticationRequired = nullptr, *rememberProxy = nullptr;
    QLabel* storageNotice = nullptr;
    bool storedProxy = false;
    bool chinese = false;
    QString storedProxyEndpoint;
    QComboBox* category;
    QComboBox* queue;
    QSpinBox *connections, *speed, *priority;
    QCheckBox *paused, *schedule, *stop;
    QDateTimeEdit *startTime, *stopTime;
    QPlainTextEdit* headers;
    QCheckBox* media;
    QComboBox *mode, *quality;
    QLineEdit *downloader, *muxer;
    QLineEdit *ftpUsername, *ftpPassword, *ftpCa;
    QCheckBox *ftpTls, *ftpActive;
    QCheckBox *torrentAll, *torrentSeed;
    QSpinBox* torrentUpload;
    TorrentMetadata torrentMetadata;
    std::vector<int> torrentSelection;
    std::string torrentSource;
    void updateStorage() {
        if (!rememberProxy)
            return;
        const bool supplied = !proxyUsername->text().isEmpty() || !proxyPassword->text().isEmpty();
        const bool retained = storedProxy && proxy->text().trimmed() == storedProxyEndpoint;
        if (storageNotice)
            storageNotice->setText(
                chinese
                    ? (retained ? QStringLiteral(
                                      "已安全保存代理凭据。留空会保留当前配置；输入新凭据后可选择是否保存。")
                                : QStringLiteral("默认不保存代理凭据，重启后需重新输入。SOCKS5 "
                                                 "由代理解析地址；BT 代理使用 TCP。"))
                    : (retained ? QStringLiteral("Proxy credentials are saved. Leave fields empty to keep "
                                                 "them, or enter replacements and choose whether to save.")
                                : QStringLiteral(
                                      "Saving proxy credentials is optional. Otherwise reenter them after "
                                      "restart. SOCKS5 resolves remotely; proxied BT uses TCP.")));
        if (retained && !supplied) {
            rememberProxy->setChecked(true);
            rememberProxy->setEnabled(false);
        } else {
            rememberProxy->setEnabled(supplied && !proxy->text().trimmed().isEmpty());
            if (!supplied)
                rememberProxy->setChecked(false);
        }
    }
};
AddDownloadDialog::AddDownloadDialog(bool chinese, QWidget* parent, QString initialUrl)
    : QDialog(parent), impl_(std::make_unique<Impl>()) {
    impl_->chinese = chinese;
    auto t = [chinese](const char* en, const char* zh) { return QString::fromUtf8(chinese ? zh : en); };
    setWindowTitle(t("New download", "新建下载"));
    resize(680, 610);
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
    impl_->url->setPlaceholderText("https://example.com/file.zip / ftp://example.com/file.zip");
    auto* sourceRow = new QWidget;
    auto* sourceLayout = new QHBoxLayout(sourceRow);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->addWidget(impl_->url);
    auto* torrentFile = new QPushButton(t("Torrent…", "种子文件…"));
    torrentFile->setObjectName("chooseTorrentFile");
    sourceLayout->addWidget(torrentFile);
    form->addRow(t("Source", "下载来源"), sourceRow);
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
        const bool torrent = isTorrentUrl(impl_->url->text().toStdString());
        auto path = torrent ? QFileDialog::getExistingDirectory(
                                  this, windowTitle(), QFileInfo(impl_->destination->text()).absolutePath())
                            : QFileDialog::getSaveFileName(this, windowTitle(), impl_->destination->text());
        if (torrent && !path.isEmpty())
            path = QDir(path).filePath(safeName(impl_->url->text()));
        if (!path.isEmpty())
            impl_->destination->setText(path);
    });
    connect(torrentFile, &QPushButton::clicked, this, [this, t] {
        const auto file =
            QFileDialog::getOpenFileName(this, t("Open torrent", "打开种子"), {}, "Torrent (*.torrent)");
        if (file.isEmpty())
            return;
        impl_->url->setText("torrent:" + QDir::fromNativeSeparators(file));
        const auto parent = QFileInfo(impl_->destination->text()).absolutePath();
        impl_->destination->setText(QDir(parent).filePath(safeName(impl_->url->text())));
    });
    connect(impl_->url, &QLineEdit::textEdited, this, [this] {
        const bool required = requiresOriginAuthentication(specification());
        impl_->cookie->clear();
        impl_->authorization->clear();
        impl_->headers->clear();
        impl_->ftpUsername->clear();
        impl_->ftpPassword->clear();
        impl_->referrer->clear();
        impl_->originAuthenticationRequired->setChecked(required);
        auto path = QFileInfo(impl_->destination->text()).absolutePath();
        impl_->destination->setText(QDir(path).filePath(safeName(impl_->url->text())));
    });
    impl_->category = new QComboBox;
    impl_->queue = new QComboBox;
    impl_->queue->setObjectName("downloadQueue");
    impl_->queue->addItem(t("Default", "默认队列"), defaultQueueId);
    form->addRow(t("Queue", "下载队列"), impl_->queue);
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
    form->addRow(t("HTTP connections", "HTTP 连接数"), impl_->connections);
    impl_->speed = new QSpinBox;
    impl_->speed->setRange(0, 1000000);
    impl_->speed->setSuffix(" KiB/s");
    impl_->speed->setSpecialValueText(t("Unlimited", "不限速"));
    form->addRow(t("Speed limit", "任务限速"), impl_->speed);
    impl_->paused = new QCheckBox(t("Add paused", "添加后暂停"));
    impl_->paused->setObjectName("addPaused");
    form->addRow(impl_->paused);
    impl_->originAuthenticationRequired =
        new QCheckBox(t("Require source sign-in details before starting", "开始任务前必须提供来源登录信息"));
    impl_->originAuthenticationRequired->setObjectName("originAuthenticationRequired");
    impl_->originAuthenticationRequired->setToolTip(t(
        "Source credentials stay in memory. Clear this only when the resource can be downloaded anonymously.",
        "来源凭据仅在当前会话有效；只有来源允许匿名下载时才取消此要求。"));
    form->addRow(impl_->originAuthenticationRequired);
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
    impl_->agent->setText(QString::fromLatin1(defaultUserAgent));
    line(impl_->cookie, "Cookie");
    impl_->cookie->setObjectName("requestCookie");
    impl_->cookie->setEchoMode(QLineEdit::Password);
    line(impl_->authorization, t("Authorization", "认证头"));
    impl_->authorization->setObjectName("requestAuthorization");
    impl_->authorization->setEchoMode(QLineEdit::Password);
    line(impl_->checksum, "SHA-256");
    impl_->checksum->setPlaceholderText(t("Optional expected checksum", "可选：预期文件摘要"));
    impl_->headers = new QPlainTextEdit;
    impl_->headers->setPlaceholderText("X-Header: value");
    impl_->headers->setMaximumHeight(70);
    options->addRow(t("Extra headers", "附加请求头"), impl_->headers);
    tabs->addTab(advanced, t("Advanced", "高级"));
    auto* proxyTab = new QWidget;
    auto* proxyForm = new QFormLayout(proxyTab);
    proxyForm->setSpacing(12);
    impl_->proxy = new QLineEdit(settings.value("proxy").toString());
    impl_->proxy->setObjectName("proxyEndpoint");
    impl_->proxy->setPlaceholderText(
        t("Empty: direct; http:// or socks5://host:port", "留空直连；http:// 或 socks5://主机:端口"));
    proxyForm->addRow(t("Proxy URL", "代理地址"), impl_->proxy);
    impl_->proxyUsername = new QLineEdit;
    impl_->proxyUsername->setObjectName("proxyUsername");
    impl_->proxyPassword = new QLineEdit;
    impl_->proxyPassword->setObjectName("proxyPassword");
    impl_->proxyPassword->setEchoMode(QLineEdit::Password);
    proxyForm->addRow(t("Proxy username", "代理用户名"), impl_->proxyUsername);
    proxyForm->addRow(t("Proxy password", "代理密码"), impl_->proxyPassword);
    impl_->proxyAuthenticationRequired =
        new QCheckBox(t("Require credentials before this task can start", "开始任务前必须提供代理凭据"));
    impl_->proxyAuthenticationRequired->setObjectName("proxyAuthenticationRequired");
    proxyForm->addRow(impl_->proxyAuthenticationRequired);
    const auto updateCredentialRequirement = [this] {
        const bool supplied =
            !impl_->proxyUsername->text().isEmpty() || !impl_->proxyPassword->text().isEmpty();
        if (supplied)
            impl_->proxyAuthenticationRequired->setChecked(true);
        impl_->proxyAuthenticationRequired->setEnabled(!supplied);
        impl_->updateStorage();
    };
    connect(impl_->proxyUsername, &QLineEdit::textChanged, this, updateCredentialRequirement);
    connect(impl_->proxyPassword, &QLineEdit::textChanged, this, updateCredentialRequirement);
    impl_->rememberProxy =
        new QCheckBox(t("Save proxy credentials securely in Windows", "由 Windows 安全保存代理凭据"));
    impl_->rememberProxy->setObjectName("rememberProxyCredentials");
    impl_->rememberProxy->setEnabled(false);
    proxyForm->addRow(impl_->rememberProxy);
    impl_->storageNotice = new QLabel(
        t("Saving is optional. Otherwise, reenter credentials after restarting. SOCKS5 resolves destinations "
          "through the proxy. Proxied BT uses TCP; HTTPS proxies are supported for direct files only.",
          "默认不保存凭据，重启后需重新输入。SOCKS5 由代理解析目标地址。BT 代理使用 TCP；"
          "HTTPS 代理仅支持直接文件下载。"));
    impl_->storageNotice->setTextFormat(Qt::PlainText);
    impl_->storageNotice->setWordWrap(true);
    proxyForm->addRow(impl_->storageNotice);
    connect(impl_->proxy, &QLineEdit::textEdited, this, [this] {
        impl_->proxyUsername->clear();
        impl_->proxyPassword->clear();
        impl_->storedProxy = false;
        impl_->rememberProxy->setChecked(false);
        if (impl_->proxy->text().trimmed().isEmpty())
            impl_->proxyAuthenticationRequired->setChecked(false);
        impl_->updateStorage();
    });
    tabs->addTab(proxyTab, t("Proxy", "代理"));
    auto* ftpTab = new QWidget;
    auto* ftpForm = new QFormLayout(ftpTab);
    ftpForm->setSpacing(12);
    impl_->ftpUsername = new QLineEdit;
    impl_->ftpUsername->setObjectName("ftpUsername");
    impl_->ftpUsername->setPlaceholderText(t("Empty for anonymous login", "留空使用匿名登录"));
    impl_->ftpPassword = new QLineEdit;
    impl_->ftpPassword->setObjectName("ftpPassword");
    impl_->ftpPassword->setEchoMode(QLineEdit::Password);
    ftpForm->addRow(t("Username", "用户名"), impl_->ftpUsername);
    ftpForm->addRow(t("Password", "密码"), impl_->ftpPassword);
    impl_->ftpTls =
        new QCheckBox(t("Require explicit TLS for ftp:// (AUTH TLS)", "ftp:// 必须使用显式 TLS（AUTH TLS）"));
    impl_->ftpActive = new QCheckBox(t("Active mode (default: passive)", "主动模式（默认被动）"));
    ftpForm->addRow(impl_->ftpTls);
    ftpForm->addRow(impl_->ftpActive);
    auto* caRow = new QWidget;
    auto* caLayout = new QHBoxLayout(caRow);
    caLayout->setContentsMargins(0, 0, 0, 0);
    impl_->ftpCa = new QLineEdit;
    impl_->ftpCa->setPlaceholderText(t("Optional private CA bundle", "可选：私有 CA 证书文件"));
    auto* caBrowse = new QPushButton(t("Browse…", "浏览…"));
    caLayout->addWidget(impl_->ftpCa);
    caLayout->addWidget(caBrowse);
    connect(caBrowse, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getOpenFileName(this, "CA", {}, "Certificates (*.pem *.crt);;All (*)");
        if (!file.isEmpty())
            impl_->ftpCa->setText(file);
    });
    ftpForm->addRow(t("CA bundle", "CA 证书"), caRow);
    auto* ftpNote = new QLabel(
        t("ftps:// uses implicit TLS. Certificates are always checked. FTP uses one data connection. "
          "Credentials are not saved; enter them again after restart. Without an expected SHA-256, "
          "resuming safely restarts the file. Plain FTP does not encrypt credentials.",
          "ftps:// 使用隐式 TLS，始终校验证书。FTP 使用单数据连接。账号密码不保存，重启后需重新输入。"
          "未提供预期 SHA-256 时，恢复任务会安全地重新下载。普通 FTP 不加密账号密码。"));
    ftpNote->setWordWrap(true);
    ftpForm->addRow(ftpNote);
    const auto ftpIndex = tabs->addTab(ftpTab, "FTP / FTPS");
    auto* torrentTab = new QWidget;
    auto* torrentForm = new QFormLayout(torrentTab);
    torrentForm->setSpacing(14);
    impl_->torrentAll = new QCheckBox(t("Download all files", "下载全部文件"));
    impl_->torrentAll->setChecked(true);
    impl_->torrentAll->setObjectName("torrentAllFiles");
    impl_->torrentSeed = new QCheckBox(t("Continue seeding after download", "下载完成后继续做种"));
    impl_->torrentSeed->setObjectName("torrentSeedAfterDownload");
    impl_->torrentUpload = new QSpinBox;
    impl_->torrentUpload->setObjectName("torrentUploadLimit");
    impl_->torrentUpload->setRange(0, 1000000);
    impl_->torrentUpload->setSpecialValueText(t("Unlimited", "不限速"));
    impl_->torrentUpload->setSuffix(" KiB/s");
    torrentForm->addRow(impl_->torrentAll);
    torrentForm->addRow(impl_->torrentSeed);
    torrentForm->addRow(t("Upload limit", "上传限速"), impl_->torrentUpload);
    auto* torrentNote = new QLabel(
        t("Use a .torrent file or magnet link. Uncheck all files to obtain the file list first, then "
          "choose files in Details. Data is verified by pieces and published into a dedicated folder. "
          "BitTorrent uploads while downloading; continuing after completion is optional.",
          "支持 .torrent 文件与磁力链接。取消“下载全部文件”后，先获取文件列表，再在详情中选择文件。"
          "数据按分片校验并保存到独立目录。BT 下载期间会上传，完成后是否继续做种由你选择。"));
    torrentNote->setWordWrap(true);
    torrentForm->addRow(torrentNote);
    const auto torrentIndex = tabs->addTab(torrentTab, "BT / Magnet");
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
    auto updateProtocol = [this, tabs, ftpIndex, torrentIndex] {
        const bool ftp = isFtpUrl(impl_->url->text().trimmed().toStdString());
        const bool torrent = isTorrentUrl(impl_->url->text().trimmed().toStdString());
        tabs->setTabEnabled(ftpIndex, ftp);
        tabs->setTabEnabled(torrentIndex, torrent);
        impl_->media->setEnabled(!ftp && !torrent);
        impl_->connections->setEnabled(!ftp && !torrent);
        impl_->referrer->setEnabled(!ftp && !torrent);
        impl_->cookie->setEnabled(!ftp && !torrent);
        impl_->authorization->setEnabled(!ftp && !torrent);
        impl_->headers->setEnabled(!ftp && !torrent);
        impl_->checksum->setEnabled(!torrent);
    };
    connect(impl_->url, &QLineEdit::textChanged, this,
            [updateProtocol](const QString&) { updateProtocol(); });
    updateProtocol();
    const auto updateSourceRequirement = [this] {
        const auto source = impl_->url->text().toStdString();
        const bool torrent = isTorrentUrl(source);
        const bool material =
            isFtpUrl(source)
                ? (!impl_->ftpUsername->text().isEmpty() || !impl_->ftpPassword->text().isEmpty())
                : (!impl_->cookie->text().isEmpty() || !impl_->authorization->text().isEmpty() ||
                   !impl_->headers->toPlainText().trimmed().isEmpty());
        if (torrent)
            impl_->originAuthenticationRequired->setChecked(false);
        else if (material)
            impl_->originAuthenticationRequired->setChecked(true);
        impl_->originAuthenticationRequired->setEnabled(!torrent && !material);
    };
    for (auto* field :
         {impl_->cookie, impl_->authorization, impl_->ftpUsername, impl_->ftpPassword, impl_->url})
        connect(field, &QLineEdit::textChanged, this, updateSourceRequirement);
    connect(impl_->headers, &QPlainTextEdit::textChanged, this, updateSourceRequirement);
    updateSourceRequirement();
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
        if ((spec.kind == maskpilot::TransferKind::Media) &&
            (!QFileInfo(impl_->downloader->text()).isExecutable() ||
             !QFileInfo(impl_->muxer->text()).isExecutable())) {
            QMessageBox::warning(this, t("Media support", "媒体支持"),
                                 t("Locate both components, or install them from Tools → Media support.",
                                   "请选择两个媒体组件，或从“工具 → 媒体支持”安装。"));
            return;
        }
        if ((spec.kind == maskpilot::TransferKind::Media)) {
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
    s.proxy = impl_->proxy->text().trimmed().toStdString();
    s.proxyUsername = impl_->proxyUsername->text().toStdString();
    s.proxyPassword = impl_->proxyPassword->text().toStdString();
    s.proxyAuthenticationRequired = impl_->proxyAuthenticationRequired->isChecked() ||
                                    !s.proxyUsername.empty() || !s.proxyPassword.empty();
    s.cookie = impl_->cookie->text().toStdString();
    s.authorization = impl_->authorization->text().toStdString();
    s.originAuthenticationRequired = impl_->originAuthenticationRequired->isChecked();
    s.expectedSha256 = impl_->checksum->text().trimmed().toStdString();
    s.kind = impl_->media->isChecked() ? maskpilot::TransferKind::Media : maskpilot::TransferKind::Http;
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
    if (isFtpUrl(s.url)) {
        s.kind = maskpilot::TransferKind::Ftp;
        s.referrer.clear();
        s.cookie.clear();
        s.authorization.clear();
        s.headers.clear();
        s.ftpUsername = impl_->ftpUsername->text().toStdString();
        s.ftpPassword = impl_->ftpPassword->text().toStdString();
        s.ftpExplicitTls = impl_->ftpTls->isChecked();
        s.ftpActiveMode = impl_->ftpActive->isChecked();
        s.ftpCaBundle = nativePath(impl_->ftpCa->text().trimmed());
    }
    if (isTorrentUrl(s.url)) {
        s.kind = TransferKind::BitTorrent;
        s.referrer.clear();
        s.cookie.clear();
        s.authorization.clear();
        s.headers.clear();
        s.expectedSha256.clear();
        s.torrent.allFiles = impl_->torrentAll->isChecked();
        s.torrent.seedAfterDownload = impl_->torrentSeed->isChecked();
        s.torrent.uploadLimit = static_cast<std::int64_t>(impl_->torrentUpload->value()) * 1024;
        if (s.url == impl_->torrentSource) {
            s.torrent.metadata = impl_->torrentMetadata;
            if (!s.torrent.allFiles)
                s.torrent.selectedFiles = impl_->torrentSelection;
        }
    }
    s.originAuthenticationRequired = s.originAuthenticationRequired || hasOriginMaterial(s);
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
    impl_->proxyUsername->setText(QString::fromStdString(spec.proxyUsername));
    impl_->proxyPassword->setText(QString::fromStdString(spec.proxyPassword));
    impl_->proxyAuthenticationRequired->setChecked(
        spec.proxyAuthenticationRequired || !spec.proxyUsername.empty() || !spec.proxyPassword.empty());
    impl_->cookie->setText(QString::fromStdString(spec.cookie));
    impl_->authorization->setText(QString::fromStdString(spec.authorization));
    impl_->checksum->setText(QString::fromStdString(spec.expectedSha256));
    impl_->ftpUsername->setText(QString::fromStdString(spec.ftpUsername));
    impl_->ftpPassword->setText(QString::fromStdString(spec.ftpPassword));
    impl_->ftpTls->setChecked(spec.ftpExplicitTls);
    impl_->ftpActive->setChecked(spec.ftpActiveMode);
    impl_->ftpCa->setText(QString::fromStdWString(spec.ftpCaBundle.wstring()));
    impl_->torrentSource = spec.url;
    impl_->torrentMetadata = spec.torrent.metadata;
    impl_->torrentSelection = spec.torrent.selectedFiles;
    impl_->torrentAll->setChecked(spec.torrent.allFiles);
    impl_->torrentSeed->setChecked(spec.torrent.seedAfterDownload);
    impl_->torrentUpload->setValue(static_cast<int>(spec.torrent.uploadLimit / 1024));
    QStringList headers;
    for (const auto& [name, value] : spec.headers)
        headers << QString::fromStdString(name + ": " + value);
    impl_->headers->setPlainText(headers.join('\n'));
    impl_->media->setChecked(spec.kind == TransferKind::Media);
    impl_->mode->setCurrentIndex(spec.audioOnly ? 1 : 0);
    auto quality = impl_->quality->findData(QString::fromStdString(spec.mediaFormat));
    if (quality >= 0)
        impl_->quality->setCurrentIndex(quality);
    if (!spec.mediaDownloader.empty())
        impl_->downloader->setText(QString::fromStdWString(spec.mediaDownloader.wstring()));
    if (!spec.mediaMuxer.empty())
        impl_->muxer->setText(QString::fromStdWString(spec.mediaMuxer.wstring()));
    impl_->destination->setText(QString::fromStdWString(spec.destination.wstring()));
    impl_->originAuthenticationRequired->setChecked(requiresOriginAuthentication(spec));
    impl_->originAuthenticationRequired->setEnabled(!isTorrentUrl(spec.url) && !hasOriginMaterial(spec));
    impl_->updateStorage();
}
bool AddDownloadDialog::rememberProxyCredentials() const { return impl_->rememberProxy->isChecked(); }
void AddDownloadDialog::setStoredProxyCredentials(bool stored) {
    impl_->storedProxy = stored;
    impl_->storedProxyEndpoint = impl_->proxy->text().trimmed();
    impl_->rememberProxy->setChecked(stored);
    impl_->updateStorage();
}
void AddDownloadDialog::setQueues(const std::vector<DownloadQueue>& queues, std::string selected,
                                  bool editable) {
    impl_->queue->clear();
    const auto t = [this](const char* en, const char* zh) {
        return QString::fromUtf8(impl_->chinese ? zh : en);
    };
    for (const auto& value : queues) {
        auto name = value.id == defaultQueueId && value.name == "Default"
                        ? t("Default", "默认队列")
                        : QString::fromStdString(value.name);
        if (!value.enabled)
            name += t(" (paused)", "（已暂停）");
        impl_->queue->addItem(name, QString::fromStdString(value.id));
    }
    if (queues.empty())
        impl_->queue->addItem(t("Default", "默认队列"), defaultQueueId);
    auto index = impl_->queue->findData(QString::fromStdString(selected));
    if (index < 0 && !editable) {
        impl_->queue->addItem(t("Unavailable queue", "队列不可用"), QString::fromStdString(selected));
        index = impl_->queue->count() - 1;
    }
    impl_->queue->setCurrentIndex(index >= 0 ? index : 0);
    impl_->queue->setEnabled(editable);
    if (!editable)
        impl_->queue->setToolTip(t("Use Move to queue from the task menu after pausing.",
                                   "暂停任务后，可通过右键菜单移动到其他队列。"));
}
std::string AddDownloadDialog::queueId() const {
    return impl_->queue->currentData().toString().toStdString();
}
} // namespace maskpilot
