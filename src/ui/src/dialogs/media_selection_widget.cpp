#include "media_selection_widget.h"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <utility>

namespace maskpilot::ui {
struct MediaSelectionWidget::Impl {
    MediaSelectionWidget* widget;
    bool chinese, setting = false, busy = false;
    Request request;
    MediaInspectionFunction inspector;
    QComboBox *mode, *container, *primary, *audio, *quality;
    QPushButton *inspect, *cancel;
    QLabel *status, *primaryLabel, *audioLabel, *qualityLabel;
    MediaInventory inventory;
    std::string wantedPrimary, wantedAudio;
    std::uint64_t generation = 0, runningGeneration = 0;
    struct Result {
        std::atomic_bool done{false};
        MediaInventory inventory;
        std::string error;
    };
    std::shared_ptr<Result> result;
    std::jthread worker;
    QString tr(const char* en, const char* zh) const { return QString::fromUtf8(chinese ? zh : en); }
    MediaSelectionMode selectedMode() const {
        switch (mode->currentIndex()) {
        case 2:
            return MediaSelectionMode::Muxed;
        case 3:
            return MediaSelectionMode::VideoAudio;
        case 4:
            return MediaSelectionMode::VideoOnly;
        case 5:
            return MediaSelectionMode::AudioOnly;
        default:
            return MediaSelectionMode::Automatic;
        }
    }
    bool audioOnly() const { return mode->currentIndex() == 1 || mode->currentIndex() == 5; }
    QString formatLabel(const MediaFormat& format) const {
        auto label = QString::fromStdString(format.id) + " · " + QString::fromStdString(format.extension);
        if (format.hasVideo && format.height > 0)
            label += " · " + QString::number(format.width) + "×" + QString::number(format.height);
        if (format.fps > 0)
            label += " · " + QString::number(format.fps, 'g', 4) + " fps";
        if (!format.videoCodec.empty())
            label += " · " + QString::fromStdString(format.videoCodec);
        if (!format.audioCodec.empty())
            label += " · " + QString::fromStdString(format.audioCodec);
        if (format.bytes >= 0)
            label += " · " + QString(format.estimated ? "~" : "") + QLocale().formattedDataSize(format.bytes);
        return label;
    }
    void rebuild() {
        const QSignalBlocker b1(primary), b2(audio), b3(container);
        const auto oldContainer = container->currentData().toInt();
        primary->clear();
        audio->clear();
        primary->addItem(tr("Choose an inspected format…", "选择探测到的格式…"), "");
        audio->addItem(tr("Choose an inspected audio format…", "选择探测到的音频格式…"), "");
        for (const auto& format : inventory.formats) {
            const auto selected = selectedMode();
            const bool primaryMatch =
                (selected == MediaSelectionMode::Muxed && format.hasVideo && format.hasAudio) ||
                ((selected == MediaSelectionMode::VideoAudio || selected == MediaSelectionMode::VideoOnly) &&
                 format.hasVideo && !format.hasAudio) ||
                (selected == MediaSelectionMode::AudioOnly && format.hasAudio);
            if (primaryMatch)
                primary->addItem(formatLabel(format), QString::fromStdString(format.id));
            if (format.hasAudio && !format.hasVideo)
                audio->addItem(formatLabel(format), QString::fromStdString(format.id));
        }
        if (inventory.formats.empty()) {
            if (!wantedPrimary.empty())
                primary->addItem(tr("Saved format: ", "已保存格式：") + QString::fromStdString(wantedPrimary),
                                 QString::fromStdString(wantedPrimary));
            if (!wantedAudio.empty())
                audio->addItem(tr("Saved audio: ", "已保存音频：") + QString::fromStdString(wantedAudio),
                               QString::fromStdString(wantedAudio));
        }
        primary->setCurrentIndex(std::max(0, primary->findData(QString::fromStdString(wantedPrimary))));
        audio->setCurrentIndex(std::max(0, audio->findData(QString::fromStdString(wantedAudio))));
        container->clear();
        container->addItem(tr("Default container", "默认容器"), static_cast<int>(MediaContainer::Automatic));
        if (audioOnly()) {
            for (const auto& [label, value] : {std::pair{"M4A", MediaContainer::M4a},
                                               {"MP3", MediaContainer::Mp3},
                                               {"Opus", MediaContainer::Opus},
                                               {"FLAC", MediaContainer::Flac}})
                container->addItem(label, static_cast<int>(value));
        } else {
            for (const auto& [label, value] : {std::pair{"MKV", MediaContainer::Mkv},
                                               {"MP4", MediaContainer::Mp4},
                                               {"WebM", MediaContainer::Webm}})
                container->addItem(label, static_cast<int>(value));
        }
        container->setCurrentIndex(std::max(0, container->findData(oldContainer)));
        const bool explicitMode = selectedMode() != MediaSelectionMode::Automatic;
        primary->setVisible(explicitMode);
        primaryLabel->setVisible(explicitMode);
        audio->setVisible(selectedMode() == MediaSelectionMode::VideoAudio);
        audioLabel->setVisible(selectedMode() == MediaSelectionMode::VideoAudio);
        quality->setVisible(!explicitMode && !audioOnly());
        qualityLabel->setVisible(!explicitMode && !audioOnly());
    }
    void stop() {
        ++generation;
        if (worker.joinable())
            worker.request_stop();
    }
    void poll() {
        if (!result || !result->done.load(std::memory_order_acquire))
            return;
        worker.join();
        busy = false;
        inspect->setEnabled(static_cast<bool>(inspector));
        cancel->setEnabled(false);
        const auto completed = std::move(result);
        result.reset();
        if (generation != runningGeneration) {
            status->setText(tr("Inspection cancelled or request changed. Inspect again for explicit formats.",
                               "探测已取消或请求已更改；请重新探测明确格式。"));
            return;
        }
        if (!completed->error.empty()) {
            status->setText(QString::fromStdString(completed->error));
            return;
        }
        inventory = completed->inventory;
        rebuild();
        status->setToolTip("<qt>" + QString::fromStdString(inventory.title).toHtmlEscaped() + "</qt>");
        status->setText(tr("%1 actual formats. Choices are checked again before download.",
                           "探测到 %1 种实际格式；下载前会再次核对明确选择。")
                            .arg(inventory.formats.size()));
        emit widget->changed();
    }
    void start() {
        if (busy || !inspector)
            return;
        MediaInspectionRequest input;
        try {
            input = request();
        } catch (...) {
            status->setText(tr("Cannot prepare this media request.", "无法准备媒体请求。"));
            return;
        }
        input.specification.kind = TransferKind::Media;
        input.specification.mediaSelection = {};
        result = std::make_shared<Result>();
        const auto state = result;
        const auto function = inspector;
        runningGeneration = ++generation;
        busy = true;
        inspect->setEnabled(false);
        cancel->setEnabled(true);
        status->setText(tr("Inspecting the selected source…", "正在探测所选来源…"));
        worker = std::jthread([state, function, input = std::move(input)](std::stop_token stop) mutable {
            try {
                state->inventory = function(std::move(input), stop);
                if (const auto error = validateMediaInventory(state->inventory); !error.empty())
                    throw std::runtime_error(error);
            } catch (const std::exception& error) {
                state->error = error.what();
            } catch (...) {
                state->error = "Media inspection failed.";
            }
            state->done.store(true, std::memory_order_release);
        });
    }
};
MediaSelectionWidget::MediaSelectionWidget(bool chinese, Request request, QWidget* parent)
    : QWidget(parent), impl_(std::make_unique<Impl>()) {
    auto& p = *impl_;
    p.widget = this;
    p.chinese = chinese;
    p.request = std::move(request);
    auto* form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    p.mode = new QComboBox;
    p.mode->setObjectName("mediaSelectionMode");
    p.mode->addItems({p.tr("Automatic video", "自动选择视频"), p.tr("Automatic audio", "自动选择音频"),
                      p.tr("Combined video + audio format", "音画合一格式"),
                      p.tr("Separate video + audio formats", "分别选择视频和音频"),
                      p.tr("Video-only format", "仅视频格式"),
                      p.tr("Audio format / extraction", "音频格式／提取")});
    form->addRow(p.tr("Selection", "选择方式"), p.mode);
    auto* actions = new QWidget;
    auto* actionLayout = new QHBoxLayout(actions);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    p.inspect = new QPushButton(p.tr("Inspect actual formats", "探测实际格式"));
    p.inspect->setObjectName("mediaInspect");
    p.inspect->setEnabled(false);
    p.cancel = new QPushButton(p.tr("Cancel inspection", "取消探测"));
    p.cancel->setObjectName("mediaCancelInspection");
    p.cancel->setEnabled(false);
    actionLayout->addWidget(p.inspect);
    actionLayout->addWidget(p.cancel);
    form->addRow(actions);
    p.status = new QLabel(p.tr("Choose automatic settings or inspect this URL for explicit formats.",
                               "可使用自动设置，或探测此地址后明确选择格式。"));
    p.status->setObjectName("mediaInspectionStatus");
    p.status->setWordWrap(true);
    p.status->setTextFormat(Qt::PlainText);
    p.status->setMinimumHeight(p.status->fontMetrics().lineSpacing() * 2);
    form->addRow(p.status);
    p.primary = new QComboBox;
    p.audio = new QComboBox;
    p.container = new QComboBox;
    p.primary->setObjectName("mediaPrimaryFormat");
    p.audio->setObjectName("mediaAudioFormat");
    p.container->setObjectName("mediaContainer");
    p.primary->setMinimumContentsLength(20);
    p.primary->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    p.audio->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    p.primaryLabel = new QLabel(p.tr("Format", "格式"));
    p.audioLabel = new QLabel(p.tr("Audio format", "音频格式"));
    form->addRow(p.primaryLabel, p.primary);
    form->addRow(p.audioLabel, p.audio);
    form->addRow(p.tr("Container", "输出容器"), p.container);
    p.quality = new QComboBox;
    p.quality->setObjectName("mediaAutomaticQuality");
    p.quality->addItem(p.tr("Best available (automatic)", "最佳可用（自动）"), "bestvideo+bestaudio/best");
    for (const int height : {2160, 1440, 1080, 720, 480})
        p.quality->addItem(QString::number(height) + "p",
                           QString("bestvideo[height<=%1]+bestaudio/best[height<=%1]").arg(height));
    p.qualityLabel = new QLabel(p.tr("Automatic height limit", "自动画质上限"));
    form->addRow(p.qualityLabel, p.quality);
    connect(p.inspect, &QPushButton::clicked, this, [&p] { p.start(); });
    connect(p.cancel, &QPushButton::clicked, this, [&p] {
        p.stop();
        p.status->setText(p.tr("Cancelling inspection…", "正在取消探测…"));
    });
    connect(p.mode, &QComboBox::currentIndexChanged, this, [&p] {
        if (p.setting)
            return;
        p.wantedPrimary.clear();
        p.wantedAudio.clear();
        p.rebuild();
        emit p.widget->changed();
    });
    for (auto* combo : {p.primary, p.audio, p.container, p.quality})
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { emit changed(); });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [&p] { p.poll(); });
    timer->start(50);
    p.rebuild();
}
MediaSelectionWidget::~MediaSelectionWidget() {
    impl_->stop();
    if (impl_->worker.joinable())
        impl_->worker.join();
}
void MediaSelectionWidget::setInspector(MediaInspectionFunction function) {
    impl_->inspector = std::move(function);
    impl_->inspect->setEnabled(!impl_->busy && static_cast<bool>(impl_->inspector));
}
void MediaSelectionWidget::invalidate() {
    auto& p = *impl_;
    p.stop();
    p.inventory = {};
    p.wantedPrimary.clear();
    p.wantedAudio.clear();
    p.rebuild();
    p.status->setText(
        p.tr("Request changed. Inspect again for explicit formats.", "请求已更改；请重新探测明确格式。"));
    emit changed();
}
void MediaSelectionWidget::setSelection(const MediaSelection& selection, bool audioOnly,
                                        const std::string& legacyFormat) {
    auto& p = *impl_;
    p.setting = true;
    int index = audioOnly ? 1 : 0;
    switch (selection.mode) {
    case MediaSelectionMode::Muxed:
        index = 2;
        break;
    case MediaSelectionMode::VideoAudio:
        index = 3;
        break;
    case MediaSelectionMode::VideoOnly:
        index = 4;
        break;
    case MediaSelectionMode::AudioOnly:
        index = 5;
        break;
    default:
        break;
    }
    p.mode->setCurrentIndex(index);
    p.wantedPrimary = selection.primaryFormat;
    p.wantedAudio = selection.audioFormat;
    p.rebuild();
    p.container->setCurrentIndex(std::max(0, p.container->findData(static_cast<int>(selection.container))));
    auto legacy = p.quality->findData(QString::fromStdString(legacyFormat));
    if (legacy < 0 && !legacyFormat.empty()) {
        p.quality->addItem(p.tr("Saved automatic selector", "已保存的自动选择规则"),
                           QString::fromStdString(legacyFormat));
        legacy = p.quality->count() - 1;
    }
    p.quality->setCurrentIndex(std::max(0, legacy));
    p.setting = false;
    emit changed();
}
MediaSelection MediaSelectionWidget::selection() const {
    MediaSelection result;
    result.mode = impl_->selectedMode();
    result.container = static_cast<MediaContainer>(impl_->container->currentData().toInt());
    if (result.mode != MediaSelectionMode::Automatic)
        result.primaryFormat = impl_->primary->currentData().toString().toStdString();
    if (result.mode == MediaSelectionMode::VideoAudio)
        result.audioFormat = impl_->audio->currentData().toString().toStdString();
    return result;
}
bool MediaSelectionWidget::audioOnly() const { return impl_->audioOnly(); }
std::string MediaSelectionWidget::legacyFormat() const {
    return impl_->quality->currentData().toString().toStdString();
}
} // namespace maskpilot::ui
