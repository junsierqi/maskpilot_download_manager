#include <QApplication>
#include <QDialog>
#include <QSettings>
#include <atomic>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/domain/site_project.h>
#include <maskpilot/domain/task.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/ui/main_window.h>
#include <sstream>
#include <thread>

using namespace maskpilot;
namespace {
SiteProjectOptions siteOptions() {
    SiteProjectOptions options;
    options.rootUrl = "https://example.test/docs";
    options.outputDirectory = std::filesystem::temp_directory_path();
    return options;
}
} // namespace
TEST(SiteProjectDomain, PublicRootsAndFiniteScopeAreRequiredBeforeAnyNetworkWork) {
    auto options = siteOptions();
    EXPECT_TRUE(validateSiteProjectOptions(options).empty());
    for (const auto* url :
         {"ftp://example.test/file", "https://user:password@example.test/", "file:///C:/x"}) {
        auto invalid = options;
        invalid.rootUrl = url;
        EXPECT_FALSE(validateSiteProjectOptions(invalid).empty());
    }
    auto invalid = options;
    invalid.maxResources = 1001;
    EXPECT_FALSE(validateSiteProjectOptions(invalid).empty());
    invalid = options;
    invalid.maxPageBytes = 0;
    EXPECT_FALSE(validateSiteProjectOptions(invalid).empty());
    invalid = options;
    invalid.maxDepth = 9;
    EXPECT_FALSE(validateSiteProjectOptions(invalid).empty());
    invalid = options;
    invalid.includePrefixes = {"relative"};
    EXPECT_FALSE(validateSiteProjectOptions(invalid).empty());
    invalid = options;
    invalid.proxy = "http://user:password@example.test:80";
    EXPECT_FALSE(validateSiteProjectOptions(invalid).empty());
}
TEST(SiteProjectDomain, LowerNewRevisionOutputBudgetCannotInvalidateOlderPublishedReceipt) {
    SiteProject project;
    project.id = newTaskId();
    project.options = siteOptions();
    project.options.maxOutputBytes = 512;
    project.revision = 2;
    SiteProjectPublication publication;
    publication.revision = 1;
    publication.stageToken = newTaskId();
    publication.files.push_back({"index.html", std::string(64, 'a'), 1024});
    project.published = publication;
    EXPECT_TRUE(validateSiteProject(project).empty());
    project.published->revision = 2;
    EXPECT_FALSE(validateSiteProject(project).empty());
    project.published = publication;
    project.published->files[0].name = "../outside.html";
    EXPECT_FALSE(validateSiteProject(project).empty());
}

namespace {
struct ProjectFaultTemp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-project-fault-" + newTaskId());
    ProjectFaultTemp() { std::filesystem::create_directories(path / "exports"); }
    ~ProjectFaultTemp() {
        std::error_code error;
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().string().starts_with("maskpilot-project-fault-"))
            std::filesystem::remove_all(path, error);
    }
};
class FailingProjectRepository final : public ISiteProjectRepository {
    std::unique_ptr<ISiteProjectRepository> inner;

  public:
    std::atomic<SiteProjectState> failState = SiteProjectState::Count;
    explicit FailingProjectRepository(const std::filesystem::path& root)
        : inner(makeSiteProjectRepository(root)) {}
    std::vector<SiteProject> load() override { return inner->load(); }
    void save(const SiteProject& project) override {
        if (project.state == failState.load())
            throw std::runtime_error("Owned manifest commit failure");
        inner->save(project);
    }
    void erase(const std::string& id) override { inner->erase(id); }
    std::filesystem::path prepareRevision(const std::string& id, std::int64_t revision) override {
        return inner->prepareRevision(id, revision);
    }
    SiteProjectBody readBody(const SiteProject& project, const SiteProjectEntry& item) override {
        return inner->readBody(project, item);
    }
};
struct ProjectFaultTransport final : ITransferTransport {
    std::atomic_int& calls;
    std::atomic_int& pauses;
    std::atomic_bool& holdImage;
    std::atomic_bool& holdRoot;
    ProjectFaultTransport(std::atomic_int& c, std::atomic_int& p, std::atomic_bool& hold,
                          std::atomic_bool& root)
        : calls(c), pauses(p), holdImage(hold), holdRoot(root) {}
    void run(const Task& task, const std::filesystem::path&, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        ++calls;
        const bool image = task.spec.url.ends_with("/photo");
        TransferEvent event;
        if (!image && holdRoot.load()) {
            event.phase = TaskState::Downloading;
            callback(event);
            while (holdRoot && !control.pause && !control.cancel)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (control.pause || control.cancel) {
                event.outcome = control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused;
                callback(event);
                return;
            }
        }
        if (image && holdImage.load()) {
            event.phase = TaskState::Downloading;
            callback(event);
            while (!control.pause && !control.cancel)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++pauses;
            event.outcome = control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused;
            callback(event);
            return;
        }
        const std::string body = image ? std::string("\x89PNG\r\n\x1a\n", 8)
                                       : "<html><body><a href='/photo'><img src='/photo'></a></body></html>";
        {
            std::ofstream file(task.spec.destination, std::ios::binary);
            file << body;
        }
        event.outcome = TransferOutcome::Complete;
        event.responseUrl = task.spec.url;
        event.responseContentType = image ? "image/png" : "text/html; charset=UTF-8";
        event.downloaded = event.total = static_cast<std::int64_t>(body.size());
        callback(event);
    }
};
bool awaitProject(const std::function<bool()>& condition) {
    for (int n = 0; n < 800; ++n) {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}
struct ProjectFaultRig {
    ProjectFaultTemp temporary;
    std::atomic_int calls = 0, pauses = 0;
    std::atomic_bool holdImage = false, holdRoot = false;
    std::unique_ptr<DownloadService> downloads;
    FailingProjectRepository* repository;
    std::unique_ptr<SiteProjectService> projects;
    ProjectFaultRig() {
        downloads = std::make_unique<DownloadService>(
            makeSqliteRepository(temporary.path / "tasks.db"),
            [&] { return std::make_unique<ProjectFaultTransport>(calls, pauses, holdImage, holdRoot); },
            temporary.path / "parts");
        auto store = std::make_unique<FailingProjectRepository>(temporary.path / "projects");
        repository = store.get();
        projects = std::make_unique<SiteProjectService>(
            *downloads, std::move(store), makeSiteContentProcessor(), makeSiteProjectPublisher());
    }
    std::string create() {
        auto options = siteOptions();
        options.rootUrl = "https://example.test/";
        options.outputDirectory = temporary.path / "exports";
        return projects->create(options).get();
    }
    bool state(const std::string& id, SiteProjectState expected) {
        const auto value = projects->inspect(id);
        return value && value->state == expected;
    }
};
} // namespace

TEST(SiteProjectFaults, RejectedPreviewManifestCannotLeaveRunnableIntent) {
    ProjectFaultRig rig;
    const auto id = rig.create();
    rig.repository->failState = SiteProjectState::Previewing;
    EXPECT_THROW(rig.projects->preview(id, 1).get(), std::runtime_error);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_FALSE(rig.projects->startupError().empty());
    EXPECT_TRUE(rig.projects->hasUnfinished());
    EXPECT_TRUE(rig.downloads->snapshot().empty());
    EXPECT_EQ(rig.calls.load(), 0);
}

TEST(SiteProjectFaults, RejectedStartManifestCannotSubmitKnownBinaryOrPublish) {
    ProjectFaultRig rig;
    const auto id = rig.create();
    rig.projects->preview(id, 1).get();
    ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::PreviewReady); }));
    ASSERT_EQ(rig.calls.load(), 1);
    rig.repository->failState = SiteProjectState::Running;
    EXPECT_THROW(rig.projects->start(id, 1).get(), std::runtime_error);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(rig.calls.load(), 1);
    EXPECT_EQ(rig.downloads->snapshot().size(), 1u);
    EXPECT_FALSE(rig.projects->inspect(id)->published.has_value());
    EXPECT_FALSE(rig.projects->startupError().empty());
}

TEST(SiteProjectFaults, RejectedPauseManifestStillStopsExistingChildren) {
    ProjectFaultRig rig;
    rig.holdImage = true;
    const auto id = rig.create();
    rig.projects->preview(id, 1).get();
    ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::PreviewReady); }));
    rig.projects->start(id, 1).get();
    ASSERT_TRUE(awaitProject([&] { return rig.calls.load() == 2; }));
    rig.repository->failState = SiteProjectState::Pausing;
    EXPECT_THROW(rig.projects->pause(id, 1).get(), std::runtime_error);
    ASSERT_TRUE(awaitProject([&] { return rig.pauses.load() == 1; }));
    EXPECT_FALSE(rig.projects->startupError().empty());
    EXPECT_EQ(rig.calls.load(), 2);
}

TEST(SiteProjectFaults, FinalManifestFailureKeepsPriorOpenAndRestartAdoptsOnlyVerifiedPublication) {
    for (const bool tamper : {false, true}) {
        SCOPED_TRACE(tamper);
        ProjectFaultRig rig;
        const auto id = rig.create();
        rig.projects->preview(id, 1).get();
        ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::PreviewReady); }));
        rig.projects->start(id, 1).get();
        ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::Completed); }));
        const auto prior = rig.projects->openPath(id).get();
        const auto options = rig.projects->inspect(id)->options;
        rig.projects->update(id, 1, options).get();
        ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::PreviewReady); }));
        rig.repository->failState = SiteProjectState::Completed;
        rig.projects->start(id, 2).get();
        ASSERT_TRUE(awaitProject([&] { return !rig.projects->startupError().empty(); }));
        EXPECT_EQ(rig.projects->openPath(id).get(), prior);
        const auto next = prior.parent_path().parent_path() / "r-2" / "index.html";
        ASSERT_TRUE(std::filesystem::is_regular_file(next));
        rig.projects.reset();
        if (tamper) {
            std::ofstream changed(next, std::ios::binary | std::ios::app);
            changed << "changed";
        }
        rig.projects = std::make_unique<SiteProjectService>(
            *rig.downloads, makeSiteProjectRepository(rig.temporary.path / "projects"),
            makeSiteContentProcessor(), makeSiteProjectPublisher());
        EXPECT_TRUE(rig.projects->startupError().empty());
        ASSERT_TRUE(rig.projects->inspect(id).has_value());
        EXPECT_TRUE(rig.state(id, tamper ? SiteProjectState::NeedsAttention : SiteProjectState::Completed));
        EXPECT_EQ(rig.projects->openPath(id).get(), tamper ? prior : next);
        EXPECT_TRUE(std::filesystem::is_regular_file(prior));
    }
}

TEST(SiteProjectLifecycle, ConcreteThumbnailRolePrecedesGenericAnchorAndBinaryNeedsStart) {
    ProjectFaultRig rig;
    const auto id = rig.create();
    rig.projects->preview(id, 1).get();
    ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::PreviewReady); }));
    const auto preview = rig.projects->inspect(id);
    ASSERT_EQ(preview->entries.size(), 2u);
    EXPECT_EQ(preview->entries[1].kind, SiteContentKind::Image);
    EXPECT_EQ(preview->entries[1].state, SiteProjectEntryState::Planned);
    EXPECT_EQ(rig.calls.load(), 1);
    rig.projects->start(id, 1).get();
    ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::Completed); }));
    EXPECT_EQ(rig.calls.load(), 2);
    EXPECT_TRUE(std::filesystem::is_regular_file(rig.projects->openPath(id).get()));
}

TEST(SiteProjectLifecycle, UnicodePathFiltersUseTheSameCanonicalEncodingAsDiscoveredUrls) {
    ProjectFaultRig rig;
    auto options = siteOptions();
    options.rootUrl = "https://example.test/\xe6\x96\x87\xe6\xa1\xa3/index";
    options.includePrefixes = {"/\xe6\x96\x87\xe6\xa1\xa3/"};
    options.outputDirectory = rig.temporary.path / "exports";
    const auto id = rig.projects->create(options).get();
    const auto project = rig.projects->inspect(id);
    ASSERT_TRUE(project.has_value());
    EXPECT_EQ(project->options.includePrefixes, std::vector<std::string>{"/%E6%96%87%E6%A1%A3/"});
    EXPECT_EQ(rig.calls.load(), 0);
    options.includePrefixes = {"//other.test/"};
    EXPECT_THROW(rig.projects->create(options).get(), std::invalid_argument);
    EXPECT_EQ(rig.calls.load(), 0);
}

TEST(SiteProjectLifecycle, CompletedChildrenDoNotPreventCancelOrPause) {
    ProjectFaultRig rig;
    const auto id = rig.create();
    rig.projects->preview(id, 1).get();
    ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::PreviewReady); }));
    rig.projects->cancel(id, 1).get();
    ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::Cancelled); }));
    rig.holdImage = true;
    auto options = rig.projects->inspect(id)->options;
    rig.projects->update(id, 1, options).get();
    ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::PreviewReady); }));
    rig.projects->start(id, 2).get();
    ASSERT_TRUE(awaitProject([&] { return rig.calls.load() == 3; }));
    rig.projects->pause(id, 2).get();
    ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::Paused); }));
    rig.projects->cancel(id, 2).get();
    ASSERT_TRUE(awaitProject([&] { return rig.state(id, SiteProjectState::Cancelled); }));
}

TEST(SiteProjectCompletion, PreviewAndCancellationCannotTriggerSuccessfulCompletionCountdown) {
    int argc = 1;
    char name[] = "project_completion";
    char* argv[] = {name, nullptr};
    std::unique_ptr<QApplication> application;
    if (!QCoreApplication::instance()) {
        if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
            qputenv("QT_QPA_PLATFORM", "offscreen");
        application = std::make_unique<QApplication>(argc, argv);
    }
    ProjectFaultRig rig;
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName("SiteProjectCompletion");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdWString(rig.temporary.path.wstring()));
    QSettings settings;
    settings.setValue("language", "en");
    settings.setValue("closeToTray", false);
    settings.setValue("completionAction", 1); // Only the owned test application exit action; never shutdown.
    MainWindow window(*rig.downloads);
    window.setSiteProjectService(*rig.projects);
    window.show();
    const auto pump = [&](const std::function<bool()>& condition, int timeout = 5000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        while (std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents();
            if (condition())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return condition();
    };
    const auto settle = [&] {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
        pump([&] { return std::chrono::steady_clock::now() >= end; }, 1500);
    };
    const auto visibleCountdown = [&]() -> QDialog* {
        auto* dialog = window.findChild<QDialog*>("completionCountdown");
        return dialog && dialog->isVisible() ? dialog : nullptr;
    };
    const auto diagnostics = [&](const std::string& id) {
        std::ostringstream detail;
        detail << "calls=" << rig.calls.load() << " holdRoot=" << rig.holdRoot.load()
               << " holdImage=" << rig.holdImage.load() << " pauses=" << rig.pauses.load()
               << " downloadError=" << rig.downloads->lastError()
               << " projectStartupError=" << rig.projects->startupError();
        if (const auto project = rig.projects->inspect(id)) {
            detail << " projectState=" << siteProjectStateName(project->state)
                   << " pendingPublication=" << project->pendingPublication.has_value()
                   << " published=" << project->published.has_value() << " projectError=" << project->error;
            for (const auto& entry : project->entries)
                detail << " entry=" << entry.id << ':' << siteProjectEntryStateName(entry.state)
                       << " task=" << entry.taskId << " error=" << entry.error;
        } else {
            detail << " projectMissing";
        }
        for (const auto& task : rig.downloads->snapshot())
            detail << " native=" << task.id << ':' << stateName(task.state)
                   << " intent=" << static_cast<int>(task.runIntent) << " generation=" << task.generation
                   << " error=" << task.error;
        return detail.str();
    };
    rig.holdRoot = true;
    const auto first = rig.create();
    rig.projects->preview(first, 1).get();
    ASSERT_TRUE(pump([&] { return rig.calls.load() == 1; })) << diagnostics(first);
    settle();
    rig.holdRoot = false;
    ASSERT_TRUE(pump([&] { return rig.state(first, SiteProjectState::PreviewReady); })) << diagnostics(first);
    settle();
    EXPECT_EQ(visibleCountdown(), nullptr);
    rig.projects->start(first, 1).get();
    ASSERT_TRUE(pump([&] { return rig.state(first, SiteProjectState::Completed); })) << diagnostics(first);
    ASSERT_TRUE(pump([&] { return visibleCountdown() != nullptr; })) << diagnostics(first);
    visibleCountdown()->reject();
    settle();
    rig.holdRoot = true;
    const auto second = rig.create();
    rig.projects->preview(second, 1).get();
    ASSERT_TRUE(pump([&] { return rig.calls.load() == 3; })) << diagnostics(second);
    settle();
    rig.holdRoot = false;
    ASSERT_TRUE(pump([&] { return rig.state(second, SiteProjectState::PreviewReady); }))
        << diagnostics(second);
    settle();
    EXPECT_EQ(visibleCountdown(), nullptr);
    rig.projects->cancel(second, 1).get();
    ASSERT_TRUE(pump([&] { return rig.state(second, SiteProjectState::Cancelled); })) << diagnostics(second);
    settle();
    EXPECT_EQ(visibleCountdown(), nullptr);
    settings.setValue("completionAction", 0);
}
