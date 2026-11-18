#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/credential_vault.h>
#include <maskpilot/ui/add_download_dialog.h>
#include <maskpilot/ui/main_window.h>
#include <thread>

using namespace maskpilot;
namespace {
template <class T> T* control(QWidget& widget, const char* name) {
    auto* value = widget.findChild<T*>(name);
    if (!value)
        throw std::runtime_error(std::string("Missing media widget: ") + name);
    return value;
}
bool untilUi(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do {
        QCoreApplication::processEvents();
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
void platform() {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
}
struct Fixture {
    QTemporaryDir temporary;
    Fixture() {
        if (!temporary.isValid())
            throw std::runtime_error("Cannot create media UI fixture");
        QCoreApplication::setOrganizationName("MaskPilotTests");
        QCoreApplication::setApplicationName("MediaSelectionUi");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings().setValue("downloadDirectory", temporary.path());
        QSettings().setValue("language", "en");
        QSettings().setValue("closeToTray", false);
    }
    DownloadSpec spec() const {
        DownloadSpec result;
        result.kind = TransferKind::Media;
        result.url = "https://example.test/media";
        result.destination = std::filesystem::path(temporary.path().toStdWString()) / L"selected.mkv";
        result.mediaDownloader = "owned-parser.exe";
        result.mediaMuxer = "owned-muxer.exe";
        result.startPaused = true;
        return result;
    }
};
MediaInventory inventory() {
    MediaInventory result;
    result.title = "Owned format fixture";
    MediaFormat video;
    video.id = "v-low";
    video.extension = "mp4";
    video.videoCodec = "avc1.64001e";
    video.audioCodec = "none";
    video.protocol = "https";
    video.width = 640;
    video.height = 360;
    video.hasVideo = true;
    result.formats.push_back(video);
    MediaFormat audio;
    audio.id = "a-track";
    audio.extension = "m4a";
    audio.videoCodec = "none";
    audio.audioCodec = "mp4a.40.2";
    audio.protocol = "https";
    audio.hasAudio = true;
    result.formats.push_back(audio);
    return result;
}
void mediaTab(AddDownloadDialog& dialog) {
    auto* tabs = control<QTabWidget>(dialog, "downloadOptionsTabs");
    tabs->setCurrentIndex(tabs->count() - 1);
}
struct NoTransfer final : ITransferTransport {
    void run(const Task&, const std::filesystem::path&, TransferControl&,
             const std::function<void(TransferEvent)>& callback) override {
        ADD_FAILURE() << "Media UI fixture unexpectedly started a payload";
        TransferEvent event;
        event.outcome = TransferOutcome::Attention;
        callback(event);
    }
};
} // namespace

TEST(MediaUi, ActualControlsChooseInspectedIdsAndRequestChangesDoNotFallBack) {
    platform();
    int argc = 1;
    char name[] = "MediaUiChoice";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    Fixture fixture;
    AddDownloadDialog dialog(false);
    dialog.setSpecification(fixture.spec());
    dialog.setMediaInspector([](MediaInspectionRequest request, std::stop_token) {
        EXPECT_EQ(request.specification.url, "https://example.test/media");
        return inventory();
    });
    dialog.show();
    mediaTab(dialog);
    auto* mode = control<QComboBox>(dialog, "mediaSelectionMode");
    mode->setCurrentIndex(3);
    control<QPushButton>(dialog, "mediaInspect")->click();
    auto* primary = control<QComboBox>(dialog, "mediaPrimaryFormat");
    auto* audio = control<QComboBox>(dialog, "mediaAudioFormat");
    ASSERT_TRUE(untilUi([&] { return primary->findData("v-low") > 0; }));
    primary->setCurrentIndex(primary->findData("v-low"));
    audio->setCurrentIndex(audio->findData("a-track"));
    auto* container = control<QComboBox>(dialog, "mediaContainer");
    container->setCurrentIndex(container->findData(static_cast<int>(MediaContainer::Mkv)));
    auto spec = dialog.specification();
    EXPECT_EQ(spec.mediaSelection.mode, MediaSelectionMode::VideoAudio);
    EXPECT_EQ(spec.mediaSelection.primaryFormat, "v-low");
    EXPECT_EQ(spec.mediaSelection.audioFormat, "a-track");
    EXPECT_TRUE(validateMediaChoice(spec.mediaSelection, inventory()).empty());
    AddDownloadDialog reopened(false);
    reopened.setSpecification(spec);
    reopened.setQueues({DownloadQueue{}}, defaultQueueId);
    EXPECT_EQ(reopened.specification().mediaSelection.primaryFormat, "v-low");
    EXPECT_EQ(reopened.specification().mediaSelection.audioFormat, "a-track");
    auto legacy = fixture.spec();
    legacy.mediaFormat = "best[ext=mp4]/best";
    reopened.setSpecification(legacy);
    EXPECT_EQ(reopened.specification().mediaFormat, legacy.mediaFormat);
    control<QLineEdit>(dialog, "downloadUrl")->setText("https://example.test/changed");
    EXPECT_EQ(mode->currentIndex(), 3);
    EXPECT_TRUE(dialog.specification().mediaSelection.primaryFormat.empty());
    EXPECT_FALSE(validateMediaSelection(dialog.specification().mediaSelection).empty());
    EXPECT_FALSE(control<QLabel>(dialog, "mediaInspectionStatus")->text().contains("actual formats"));
}

TEST(MediaUi, CancelAndLateOldResultsNeverPopulateChangedRequest) {
    platform();
    int argc = 1;
    char name[] = "MediaUiCancel";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    Fixture fixture;
    std::atomic_int completed{0};
    AddDownloadDialog dialog(false);
    dialog.setSpecification(fixture.spec());
    dialog.setMediaInspector([&](MediaInspectionRequest, std::stop_token) {
        // Deliberately late cooperative-bound fixture result; generation must still reject it.
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        ++completed;
        return inventory();
    });
    dialog.show();
    mediaTab(dialog);
    control<QComboBox>(dialog, "mediaSelectionMode")->setCurrentIndex(3);
    auto* inspect = control<QPushButton>(dialog, "mediaInspect");
    inspect->click();
    control<QLineEdit>(dialog, "downloadUrl")->setText("https://example.test/new");
    ASSERT_TRUE(untilUi([&] { return completed == 1 && inspect->isEnabled(); }));
    EXPECT_EQ(control<QComboBox>(dialog, "mediaPrimaryFormat")->count(), 1);
    inspect->click();
    control<QPushButton>(dialog, "mediaCancelInspection")->click();
    ASSERT_TRUE(untilUi([&] { return completed == 2 && inspect->isEnabled(); }));
    EXPECT_FALSE(control<QPushButton>(dialog, "mediaCancelInspection")->isEnabled());
    EXPECT_EQ(control<QComboBox>(dialog, "mediaPrimaryFormat")->count(), 1);
}

TEST(MediaUi, SidebarLeadingIconsStayVisibleAcrossSelectionAtMinimumWidth) {
    platform();
    int argc = 1;
    char name[] = "MediaSidebar";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    Fixture fixture;
    const auto root = std::filesystem::path(fixture.temporary.path().toStdWString());
    auto store = makeSqliteStore(root / L"tasks.db");
    DownloadService service(
        std::move(store.tasks), [] { return std::make_unique<NoTransfer>(); }, root / L"parts", {},
        QueueDependencies{std::move(store.queues)});
    MainWindow window(service);
    window.resize(850, 560);
    window.show();
    auto* sidebar = control<QListWidget>(window, "downloadSidebar");
    for (int row = 0; row < sidebar->count(); ++row) {
        if (!(sidebar->item(row)->flags() & Qt::ItemIsSelectable))
            continue;
        sidebar->setCurrentRow(row);
        QCoreApplication::processEvents();
        EXPECT_EQ(sidebar->horizontalScrollBar()->value(), 0);
        EXPECT_GE(sidebar->visualItemRect(sidebar->item(row)).left(), 0);
    }
    sidebar->setCurrentRow(0);
    QCoreApplication::processEvents();
    const auto capture = qEnvironmentVariable("MASKPILOT_TEST_MEDIA_SIDEBAR_SCREENSHOT");
    if (!capture.isEmpty())
        EXPECT_TRUE(window.grab().save(capture));
    window.requestQuit();
}

TEST(MediaUi, TrustedInspectionResolvesStoredStandaloneProxyAndRejectsQuotaOrChangedRoute) {
    platform();
    int argc = 1;
    char name[] = "MediaRouteResolution";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    Fixture fixture;
    const auto root = std::filesystem::path(fixture.temporary.path().toStdWString());
    auto store = makeSqliteStore(root / L"tasks.db");
    auto vault = makeNativeCredentialVault(store.contexts->installationId());
    DownloadService service(
        std::move(store.tasks), [] { return std::make_unique<NoTransfer>(); }, root / L"parts",
        {std::move(store.contexts), std::move(vault)},
        QueueDependencies{std::move(store.queues), {}, {}, std::move(store.traffic)});
    auto original = fixture.spec();
    original.kind = TransferKind::Http;
    original.proxy = "http://127.0.0.1:18089";
    original.proxyUsername = "owned-media-route-user";
    original.proxyPassword = "owned-media-route-private-password";
    original.proxyAuthenticationRequired = true;
    const auto id = service.add(original, {}, true).get();
    struct Cleanup {
        DownloadService& service;
        std::string id;
        ~Cleanup() {
            try {
                service.forgetSavedProxyCredentials(id).get();
            } catch (...) {
            }
        }
    } cleanup{service, id};
    auto preview = original;
    preview.kind = TransferKind::Media;
    preview.proxyUsername.clear();
    preview.proxyPassword.clear();
    const auto resolved = service.resolveMediaInspection({preview, id}).get();
    EXPECT_EQ(resolved.proxyUsername, original.proxyUsername);
    EXPECT_EQ(resolved.proxyPassword, original.proxyPassword);
    EXPECT_EQ(service.snapshot()[0].spec.kind, TransferKind::Http);
    EXPECT_EQ(service.snapshot()[0].state, TaskState::Paused);
    preview.proxy = "http://127.0.0.1:18090";
    EXPECT_THROW(service.resolveMediaInspection({preview, id}).get(), std::exception);
    QueueConfiguration queue;
    queue.name = "No media allowance";
    queue.dailyPayloadBudget = 1024;
    const auto restricted = service.configureQueue(queue).get();
    auto publicMedia = fixture.spec();
    EXPECT_THROW(service.resolveMediaInspection({publicMedia, {}, restricted.id}).get(),
                 std::invalid_argument);
    EXPECT_THROW(service.resolveMediaInspection({publicMedia, newTaskId()}).get(), std::invalid_argument);
}
