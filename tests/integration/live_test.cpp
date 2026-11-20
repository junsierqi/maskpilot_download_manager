#include "../../src/infrastructure/src/media/live_playlist.h"
#include "../../src/ui/src/models/task_list_codec.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/ui/add_download_dialog.h>
#include <sqlite3.h>
#include <thread>

using namespace maskpilot;
namespace {
DownloadSpec liveSpec(const std::filesystem::path& destination) {
    DownloadSpec spec;
    spec.url = "http://127.0.0.1/stream.flv";
    spec.destination = destination;
    spec.kind = TransferKind::Live;
    spec.live.format = LiveFormat::Flv;
    spec.live.durationSeconds = 3;
    return spec;
}
class Feed {
  public:
    QTcpServer server;
    int requests = 0;
    QByteArray header = QByteArray::fromHex("464c5601050000000900000000");
    QByteArray tag = QByteArray::fromHex("0900000100000000000000120000000c");
    Feed() {
        if (!server.listen(QHostAddress::LocalHost))
            throw std::runtime_error("Cannot start owned FLV fixture.");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    if (socket->property("sent").toBool())
                        return;
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n"))
                        return;
                    socket->setProperty("sent", true);
                    ++requests;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                                  QByteArray::number(header.size() + tag.size() * 1000) + "\r\n\r\n");
                    socket->write(header);
                    auto* timer = new QTimer(socket);
                    QObject::connect(timer, &QTimer::timeout, socket, [this, socket, timer] {
                        if (socket->state() != QAbstractSocket::ConnectedState) {
                            timer->stop();
                            return;
                        }
                        socket->write(tag);
                    });
                    timer->start(10);
                });
            }
        });
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/feed.flv";
    }
};
template <class Predicate> bool pump(Predicate predicate, int milliseconds = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
} // namespace

TEST(LivePlaylist, StrictSameOriginSequenceMasterAndUnsupportedVariants) {
    const QUrl origin("https://example.test/live/index.m3u8");
    auto parse = [&](const QByteArray& text) { return live::parsePlaylist(text, origin, origin); };
    const QByteArray base = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:42\n";
    auto playlist = parse(base + "#EXTINF:2,\na.ts\n#EXTINF:2,\nb.ts\n#EXT-X-ENDLIST\n");
    EXPECT_EQ(playlist.sequence, 42);
    ASSERT_EQ(playlist.segments.size(), 2u);
    EXPECT_EQ(playlist.segments[0], QUrl("https://example.test/live/a.ts"));
    EXPECT_TRUE(playlist.ended);
    auto master = parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100\na.m3u8\n"
                        "#EXT-X-STREAM-INF:BANDWIDTH=200\nb.m3u8\n");
    EXPECT_EQ(master.variant, QUrl("https://example.test/live/b.m3u8"));
    for (const auto* tag :
         {"#EXT-X-KEY:METHOD=AES-128,URI=key", "#EXT-X-MAP:URI=init.mp4", "#EXT-X-BYTERANGE:100@0",
          "#EXT-X-DISCONTINUITY", "#EXT-X-GAP", "#EXT-X-PART:DURATION=0.5,URI=a.ts"})
        EXPECT_THROW(parse(base + tag + "\n#EXTINF:2,\na.ts\n"), std::exception) << tag;
    EXPECT_THROW(parse(base + "#EXTINF:2,\nhttps://foreign.test/a.ts\n"), std::exception);
    EXPECT_THROW(parse(base + "#EXTINF:2,\nhttps://example.test:444/a.ts\n"), std::exception);
    EXPECT_THROW(parse(base + "#EXTINF:2,\nhttp://example.test/a.ts\n"), std::exception);
    EXPECT_THROW(parse(base + "#EXTINF:nan,\na.ts\n"), std::exception);
    EXPECT_THROW(parse(base + "#EXT-X-MEDIA-SEQUENCE:43\n#EXTINF:2,\na.ts\n"), std::exception);
    EXPECT_THROW(parse(QByteArray(1024 * 1024 + 1, 'x')), std::exception);
}

TEST(LiveStorage, StrictPublicConfigurationCodecAndActualVersionSeventeenMigration) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    const auto database = root / "tasks.db";
    Task task;
    task.id = newTaskId();
    task.spec = liveSpec(root / "record.flv");
    task.spec.cookie = "owned-live-cookie-never-save";
    task.spec.live.maxBytes = 2 * 1024 * 1024 + 1;
    {
        auto store = makeSqliteRepository(database);
        store->save(task);
        const auto rows = store->load();
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0].spec.kind, TransferKind::Live);
        EXPECT_EQ(rows[0].spec.live, task.spec.live);
        EXPECT_TRUE(rows[0].spec.cookie.empty());
        auto encoded = ui::encodeTaskList(rows);
        EXPECT_FALSE(encoded.contains("owned-live-cookie"));
        auto decoded = ui::decodeTaskList(encoded, [](const QString&) { return DownloadSpec{}; });
        ASSERT_EQ(decoded.downloads.size(), 1u);
        EXPECT_EQ(decoded.downloads[0].kind, TransferKind::Live);
        EXPECT_EQ(decoded.downloads[0].live, task.spec.live);
        encoded.replace("\"format\":1", "\"format\":2");
        EXPECT_THROW(ui::decodeTaskList(encoded, [](const QString&) { return DownloadSpec{}; }),
                     std::exception);
        auto invalid = task;
        invalid.spec.kind = TransferKind::Http;
        EXPECT_THROW(store->save(invalid), std::exception);
    }
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(database.string().c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
                           "UPDATE tasks SET kind=0;"
                           "ALTER TABLE tasks DROP COLUMN live_format;"
                           "ALTER TABLE tasks DROP COLUMN live_duration;"
                           "ALTER TABLE tasks DROP COLUMN live_max_bytes; PRAGMA user_version=17;",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(db);
    {
        auto store = makeSqliteRepository(database);
        auto rows = store->load();
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0].spec.live, LiveRecordingOptions{});
    }
    bool backup = false;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        backup |= entry.path().filename().string().find(".before-v18-") != std::string::npos;
    EXPECT_TRUE(backup);
    ASSERT_EQ(sqlite3_open(database.string().c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "UPDATE tasks SET live_duration='broken';", nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(db);
    EXPECT_THROW(makeSqliteRepository(database)->load(), std::exception);
}

TEST(LiveUi, ActualDialogServiceFinishPauseRestartAndPublicationRace) {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char name[] = "LiveWidgets";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName(name);
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    Feed feed;
    auto spec = liveSpec(root / L"录制.flv");
    spec.url = feed.url();
    AddDownloadDialog dialog(false);
    dialog.setSpecification(spec);
    EXPECT_EQ(dialog.specification().kind, TransferKind::Live);
    EXPECT_EQ(dialog.specification().live, spec.live);
    auto* tabs = dialog.findChild<QTabWidget*>("downloadOptionsTabs");
    ASSERT_NE(tabs, nullptr);
    for (int n = 0; n < tabs->count(); ++n)
        if (tabs->tabText(n) == "Live recording")
            tabs->setCurrentIndex(n);
    dialog.show();
    QCoreApplication::processEvents();
    if (qEnvironmentVariableIsSet("MASKPILOT_TEST_LIVE_SCREENSHOT"))
        EXPECT_TRUE(dialog.grab().save(qEnvironmentVariable("MASKPILOT_TEST_LIVE_SCREENSHOT")));
    DownloadService service(makeSqliteRepository(root / "tasks.db"), makeUnifiedTransfer, root / "work");
    const auto id = service.add(dialog.specification()).get();
    ASSERT_TRUE(pump([&] { return !service.snapshot().empty() && service.snapshot()[0].downloaded > 200; }));
    service.finishRecording(id);
    ASSERT_TRUE(pump([&] { return service.snapshot()[0].state == TaskState::Completed; }))
        << service.snapshot()[0].error;
    EXPECT_FALSE(std::filesystem::exists(root / "work" / id / "live-recording.tmp"));
    QFile output(QString::fromStdWString(spec.destination.wstring()));
    ASSERT_TRUE(output.open(QIODevice::ReadOnly));
    auto bytes = output.readAll();
    output.close();
    EXPECT_TRUE(bytes.startsWith(feed.header));
    EXPECT_EQ((bytes.size() - feed.header.size()) % feed.tag.size(), 0);
    spec.destination = root / "restarted.flv";
    const auto second = service.add(spec).get();
    auto state = [&]() {
        for (const auto& task : service.snapshot())
            if (task.id == second)
                return task;
        return Task{};
    };
    ASSERT_TRUE(pump([&] { return state().downloaded > 200; }));
    service.pause(second);
    ASSERT_TRUE(pump([&] { return state().state == TaskState::Paused; }));
    EXPECT_FALSE(std::filesystem::exists(spec.destination));
    const auto requests = feed.requests;
    service.resume(second);
    ASSERT_TRUE(pump([&] { return feed.requests > requests && state().downloaded > 200; }));
    service.finishRecording(second);
    ASSERT_TRUE(pump([&] { return state().state == TaskState::Completed; })) << state().error;
    QFile restarted(QString::fromStdWString(spec.destination.wstring()));
    ASSERT_TRUE(restarted.open(QIODevice::ReadOnly));
    EXPECT_EQ(restarted.readAll().count("FLV"), 1);
    restarted.close();
    spec.destination = root / "preserved.flv";
    const auto race = service.add(spec).get();
    ASSERT_TRUE(pump([&] { return feed.requests > requests + 1; }));
    std::ofstream(spec.destination, std::ios::binary) << "preserved";
    service.finishRecording(race);
    ASSERT_TRUE(pump([&] {
        for (const auto& task : service.snapshot())
            if (task.id == race)
                return task.state == TaskState::NeedsAttention;
        return false;
    }));
    QFile preserved(QString::fromStdWString(spec.destination.wstring()));
    ASSERT_TRUE(preserved.open(QIODevice::ReadOnly));
    EXPECT_EQ(preserved.readAll(), "preserved");
    preserved.close();
    spec.destination = root / "cancelled.flv";
    const auto cancelled = service.add(spec).get();
    ASSERT_TRUE(pump([&] {
        for (const auto& task : service.snapshot())
            if (task.id == cancelled)
                return task.downloaded > 100;
        return false;
    }));
    service.cancel(cancelled);
    ASSERT_TRUE(pump([&] {
        for (const auto& task : service.snapshot())
            if (task.id == cancelled)
                return task.state == TaskState::Cancelled;
        return false;
    }));
    EXPECT_FALSE(std::filesystem::exists(spec.destination));
    EXPECT_FALSE(std::filesystem::exists(root / "work" / cancelled / "live-recording.tmp"));
}
