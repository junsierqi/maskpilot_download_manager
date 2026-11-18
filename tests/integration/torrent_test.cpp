#include "../../src/infrastructure/src/torrent/torrent_internal.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iterator>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/torrent_engine.h>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace lt = libtorrent;
using namespace std::chrono_literals;
using Tcp = boost::asio::ip::tcp;
using Udp = boost::asio::ip::udp;

unsigned short availableTcpAndUdpPort(const std::string& address = "127.0.0.1") {
    boost::asio::io_context context;
    boost::system::error_code lastError;
    thread_local std::mt19937 random(std::random_device{}());
    std::uniform_int_distribution<unsigned short> portCandidates(10240, 49151);
    for (int attempt = 0; attempt < 64; ++attempt) {
        // TCP port 0 can repeatedly select one contiguous range that UDP excludes on Windows.
        // Sample explicit unprivileged ports below the default dynamic range instead.
        const auto port = portCandidates(random);
        boost::system::error_code error;
        Tcp::acceptor tcp(context);
        tcp.open(Tcp::v4(), error);
        if (error) {
            lastError = error;
            continue;
        }
        tcp.bind({boost::asio::ip::make_address(address), port}, error);
        if (!error)
            tcp.listen(boost::asio::socket_base::max_listen_connections, error);
        if (error) {
            lastError = error;
            continue;
        }
        Udp::socket udp(context);
        udp.open(Udp::v4(), error);
        if (!error)
            udp.bind({boost::asio::ip::make_address(address), port}, error);
        if (!error)
            return port; // Both owned sockets are released immediately before libtorrent binds.
        lastError = error;
    }
    throw std::runtime_error("No loopback port permits both TCP and UDP: " + lastError.message());
}

std::string pathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
std::filesystem::path nativePath(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("maskpilot-bt-test-" + maskpilot::newTaskId());
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
        if (!error && absolute.parent_path() == std::filesystem::temp_directory_path().lexically_normal() &&
            absolute.filename().string().starts_with("maskpilot-bt-test-"))
            std::filesystem::remove_all(absolute, error);
    }
};
std::vector<char> contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read test payload.");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void writeBytes(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output)
        throw std::runtime_error("Cannot write test payload.");
}
lt::settings_pack isolatedSettings(const std::string& listen = {}, const std::string& address = "127.0.0.1") {
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces,
                     listen.empty() ? address + ":" + std::to_string(availableTcpAndUdpPort(address))
                                    : listen);
    settings.set_str(lt::settings_pack::outgoing_interfaces, address);
    settings.set_bool(lt::settings_pack::enable_dht, false);
    settings.set_bool(lt::settings_pack::enable_lsd, false);
    settings.set_bool(lt::settings_pack::enable_upnp, false);
    settings.set_bool(lt::settings_pack::enable_natpmp, false);
    settings.set_bool(lt::settings_pack::enable_outgoing_utp, false);
    settings.set_bool(lt::settings_pack::enable_incoming_utp, false);
    settings.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    settings.set_int(lt::settings_pack::alert_mask,
                     static_cast<int>(static_cast<std::uint32_t>(
                         lt::alert_category::error | lt::alert_category::peer | lt::alert_category::connect |
                         lt::alert_category::tracker | lt::alert_category::status)));
    settings.set_int(lt::settings_pack::alert_queue_size, 256);
    settings.set_int(lt::settings_pack::unchoke_interval, 1);
    settings.set_int(lt::settings_pack::optimistic_unchoke_interval, 2);
    return settings;
}
template <typename Predicate> bool waitUntil(Predicate&& predicate, std::chrono::seconds timeout = 20s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

// A single local tracker returns only this fixture's seed. No DNS, discovery or public endpoints.
class LocalTracker {
    boost::asio::io_context context_;
    Tcp::acceptor acceptor_{context_, {boost::asio::ip::make_address("127.0.0.1"), 0}};
    std::atomic_bool stopping_{false};
    std::atomic_uint requests_{0};
    unsigned short port_ = acceptor_.local_endpoint().port();
    std::jthread worker_;

  public:
    explicit LocalTracker(unsigned short seedPort) {
        std::string peer{"\x7f\x00\x00\x01", 4};
        peer += static_cast<char>(seedPort >> 8);
        peer += static_cast<char>(seedPort & 255);
        lt::entry response;
        response["interval"] = 1;
        response["min interval"] = 1;
        response["peers"] = peer;
        std::vector<char> body;
        lt::bencode(std::back_inserter(body), response);
        const auto header =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n";
        worker_ = std::jthread([this, header, body = std::move(body)] {
            while (!stopping_) {
                boost::system::error_code error;
                Tcp::socket socket(context_);
                acceptor_.accept(socket, error);
                if (error || stopping_)
                    break;
                socket.non_blocking(true, error);
                std::string request;
                std::array<char, 1024> buffer{};
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (!stopping_ && std::chrono::steady_clock::now() < deadline && request.size() < 16384 &&
                       request.find("\r\n\r\n") == std::string::npos) {
                    const auto read = socket.read_some(boost::asio::buffer(buffer), error);
                    if (!error)
                        request.append(buffer.data(), read);
                    else if (error != boost::asio::error::would_block &&
                             error != boost::asio::error::try_again)
                        break;
                    else
                        std::this_thread::sleep_for(1ms);
                }
                if (stopping_)
                    break;
                ++requests_;
                socket.non_blocking(false, error);
                const std::array<boost::asio::const_buffer, 2> buffers{boost::asio::buffer(header),
                                                                       boost::asio::buffer(body)};
                boost::asio::write(socket, buffers, error);
            }
        });
    }
    ~LocalTracker() {
        stopping_ = true;
        boost::system::error_code error;
        Tcp::socket wake(context_);
        wake.connect({boost::asio::ip::make_address("127.0.0.1"), port_}, error);
        if (worker_.joinable())
            worker_.join();
        acceptor_.close(error);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/announce"; }
    unsigned requests() const { return requests_; }
};

class TorrentFixture {
  public:
    TemporaryDirectory temporary;
    const std::string bundle;
    std::vector<std::pair<std::string, std::vector<char>>> payload;
    std::unique_ptr<lt::session> seed;
    lt::torrent_handle seedHandle;
    std::unique_ptr<LocalTracker> tracker;
    lt::entry metadataEntry;
    maskpilot::TorrentMetadata metadata;
    std::string magnet;

    explicit TorrentFixture(lt::create_flags_t flags = lt::create_torrent::v1_only,
                            std::string name = "bundle", unsigned char salt = 19)
        : bundle(std::move(name)) {
        const auto format = (flags & lt::create_torrent::v1_only)   ? "v1-only"
                            : (flags & lt::create_torrent::v2_only) ? "v2-only"
                                                                    : "hybrid";
        try {
            for (const auto& [relativeName, size] :
                 {std::pair{"alpha.bin", 512 * 1024 + 113}, std::pair{"nested/beta.bin", 384 * 1024 + 227},
                  std::pair{"empty.bin", 0}}) {
                std::vector<char> bytes(static_cast<std::size_t>(size));
                for (std::size_t n = 0; n < bytes.size(); ++n)
                    bytes[n] = static_cast<char>((n * 29 + salt) % 251);
                payload.emplace_back(bundle + "/" + relativeName, std::move(bytes));
                writeBytes(temporary.path / "seed" / nativePath(payload.back().first), payload.back().second);
            }
            std::vector<lt::create_file_entry> files;
            for (const auto& [path, bytes] : payload)
                files.emplace_back(path, static_cast<std::int64_t>(bytes.size()));
            lt::create_torrent builder(std::move(files), 16 * 1024, flags);
            lt::error_code error;
            lt::set_piece_hashes(builder, pathText(temporary.path / "seed"), error);
            if (error)
                throw std::runtime_error("Synthetic torrent hashing failed: " + error.message());
            metadataEntry = builder.generate();
            std::vector<char> bytes;
            lt::bencode(std::back_inserter(bytes), metadataEntry);
            auto params = lt::load_torrent_buffer(lt::span<char const>(bytes.data(), bytes.size()));
            params.save_path = pathText(temporary.path / "seed");
            params.flags &= ~lt::torrent_flags::auto_managed;
            params.flags &= ~lt::torrent_flags::paused;
            const auto readyDeadline = std::chrono::steady_clock::now() + 20s;
            std::vector<std::string> seedAlerts;
            bool ready = false;
            int listenerAttempts = 0;
            for (; listenerAttempts < 8 && std::chrono::steady_clock::now() < readyDeadline;) {
                ++listenerAttempts;
                seed = std::make_unique<lt::session>(lt::session_params(isolatedSettings()));
                error.clear();
                seedHandle = seed->add_torrent(params, error);
                if (error)
                    break;
                bool bindFailed = false;
                while (std::chrono::steady_clock::now() < readyDeadline) {
                    std::vector<lt::alert*> alerts;
                    seed->pop_alerts(&alerts);
                    for (const auto* alert : alerts) {
                        bindFailed |= lt::alert_cast<lt::listen_failed_alert>(alert) != nullptr;
                        seedAlerts.push_back("attempt=" + std::to_string(listenerAttempts) + " " +
                                             alert->message().substr(0, 200));
                        if (seedAlerts.size() > 8)
                            seedAlerts.erase(seedAlerts.begin());
                    }
                    if (seedHandle.status().is_seeding && seed->listen_port() != 0) {
                        ready = true;
                        break;
                    }
                    if (bindFailed && seed->listen_port() == 0)
                        break;
                    std::this_thread::sleep_for(10ms);
                }
                if (ready || !bindFailed || std::chrono::steady_clock::now() >= readyDeadline)
                    break;
                // A release-to-bind race can still occur. Retry only a reported bind failure,
                // within the original deadline, and verify the payload again in the new session.
                seedHandle = lt::torrent_handle{};
                seed.reset();
            }
            if (!ready) {
                std::ostringstream detail;
                detail << "seed readiness failed: create_flags=" << flags << " add_errc=" << error.value()
                       << " add_error=" << error.message()
                       << " listen_port=" << (seed ? seed->listen_port() : 0)
                       << " listener_attempts=" << listenerAttempts;
                if (seedHandle.is_valid()) {
                    const auto status = seedHandle.status();
                    detail << " state=" << static_cast<int>(status.state)
                           << " is_finished=" << status.is_finished << " is_seeding=" << status.is_seeding
                           << " wanted=" << status.total_wanted_done << '/' << status.total_wanted
                           << " done=" << status.total_done << '/' << status.total
                           << " pieces=" << status.num_pieces << " has_metadata=" << status.has_metadata
                           << " flags=" << status.flags << " errc=" << status.errc.value()
                           << " error=" << status.errc.message().substr(0, 256)
                           << " error_file=" << static_cast<int>(status.error_file);
                }
                for (const auto& alert : seedAlerts)
                    detail << " alert=" << alert;
                throw std::runtime_error(detail.str());
            }
            tracker = std::make_unique<LocalTracker>(seed->listen_port());
            metadataEntry["announce"] = tracker->url();
            bytes.clear();
            lt::bencode(std::back_inserter(bytes), metadataEntry);
            metadata = std::make_shared<const std::vector<char>>(std::move(bytes));
            magnet = lt::make_magnet_uri(params) + "&x.pe=127.0.0.1:" + std::to_string(seed->listen_port());
        } catch (const std::exception& error) {
            throw std::runtime_error("Synthetic " + std::string(format) + " fixture (" + bundle +
                                     "): " + error.what());
        }
    }
    maskpilot::Task task(const std::filesystem::path& destination, bool magnetOnly = false) const {
        maskpilot::Task result;
        result.id = maskpilot::newTaskId();
        result.spec.kind = maskpilot::TransferKind::BitTorrent;
        result.spec.url = magnetOnly ? magnet : "torrent:" + pathText(temporary.path / "source.torrent");
        result.spec.destination = destination;
        if (!magnetOnly)
            result.spec.torrent.metadata = metadata;
        return result;
    }
    void expectPayload(const std::filesystem::path& destination) const {
        for (const auto& [path, bytes] : payload) {
            ASSERT_TRUE(std::filesystem::is_regular_file(destination / nativePath(path))) << path;
            EXPECT_EQ(contents(destination / nativePath(path)), bytes) << path;
        }
    }
};

maskpilot::TransportFactory localFactory() {
    maskpilot::TorrentRuntimeOptions options;
    options.enableDiscovery = false;
    options.listenInterfaces = "127.0.0.1:0";
    return maskpilot::makeTorrentTransferFactory(options);
}
struct TransferResult {
    maskpilot::TransferEvent terminal;
    maskpilot::TransferEvent lastProgress;
    maskpilot::TorrentMetadata metadata;
    std::shared_ptr<const maskpilot::TransferDescription> description;
    int terminals = 0;
    bool timedOut = false;
    bool sawProgress = false;
    std::string producerDiagnostics;
    std::string diagnostics() const {
        const auto& event = sawProgress ? lastProgress : terminal;
        std::ostringstream text;
        text << "last phase=" << maskpilot::stateName(event.phase) << " downloaded=" << event.downloaded
             << " total=" << event.total << " peers=" << event.peers << " seeds=" << event.seeds
             << " metadata=" << static_cast<bool>(metadata)
             << " terminal error=" << terminal.error.substr(0, 256);
        if (!producerDiagnostics.empty())
            text << " producer={" << producerDiagnostics.substr(0, 2400) << '}';
        return text.str();
    }
};
TransferResult run(const maskpilot::TransportFactory& factory, const maskpilot::Task& task,
                   const std::filesystem::path& work, maskpilot::TransferControl& control,
                   const std::function<void(const maskpilot::TransferEvent&)>& action = {},
                   std::chrono::seconds timeout = 30s) {
    TransferResult result;
    std::atomic_bool expired = false;
    std::jthread watchdog([&](std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        if (!stop.stop_requested()) {
            expired = true;
            control.cancel = true;
        }
    });
    factory()->run(task, work, control, [&](auto event) {
        if (event.outcome == maskpilot::TransferOutcome::Progress) {
            result.lastProgress = event;
            result.sawProgress = true;
        }
        if (event.torrentMetadata)
            result.metadata = event.torrentMetadata;
        if (event.description)
            result.description = event.description;
        if (action)
            action(event);
        if (event.outcome != maskpilot::TransferOutcome::Progress) {
            ++result.terminals;
            result.terminal = std::move(event);
        }
    });
    watchdog.request_stop();
    watchdog.join();
    result.timedOut = expired;
    return result;
}
void expectComplete(const TransferResult& result) {
    EXPECT_FALSE(result.timedOut) << result.diagnostics();
    EXPECT_EQ(result.terminals, 1) << result.diagnostics();
    EXPECT_EQ(result.terminal.outcome, maskpilot::TransferOutcome::Complete) << result.diagnostics();
}
maskpilot::DownloadSpec specWithMetadata(std::vector<char> bytes) {
    maskpilot::DownloadSpec spec;
    spec.kind = maskpilot::TransferKind::BitTorrent;
    spec.url = "torrent:" + pathText(std::filesystem::temp_directory_path() / "fixture.torrent");
    spec.destination = std::filesystem::temp_directory_path() / "unused-torrent-destination";
    spec.torrent.metadata = std::make_shared<const std::vector<char>>(std::move(bytes));
    return spec;
}
std::vector<char> encode(const lt::entry& entry) {
    std::vector<char> bytes;
    lt::bencode(std::back_inserter(bytes), entry);
    return bytes;
}
class RecordingProxyFixture {
    TemporaryDirectory temporary_;
    QProcess process_;
    std::filesystem::path state_ = temporary_.path / "proxy-state.json";
    std::string endpoint_;

  public:
    explicit RecordingProxyFixture(const char* kind, const std::string& username = {},
                                   const std::string& password = {}) {
#ifdef MASKPILOT_TEST_PROXY_FIXTURE
        const auto script = QString::fromUtf8(MASKPILOT_TEST_PROXY_FIXTURE);
#else
        const auto script = QString::fromStdWString(
            (std::filesystem::path(__FILE__).parent_path().parent_path() / "support" / "proxy_fixture.py")
                .wstring());
#endif
#ifdef MASKPILOT_TEST_PYTHON
        const auto python = QString::fromUtf8(MASKPILOT_TEST_PYTHON);
#else
        const auto python = qEnvironmentVariable("MASKPILOT_TEST_PYTHON", "python");
#endif
        QStringList arguments{script,    "--serve",
                              "--kind",  kind,
                              "--state", QString::fromStdWString(state_.wstring()),
                              "--map",   "proxy-origin.invalid=127.0.0.1"};
        if (!username.empty())
            arguments << "--username" << QString::fromStdString(username) << "--password"
                      << QString::fromStdString(password);
#ifdef _WIN32
        process_.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* arguments) { arguments->flags |= CREATE_NO_WINDOW; });
#endif
        process_.start(python, arguments);
        if (!process_.waitForStarted(5000))
            throw std::runtime_error("Cannot start the owned recording proxy fixture.");
        QByteArray ready;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!ready.contains('\n') && std::chrono::steady_clock::now() < deadline) {
            process_.waitForReadyRead(100);
            ready += process_.readAllStandardOutput();
        }
        const auto value = QJsonDocument::fromJson(ready.trimmed()).object();
        const auto port = value.value("port").toInt();
        if (value.value("host").toString() != "127.0.0.1" || port <= 0 || port > 65535)
            throw std::runtime_error("Recording proxy fixture did not report a valid owned listener.");
        endpoint_ = std::string(kind == std::string("http") ? "http://" : "socks5h://") +
                    "127.0.0.1:" + std::to_string(port);
    }
    ~RecordingProxyFixture() {
        process_.closeWriteChannel();
        if (!process_.waitForFinished(5000)) {
            process_.kill();
            process_.waitForFinished(5000);
        }
    }
    const std::string& endpoint() const { return endpoint_; }
    QJsonObject snapshot() const {
        QFile file(QString::fromStdWString(state_.wstring()));
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read recording proxy fixture counters.");
        return QJsonDocument::fromJson(file.readAll()).object();
    }
};
std::string trackerOnlyMagnet(const TorrentFixture& fixture, bool remoteDns = false) {
    auto params =
        lt::load_torrent_buffer(lt::span<char const>(fixture.metadata->data(), fixture.metadata->size()));
    params.peers.clear();
    params.dht_nodes.clear();
    params.url_seeds.clear();
    if (remoteDns) {
        for (auto& tracker : params.trackers) {
            const auto position = tracker.find("127.0.0.1");
            if (position != std::string::npos)
                tracker.replace(position, 9, "proxy-origin.invalid");
        }
    }
    return lt::make_magnet_uri(params);
}
} // namespace

TEST(TorrentProxy, AuthenticatedHttpAndSocksFetchMagnetMetadataAndPayloadWithRemoteDns) {
    for (const auto* kind : {"http", "socks5"}) {
        SCOPED_TRACE(kind);
        TorrentFixture fixture;
        RecordingProxyFixture proxy(kind, "fixture-user", "fixture-secret");
        auto task = fixture.task(fixture.temporary.path / "proxied", true);
        task.spec.url = trackerOnlyMagnet(fixture, true);
        task.spec.proxy = proxy.endpoint();
        task.spec.proxyUsername = "fixture-user";
        task.spec.proxyPassword = "fixture-secret";
        task.spec.proxyAuthenticationRequired = true;
        maskpilot::TransferControl control;
        const auto result = run(localFactory(), task, fixture.temporary.path / "work" / task.id, control);
        expectComplete(result);
        EXPECT_NE(result.metadata, nullptr);
        fixture.expectPayload(task.spec.destination);
        const auto state = proxy.snapshot();
        EXPECT_GT(state.value("auth_accepted").toInt(), 0);
        EXPECT_GT(state.value("forwarded_bytes").toDouble(), 500000);
        bool tracker = false, peer = false;
        for (const auto& entry : state.value("destinations").toArray()) {
            const auto destination = entry.toObject();
            tracker |= destination.value("host").toString() == "proxy-origin.invalid";
            peer |= destination.value("port").toInt() == fixture.seed->listen_port();
        }
        EXPECT_TRUE(tracker) << "Tracker DNS must be performed by the proxy fixture.";
        EXPECT_TRUE(peer) << "Payload peers must use the same proxy route.";
    }
}

TEST(TorrentProxy, ConcurrentDirectAndDifferentProxyRoutesStayIsolated) {
    TorrentFixture fixture;
    RecordingProxyFixture http("http"), socks("socks5");
    auto direct = fixture.task(fixture.temporary.path / "direct");
    auto first = fixture.task(fixture.temporary.path / "http");
    auto second = fixture.task(fixture.temporary.path / "socks");
    first.spec.proxy = http.endpoint();
    second.spec.proxy = socks.endpoint();
    const auto factory = localFactory();
    maskpilot::TransferControl directControl, firstControl, secondControl;
    auto start = [&](const maskpilot::Task& task, maskpilot::TransferControl& control) {
        return std::async(std::launch::async, [&, task] {
            return run(factory, task, fixture.temporary.path / "work" / task.id, control);
        });
    };
    auto directResult = start(direct, directControl);
    auto firstResult = start(first, firstControl);
    auto secondResult = start(second, secondControl);
    expectComplete(directResult.get());
    expectComplete(firstResult.get());
    expectComplete(secondResult.get());
    for (const auto& task : {direct, first, second})
        fixture.expectPayload(task.spec.destination);
    EXPECT_GT(http.snapshot().value("forwarded_bytes").toDouble(), 500000);
    EXPECT_GT(socks.snapshot().value("forwarded_bytes").toDouble(), 500000);
}

TEST(TorrentProxy, DifferentCredentialsAtOneEndpointNeverShareAnActiveSession) {
    TorrentFixture fixture;
    RecordingProxyFixture proxy("socks5", "fixture-user", "fixture-secret");
    auto good = fixture.task(fixture.temporary.path / "good");
    good.spec.proxy = proxy.endpoint();
    good.spec.proxyUsername = "fixture-user";
    good.spec.proxyPassword = "fixture-secret";
    auto bad = fixture.task(fixture.temporary.path / "bad");
    bad.spec = good.spec;
    bad.spec.destination = fixture.temporary.path / "bad";
    bad.spec.proxyPassword = "wrong-secret";
    const auto factory = localFactory();
    maskpilot::TransferControl goodControl, badControl;
    auto valid = std::async(std::launch::async, [&] {
        return run(factory, good, fixture.temporary.path / "work" / good.id, goodControl);
    });
    const auto invalid = run(factory, bad, fixture.temporary.path / "work" / bad.id, badControl);
    expectComplete(valid.get());
    EXPECT_FALSE(invalid.timedOut) << invalid.diagnostics();
    EXPECT_EQ(invalid.terminals, 1);
    EXPECT_EQ(invalid.terminal.outcome, maskpilot::TransferOutcome::Attention);
    EXPECT_EQ(invalid.terminal.error.find("wrong-secret"), std::string::npos);
    EXPECT_EQ(invalid.terminal.error.find("fixture-secret"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(bad.spec.destination));
    fixture.expectPayload(good.spec.destination);
    const auto state = proxy.snapshot();
    EXPECT_GT(state.value("auth_accepted").toInt(), 0);
    EXPECT_GT(state.value("auth_rejected").toInt(), 0);
}

TEST(TorrentProxy, DifferentOwnersNeverShareAnActiveSessionEvenWithIdenticalProxyCredentials) {
    TorrentFixture fixture;
    RecordingProxyFixture proxy("http", "fixture-user", "fixture-secret");
    auto first = fixture.task(fixture.temporary.path / "first-owner");
    first.ownerId = maskpilot::newTaskId();
    first.spec.proxy = proxy.endpoint();
    first.spec.proxyUsername = "fixture-user";
    first.spec.proxyPassword = "fixture-secret";
    auto second = fixture.task(fixture.temporary.path / "second-owner");
    second.ownerId = maskpilot::newTaskId();
    second.spec = first.spec;
    second.spec.destination = fixture.temporary.path / "second-owner";
    ASSERT_NE(first.ownerId, second.ownerId);
    const auto factory = localFactory();
    maskpilot::TransferControl firstControl, secondControl;
    auto active = std::async(std::launch::async, [&] {
        return run(factory, first, fixture.temporary.path / "work" / first.id, firstControl);
    });
    const auto parallel = run(factory, second, fixture.temporary.path / "work" / second.id, secondControl);
    expectComplete(active.get());
    expectComplete(parallel);
    fixture.expectPayload(first.spec.destination);
    fixture.expectPayload(second.spec.destination);
    const auto state = proxy.snapshot();
    EXPECT_GT(state.value("auth_accepted").toInt(), 0);
    EXPECT_GT(state.value("forwarded_bytes").toDouble(), 1000000);
}

TEST(TorrentProxy, MissingSecretsAndUnsupportedHttpsFailBeforeContactingTheSource) {
    TorrentFixture fixture;
    for (const bool missing : {true, false}) {
        auto task = fixture.task(fixture.temporary.path / (missing ? "missing-secret" : "https-proxy"), true);
        task.spec.url = trackerOnlyMagnet(fixture);
        task.spec.proxy = missing ? "http://127.0.0.1:1" : "https://127.0.0.1:1";
        task.spec.proxyAuthenticationRequired = missing;
        maskpilot::TransferControl control;
        const auto start = std::chrono::steady_clock::now();
        const auto result = run(localFactory(), task, fixture.temporary.path / "work" / task.id, control);
        EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
        EXPECT_FALSE(result.timedOut);
        EXPECT_EQ(result.terminal.outcome, maskpilot::TransferOutcome::Attention);
        EXPECT_EQ(fixture.tracker->requests(), 0u);
        EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
    }
}

// Source-only parser regression: no session, resolver or fixture server is created.
TEST(TorrentMetadataParser, CachedMetadataPreservesDeduplicatedBoundedMagnetWebSeeds) {
    lt::entry metainfo;
    metainfo["info"]["name"] = "parser-fixture.bin";
    metainfo["info"]["length"] = 1;
    metainfo["info"]["piece length"] = 16 * 1024;
    metainfo["info"]["pieces"] = std::string(20, 'h');
    metainfo["url-list"] = "http://seed.invalid/embedded/";
    auto spec = specWithMetadata(encode(metainfo));
    auto original = maskpilot::torrent_internal::parse(spec);
    auto magnetParams = original.params;
    magnetParams.url_seeds = {"http://seed.invalid/embedded/", "http://seed.invalid/magnet/",
                              "http://seed.invalid/magnet/"};
    spec.url = lt::make_magnet_uri(magnetParams);
    auto merged = maskpilot::torrent_internal::parse(spec);
    EXPECT_EQ(merged.params.url_seeds.size(), 2u);
    EXPECT_EQ(std::count(merged.params.url_seeds.begin(), merged.params.url_seeds.end(),
                         "http://seed.invalid/magnet/"),
              1);
    spec.torrent.metadata = maskpilot::torrent_internal::metadataFrom(merged.params);
    const auto roundTrip = maskpilot::torrent_internal::parse(spec);
    EXPECT_EQ(roundTrip.params.url_seeds, merged.params.url_seeds);
    magnetParams.url_seeds.clear();
    for (int n = 0; n < 256; ++n)
        magnetParams.url_seeds.push_back("http://seed.invalid/source/" + std::to_string(n) + "/");
    spec.url = lt::make_magnet_uri(magnetParams);
    EXPECT_THROW(maskpilot::torrent_internal::parse(spec), std::invalid_argument);
}

TEST(TorrentProxy, PeerOnlyHttpMagnetReportsRejectedTunnelsWithoutBypassingTheProxy) {
    TorrentFixture fixture;
    boost::asio::io_context deadContext;
    Tcp::acceptor deadPeer(deadContext);
    deadPeer.open(Tcp::v4());
    // Keep a real non-listening port owned for the entire test. Native peer_list
    // silently ignores port 1, so that value cannot prove alternate-peer handling.
    deadPeer.bind({boost::asio::ip::make_address("127.0.0.2"), 0});
    const auto deadPort = deadPeer.local_endpoint().port();
    ASSERT_GT(deadPort, 1024);
    RecordingProxyFixture proxy("http", "fixture-user", "fixture-secret");
    auto params =
        lt::load_torrent_buffer(lt::span<char const>(fixture.metadata->data(), fixture.metadata->size()));
    params.trackers.clear();
    params.tracker_tiers.clear();
    params.url_seeds.clear();
    params.dht_nodes.clear();
    params.peers = {{boost::asio::ip::make_address("127.0.0.2"), deadPort},
                    {boost::asio::ip::make_address("127.0.0.1"), fixture.seed->listen_port()}};
    const auto magnet = lt::make_magnet_uri(params);
    ASSERT_EQ(magnet.find("tr="), std::string::npos);
    ASSERT_EQ(magnet.find("ws="), std::string::npos);
    ASSERT_NE(magnet.find("x.pe="), std::string::npos);
    auto valid = fixture.task(fixture.temporary.path / "valid-peer-only", true);
    valid.spec.url = magnet;
    valid.spec.proxy = proxy.endpoint();
    valid.spec.proxyUsername = "fixture-user";
    valid.spec.proxyPassword = "fixture-secret";
    valid.spec.proxyAuthenticationRequired = true;
    const auto factory = localFactory();
    maskpilot::TransferControl validControl;
    const auto complete = run(factory, valid, fixture.temporary.path / "work" / valid.id, validControl);
    expectComplete(complete);
    ASSERT_NE(complete.metadata, nullptr);
    fixture.expectPayload(valid.spec.destination);
    const auto before = proxy.snapshot();
    EXPECT_GT(before.value("auth_accepted").toInt(), 0);
    EXPECT_GT(before.value("forwarded_bytes").toDouble(), 500000);
    bool deadAttempted = false, healthyAttempted = false;
    for (const auto& value : before.value("destinations").toArray()) {
        const auto destination = value.toObject();
        deadAttempted |= destination.value("host").toString() == "127.0.0.2" &&
                         destination.value("port").toInt() == deadPort;
        healthyAttempted |= destination.value("host").toString() == "127.0.0.1" &&
                            destination.value("port").toInt() == fixture.seed->listen_port();
    }
    EXPECT_TRUE(deadAttempted);
    EXPECT_TRUE(healthyAttempted);
    for (const bool cachedMetadata : {false, true}) {
        SCOPED_TRACE(cachedMetadata ? "cached metadata" : "metadata exchange");
        const auto destination =
            fixture.temporary.path / (cachedMetadata ? "invalid-cached" : "invalid-peer-only");
        auto invalid = fixture.task(destination, true);
        invalid.spec = valid.spec;
        invalid.spec.destination = destination;
        invalid.spec.proxyPassword = "wrong-secret";
        // An explicitly ignored native hint must not hide rejection of every
        // usable hint. The real dead alternate above still exercises CONNECT.
        invalid.spec.url += "&x.pe=127.0.0.3:1";
        if (cachedMetadata)
            invalid.spec.torrent.metadata = complete.metadata;
        const auto previous = proxy.snapshot();
        maskpilot::TransferControl invalidControl;
        const auto rejected =
            run(factory, invalid, fixture.temporary.path / "work" / invalid.id, invalidControl);
        EXPECT_FALSE(rejected.timedOut) << rejected.diagnostics();
        EXPECT_EQ(rejected.terminals, 1);
        EXPECT_EQ(rejected.terminal.outcome, maskpilot::TransferOutcome::Attention) << rejected.diagnostics();
        EXPECT_EQ(rejected.terminal.error.find("wrong-secret"), std::string::npos);
        EXPECT_EQ(rejected.terminal.error.find("fixture-secret"), std::string::npos);
        EXPECT_EQ(rejected.metadata, nullptr);
        EXPECT_FALSE(std::filesystem::exists(invalid.spec.destination));
        const auto after = proxy.snapshot();
        EXPECT_GT(after.value("auth_rejected").toInt(), previous.value("auth_rejected").toInt());
        EXPECT_EQ(after.value("forwarded_bytes").toDouble(), before.value("forwarded_bytes").toDouble());
    }
}

TEST(TorrentProxy, UnreachablePeerAndTrackerDoNotPoisonHealthyAlternates) {
    for (const auto* kind : {"http", "socks5"}) {
        SCOPED_TRACE(kind);
        TorrentFixture fixture;
        RecordingProxyFixture proxy(kind);
        auto params =
            lt::load_torrent_buffer(lt::span<char const>(fixture.metadata->data(), fixture.metadata->size()));
        params.trackers.insert(params.trackers.begin(), "http://127.0.0.1:1/announce");
        params.tracker_tiers.assign(params.trackers.size(), 1);
        params.tracker_tiers.front() = 0;
        params.peers = {{boost::asio::ip::make_address("127.0.0.1"), 1},
                        {boost::asio::ip::make_address("127.0.0.1"), fixture.seed->listen_port()}};
        auto task = fixture.task(fixture.temporary.path / "alternates", true);
        task.spec.url = lt::make_magnet_uri(params);
        task.spec.proxy = proxy.endpoint();
        maskpilot::TransferControl control;
        const auto result = run(localFactory(), task, fixture.temporary.path / "work" / task.id, control);
        expectComplete(result);
        fixture.expectPayload(task.spec.destination);
        bool failedDestinationAttempted = false, healthyPeerAttempted = false;
        for (const auto& entry : proxy.snapshot().value("destinations").toArray()) {
            failedDestinationAttempted |= entry.toObject().value("port").toInt() == 1;
            healthyPeerAttempted |= entry.toObject().value("port").toInt() == fixture.seed->listen_port();
        }
        EXPECT_TRUE(failedDestinationAttempted);
        EXPECT_TRUE(healthyPeerAttempted);
    }
}

TEST(TorrentProxy, OneUnavailableSourceDoesNotAbortAnotherJobUsingTheSameRoute) {
    TorrentFixture fixture;
    RecordingProxyFixture proxy("http");
    auto badEntry = fixture.metadataEntry;
    badEntry["info"]["name"] = "unavailable-bundle";
    badEntry["announce"] = "http://127.0.0.1:1/announce";
    badEntry.dict().erase("announce-list");
    auto bad = fixture.task(fixture.temporary.path / "unavailable");
    bad.spec.torrent.metadata = std::make_shared<const std::vector<char>>(encode(badEntry));
    bad.spec.proxy = proxy.endpoint();
    auto good = fixture.task(fixture.temporary.path / "healthy");
    good.spec.proxy = proxy.endpoint();
    const auto factory = localFactory();
    maskpilot::TransferControl badControl, goodControl;
    std::atomic_bool waiting = false;
    auto blocked = std::async(std::launch::async, [&] {
        return run(factory, bad, fixture.temporary.path / "work" / bad.id, badControl,
                   [&](const auto& event) {
                       if (event.phase == maskpilot::TaskState::Downloading)
                           waiting = true;
                   });
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!waiting && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(waiting);
    const auto result = run(factory, good, fixture.temporary.path / "work" / good.id, goodControl);
    badControl.cancel = true;
    const auto unavailable = blocked.get();
    expectComplete(result);
    fixture.expectPayload(good.spec.destination);
    EXPECT_FALSE(unavailable.timedOut);
    EXPECT_EQ(unavailable.terminal.outcome, maskpilot::TransferOutcome::Cancelled);
    EXPECT_TRUE(unavailable.terminal.error.empty());
    EXPECT_FALSE(std::filesystem::exists(bad.spec.destination));
}

TEST(TorrentEngine, LocalTorrentTransfersVerifiedFilesWithoutPublicDiscovery) {
    TorrentFixture fixture(lt::create_torrent::v1_only, "下载目录");
    auto task = fixture.task(fixture.temporary.path / nativePath("下载结果"));
    maskpilot::TransferControl control;
    const auto result = run(localFactory(), task, fixture.temporary.path / "work" / task.id, control);
    expectComplete(result);
    fixture.expectPayload(task.spec.destination);
}
TEST(TorrentEngine, RequiredOriginAuthenticationIsRejectedBeforeSessionOrSourceContact) {
    TorrentFixture fixture;
    for (const bool taskMarker : {false, true}) {
        auto task = fixture.task(fixture.temporary.path / (taskMarker ? "task-marker" : "spec-marker"));
        task.originAuthenticationRequired = taskMarker;
        task.spec.originAuthenticationRequired = !taskMarker;
        maskpilot::TransferControl control;
        const auto started = std::chrono::steady_clock::now();
        const auto result = run(localFactory(), task, fixture.temporary.path / "work" / task.id, control);
        EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
        EXPECT_FALSE(result.timedOut);
        EXPECT_EQ(result.terminals, 1);
        EXPECT_EQ(result.terminal.outcome, maskpilot::TransferOutcome::Attention);
        EXPECT_EQ(fixture.tracker->requests(), 0u);
        EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
    }
}

TEST(TorrentEngine, MagnetFetchesMetadataThenTransfersVerifiedFiles) {
    const std::array<lt::create_flags_t, 3> variants{lt::create_torrent::v1_only, lt::create_torrent::v2_only,
                                                     lt::create_flags_t{}};
    const std::array<const char*, 3> names{"v1 magnet", "v2-only magnet", "hybrid magnet"};
    for (std::size_t n = 0; n < variants.size(); ++n) {
        SCOPED_TRACE(names[n]);
        TorrentFixture fixture(variants[n]);
        auto task = fixture.task(fixture.temporary.path / "magnet-received", true);
        maskpilot::TransferControl control;
        bool fetching = false;
        auto result = run(
            localFactory(), task, fixture.temporary.path / "work" / task.id, control,
            [&](const auto& event) { fetching |= event.phase == maskpilot::TaskState::FetchingMetadata; });
        if (result.timedOut || result.terminal.outcome != maskpilot::TransferOutcome::Complete) {
            std::ostringstream producer;
            try {
                producer << "listen_port=" << fixture.seed->listen_port()
                         << " handle_valid=" << fixture.seedHandle.is_valid();
                if (fixture.seedHandle.is_valid()) {
                    const auto status = fixture.seedHandle.status();
                    producer << " state=" << static_cast<int>(status.state)
                             << " is_finished=" << status.is_finished << " is_seeding=" << status.is_seeding
                             << " wanted=" << status.total_wanted_done << '/' << status.total_wanted
                             << " peers=" << status.num_peers << " seeds=" << status.num_seeds
                             << " errc=" << status.errc.value()
                             << " error=" << status.errc.message().substr(0, 256);
                }
                std::vector<lt::alert*> alerts;
                fixture.seed->pop_alerts(&alerts);
                const auto first = alerts.size() > 8 ? alerts.size() - 8 : 0;
                for (auto i = first; i < alerts.size(); ++i)
                    producer << " alert=" << alerts[i]->message().substr(0, 200);
            } catch (const std::exception& error) {
                producer << " snapshot_error=" << std::string(error.what()).substr(0, 256);
            }
            result.producerDiagnostics = producer.str();
        }
        expectComplete(result);
        EXPECT_TRUE(fetching);
        EXPECT_NE(result.metadata, nullptr) << result.diagnostics();
        EXPECT_NE(result.description, nullptr) << result.diagnostics();
        if (!result.metadata || !result.description)
            continue; // Keep every format independently exercised even when an earlier format fails.
        EXPECT_FALSE(result.description->infoHash.empty());
        auto captured = task.spec;
        captured.torrent.metadata = result.metadata;
        const auto restored = maskpilot::describeTorrent(captured);
        EXPECT_EQ(restored.infoHash, result.description->infoHash);
        fixture.expectPayload(task.spec.destination);
    }
}

TEST(TorrentEngine, TrackerOnlyMagnetFetchesMetadataAndKeepsPayloadPeerDiscoveryWorking) {
    TorrentFixture fixture;
    auto params =
        lt::load_torrent_buffer(lt::span<char const>(fixture.metadata->data(), fixture.metadata->size()));
    ASSERT_FALSE(params.trackers.empty());
    ASSERT_TRUE(std::all_of(params.trackers.begin(), params.trackers.end(),
                            [](const auto& tracker) { return tracker.starts_with("http://127.0.0.1:"); }));
    params.peers.clear();
    params.dht_nodes.clear();
    params.url_seeds.clear();
    const auto magnet = lt::make_magnet_uri(params);
    ASSERT_NE(magnet.find("tr="), std::string::npos);
    ASSERT_EQ(magnet.find("x.pe="), std::string::npos);
    auto task = fixture.task(fixture.temporary.path / "tracker-magnet", true);
    task.spec.url = magnet;
    ASSERT_EQ(task.spec.torrent.metadata, nullptr);
    maskpilot::TransferControl control;
    bool fetching = false;
    const auto result =
        run(localFactory(), task, fixture.temporary.path / "work" / task.id, control,
            [&](const auto& event) { fetching |= event.phase == maskpilot::TaskState::FetchingMetadata; });
    expectComplete(result);
    EXPECT_TRUE(fetching);
    EXPECT_NE(result.metadata, nullptr) << result.diagnostics();
    EXPECT_NE(result.description, nullptr) << result.diagnostics();
    fixture.expectPayload(task.spec.destination);
}

TEST(TorrentEngine, V2AndHybridMetadataPreserveVerifiedPayloads) {
    const std::array<lt::create_flags_t, 2> variants{lt::create_torrent::v2_only, lt::create_flags_t{}};
    for (std::size_t n = 0; n < variants.size(); ++n) {
        SCOPED_TRACE(n == 0 ? "v2 torrent" : "hybrid torrent");
        TorrentFixture fixture(variants[n]);
        auto task = fixture.task(fixture.temporary.path / "received");
        maskpilot::TransferControl control;
        const auto result = run(localFactory(), task, fixture.temporary.path / "work" / task.id, control);
        expectComplete(result);
        fixture.expectPayload(task.spec.destination);
    }
}

TEST(TorrentEngine, MetadataOnlySelectionHoldsPayloadThenDownloadsOnlyWantedFile) {
    TorrentFixture fixture;
    auto task = fixture.task(fixture.temporary.path / "selected", true);
    task.spec.torrent.allFiles = false;
    maskpilot::TransferControl control;
    const auto factory = localFactory();
    const auto work = fixture.temporary.path / "work" / task.id;
    const auto held = run(factory, task, work, control);
    EXPECT_FALSE(held.timedOut);
    EXPECT_EQ(held.terminals, 1);
    EXPECT_EQ(held.terminal.outcome, maskpilot::TransferOutcome::SelectionRequired) << held.terminal.error;
    EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
    ASSERT_NE(held.metadata, nullptr);
    ASSERT_NE(held.description, nullptr);
    ASSERT_NE(held.description->files, nullptr);
    const auto& files = *held.description->files;
    const auto chosen = std::find_if(files.begin(), files.end(),
                                     [](const auto& file) { return file.path.ends_with("nested/beta.bin"); });
    ASSERT_NE(chosen, files.end());
    task.spec.torrent.metadata = held.metadata;
    task.spec.torrent.selectedFiles = {chosen->index};
    const auto received = run(factory, task, work, control);
    expectComplete(received);
    EXPECT_EQ(contents(task.spec.destination / nativePath(fixture.payload[1].first)),
              fixture.payload[1].second);
    EXPECT_FALSE(std::filesystem::exists(task.spec.destination / nativePath(fixture.payload[0].first)));
    EXPECT_FALSE(std::filesystem::exists(task.spec.destination / nativePath(fixture.payload[2].first)));
}

TEST(TorrentEngine, EmptyFileSelectionCompletesButInvalidIndicesNeverStartPayload) {
    TorrentFixture fixture;
    auto task = fixture.task(fixture.temporary.path / "empty-only");
    task.spec.torrent.allFiles = false;
    const auto description = maskpilot::describeTorrent(task.spec);
    ASSERT_NE(description.files, nullptr);
    const auto empty =
        std::find_if(description.files->begin(), description.files->end(),
                     [](const auto& file) { return !file.padding && file.path.ends_with("empty.bin"); });
    ASSERT_NE(empty, description.files->end());
    task.spec.torrent.selectedFiles = {empty->index};
    maskpilot::TransferControl control;
    const auto factory = localFactory();
    const auto received = run(factory, task, fixture.temporary.path / "work" / task.id, control);
    expectComplete(received);
    ASSERT_TRUE(std::filesystem::is_regular_file(task.spec.destination / nativePath(empty->path)));
    EXPECT_EQ(std::filesystem::file_size(task.spec.destination / nativePath(empty->path)), 0);
    EXPECT_FALSE(std::filesystem::exists(task.spec.destination / nativePath(fixture.payload[0].first)));
    EXPECT_FALSE(std::filesystem::exists(task.spec.destination / nativePath(fixture.payload[1].first)));
    for (const int index : {-1, 19999}) {
        auto invalid = fixture.task(fixture.temporary.path / ("invalid-" + std::to_string(index)));
        invalid.spec.torrent.allFiles = false;
        invalid.spec.torrent.selectedFiles = {index};
        const auto rejected = run(factory, invalid, fixture.temporary.path / "work" / invalid.id, control);
        EXPECT_FALSE(rejected.timedOut);
        EXPECT_EQ(rejected.terminals, 1);
        EXPECT_EQ(rejected.terminal.outcome, maskpilot::TransferOutcome::Attention)
            << rejected.terminal.error;
        EXPECT_FALSE(std::filesystem::exists(invalid.spec.destination));
    }
}

TEST(TorrentEngine, RecreatedRuntimeRechecksCorruptedRecoveryDataBeforePublishing) {
    TorrentFixture fixture;
    auto task = fixture.task(fixture.temporary.path / "resumed");
    const auto work = fixture.temporary.path / "work" / task.id;
    maskpilot::TransferControl control;
    control.speedLimit = 256 * 1024;
    {
        const auto factory = localFactory();
        const auto paused = run(factory, task, work, control, [&](const auto& event) {
            if (event.outcome == maskpilot::TransferOutcome::Progress && event.downloaded > 256 * 1024)
                control.pause = true;
        });
        ASSERT_FALSE(paused.timedOut);
        ASSERT_EQ(paused.terminal.outcome, maskpilot::TransferOutcome::Paused) << paused.terminal.error;
        ASSERT_FALSE(std::filesystem::exists(task.spec.destination));
    }
    bool corrupted = false;
    const auto stage = task.spec.destination.parent_path() / (".maskpilot-torrent-" + task.id);
    for (const auto& [path, expected] : fixture.payload) {
        if (expected.empty() || !std::filesystem::is_regular_file(stage / nativePath(path)))
            continue;
        const auto data = contents(stage / nativePath(path));
        for (std::size_t offset = 0; offset + 16384 <= std::min(data.size(), expected.size());
             offset += 16384) {
            if (!std::equal(data.begin() + offset, data.begin() + offset + 16384, expected.begin() + offset))
                continue;
            const auto modified = std::filesystem::last_write_time(stage / nativePath(path));
            std::fstream output(stage / nativePath(path), std::ios::binary | std::ios::in | std::ios::out);
            output.seekp(static_cast<std::streamoff>(offset));
            output.put(static_cast<char>(static_cast<unsigned char>(expected[offset]) ^ 0xff));
            output.close();
            std::filesystem::last_write_time(stage / nativePath(path), modified);
            corrupted = true;
            break;
        }
        if (corrupted)
            break;
    }
    ASSERT_TRUE(corrupted) << "Fixture must damage a fully downloaded piece while preserving its timestamp.";
    control.pause = false;
    control.speedLimit = 0;
    const auto resumed = run(localFactory(), task, work, control);
    expectComplete(resumed);
    fixture.expectPayload(task.spec.destination);
}

TEST(TorrentEngine, FinalizingCancellationAndExistingDestinationNeverPublishOverUserFiles) {
    TorrentFixture fixture;
    auto factory = localFactory();
    auto task = fixture.task(fixture.temporary.path / "cancelled");
    maskpilot::TransferControl control;
    bool finalizing = false;
    const auto cancelled =
        run(factory, task, fixture.temporary.path / "work" / task.id, control, [&](const auto& event) {
            if (event.outcome == maskpilot::TransferOutcome::Progress &&
                event.phase == maskpilot::TaskState::Finalizing) {
                finalizing = true;
                control.cancel = true;
            }
        });
    EXPECT_TRUE(finalizing);
    EXPECT_EQ(cancelled.terminals, 1);
    EXPECT_EQ(cancelled.terminal.outcome, maskpilot::TransferOutcome::Cancelled) << cancelled.terminal.error;
    EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
    factory()->discard(task, fixture.temporary.path / "work" / task.id);
    task = fixture.task(fixture.temporary.path / "occupied");
    control.cancel = false;
    const auto work = fixture.temporary.path / "work" / task.id;
    const auto paused = run(factory, task, work, control, [&](const auto& event) {
        if (event.outcome == maskpilot::TransferOutcome::Progress && event.downloaded > 0)
            control.pause = true;
    });
    ASSERT_EQ(paused.terminal.outcome, maskpilot::TransferOutcome::Paused) << paused.terminal.error;
    const auto stage = task.spec.destination.parent_path() / (".maskpilot-torrent-" + task.id);
    ASSERT_TRUE(std::filesystem::is_directory(stage));
    const std::vector<char> sentinel{'k', 'e', 'e', 'p'};
    writeBytes(task.spec.destination / "user-file.bin", sentinel);
    control.pause = false;
    const auto conflict = run(factory, task, work, control);
    EXPECT_FALSE(conflict.timedOut);
    EXPECT_NE(conflict.terminal.outcome, maskpilot::TransferOutcome::Complete);
    EXPECT_EQ(contents(task.spec.destination / "user-file.bin"), sentinel);
    EXPECT_FALSE(std::filesystem::exists(task.spec.destination / nativePath(fixture.payload[0].first)));
    factory()->discard(task, work);
    EXPECT_FALSE(std::filesystem::exists(stage));
    EXPECT_FALSE(std::filesystem::exists(work / "torrent"));
    EXPECT_EQ(contents(task.spec.destination / "user-file.bin"), sentinel);
}

TEST(TorrentEngine, SharedFactoryRoutesConcurrentDifferentTorrentsToTheirOwnFiles) {
    TorrentFixture first(lt::create_torrent::v1_only, "first", 31);
    TorrentFixture second(lt::create_torrent::v1_only, "second", 73);
    const auto factory = localFactory();
    auto one = first.task(first.temporary.path / "received");
    auto two = second.task(second.temporary.path / "received");
    maskpilot::TransferControl firstControl, secondControl;
    auto firstRun = std::async(std::launch::async, [&] {
        return run(factory, one, first.temporary.path / "work" / one.id, firstControl);
    });
    auto secondRun = std::async(std::launch::async, [&] {
        return run(factory, two, second.temporary.path / "work" / two.id, secondControl);
    });
    expectComplete(firstRun.get());
    expectComplete(secondRun.get());
    first.expectPayload(one.spec.destination);
    second.expectPayload(two.spec.destination);
}

TEST(TorrentEngine, ConcurrentDuplicateIdentityCannotShareAnotherTasksStorage) {
    TorrentFixture fixture;
    const auto factory = localFactory();
    auto first = fixture.task(fixture.temporary.path / "first-output");
    auto second = fixture.task(fixture.temporary.path / "second-output");
    maskpilot::TransferControl firstControl, secondControl;
    firstControl.speedLimit = 32 * 1024;
    std::promise<void> started;
    auto ready = started.get_future();
    std::atomic_bool signaled = false;
    auto firstRun = std::async(std::launch::async, [&] {
        return run(factory, first, fixture.temporary.path / "work" / first.id, firstControl,
                   [&](const auto& event) {
                       if (event.outcome == maskpilot::TransferOutcome::Progress && event.downloaded > 0 &&
                           !signaled.exchange(true))
                           started.set_value();
                   });
    });
    const bool reached = ready.wait_for(20s) == std::future_status::ready;
    if (!reached) {
        firstControl.cancel = true;
        const auto stopped = firstRun.get();
        FAIL() << "First torrent did not start: " << stopped.terminal.error;
    }
    firstControl.speedLimit = 1;
    const auto duplicate =
        run(factory, second, fixture.temporary.path / "work" / second.id, secondControl, {}, 10s);
    EXPECT_FALSE(duplicate.timedOut);
    EXPECT_EQ(duplicate.terminals, 1);
    EXPECT_TRUE(duplicate.terminal.outcome == maskpilot::TransferOutcome::Attention ||
                duplicate.terminal.outcome == maskpilot::TransferOutcome::Failed)
        << duplicate.terminal.error;
    EXPECT_FALSE(duplicate.terminal.error.empty());
    EXPECT_FALSE(std::filesystem::exists(second.spec.destination));
    firstControl.pause = true;
    const auto paused = firstRun.get();
    EXPECT_EQ(paused.terminal.outcome, maskpilot::TransferOutcome::Paused) << paused.terminal.error;
    EXPECT_FALSE(std::filesystem::exists(first.spec.destination));
}

TEST(TorrentEngine, PublishedSeedingUploadsWithLimitAndPausesWithoutRemovingFiles) {
    TorrentFixture fixture;
    const auto port = availableTcpAndUdpPort();
    maskpilot::TorrentRuntimeOptions options;
    options.enableDiscovery = false;
    options.listenInterfaces = "127.0.0.1:" + std::to_string(port);
    const auto factory = maskpilot::makeTorrentTransferFactory(options);
    auto task = fixture.task(fixture.temporary.path / "published-seed");
    task.spec.torrent.seedAfterDownload = true;
    task.spec.torrent.uploadLimit = 64 * 1024;
    maskpilot::TransferControl control;
    control.uploadLimit = task.spec.torrent.uploadLimit;
    std::unique_ptr<lt::session> receiver;
    lt::torrent_handle receiverHandle;
    const auto received = fixture.temporary.path / "receiving-peer";
    bool seeding = false, receiverFinished = false;
    std::int64_t uploaded = 0;
    auto began = std::chrono::steady_clock::now();
    const auto result = run(
        factory, task, fixture.temporary.path / "work" / task.id, control,
        [&](const auto& event) {
            if (event.outcome != maskpilot::TransferOutcome::Progress ||
                event.phase != maskpilot::TaskState::Seeding)
                return;
            seeding = true;
            uploaded = std::max(uploaded, event.uploaded);
            if (!receiver) {
                // Model a distinct peer IP so the product's normal per-IP connection policy cannot
                // confuse this receiver with the original seed still advertised by the local tracker.
                auto settings = lt::session_params(isolatedSettings({}, "127.0.0.2"));
                // The receiving peer already has metadata. Disable PEX so only our product seed can supply
                // it.
                settings.extensions.clear();
                receiver = std::make_unique<lt::session>(std::move(settings));
                auto params = lt::load_torrent_buffer(
                    lt::span<char const>(fixture.metadata->data(), fixture.metadata->size()));
                params.trackers.clear();
                params.tracker_tiers.clear();
                params.url_seeds.clear();
                params.peers = {{boost::asio::ip::make_address("127.0.0.1"), port}};
                params.save_path = pathText(received);
                params.flags &= ~lt::torrent_flags::auto_managed;
                params.flags &= ~lt::torrent_flags::paused;
                lt::error_code error;
                receiverHandle = receiver->add_torrent(params, error);
                if (error)
                    throw std::runtime_error("Cannot start receiving fixture peer: " + error.message());
                began = std::chrono::steady_clock::now();
            }
            if (receiverHandle.status().is_finished && uploaded > 0) {
                receiverFinished = true;
                control.pause = true;
            }
        },
        60s);
    std::ostringstream diagnostics;
    diagnostics << result.diagnostics() << " product_port=" << port << " product_uploaded=" << uploaded;
    if (result.timedOut || !receiverFinished) {
        try {
            diagnostics << " receiver_exists=" << static_cast<bool>(receiver);
            if (receiver)
                diagnostics << " receiver_listen_port=" << receiver->listen_port();
            diagnostics << " receiver_handle_valid=" << receiverHandle.is_valid();
            if (receiverHandle.is_valid()) {
                const auto status = receiverHandle.status();
                diagnostics << " receiver_state=" << static_cast<int>(status.state)
                            << " receiver_wanted=" << status.total_wanted_done << '/' << status.total_wanted
                            << " receiver_peers=" << status.num_peers
                            << " receiver_seeds=" << status.num_seeds
                            << " receiver_errc=" << status.errc.value()
                            << " receiver_error=" << status.errc.message().substr(0, 256);
            }
            if (receiver) {
                std::vector<lt::alert*> alerts;
                receiver->pop_alerts(&alerts);
                const auto first = alerts.size() > 16 ? alerts.size() - 16 : 0;
                for (auto i = first; i < alerts.size(); ++i)
                    diagnostics << " receiver_alert=" << alerts[i]->message().substr(0, 250);
            }
        } catch (const std::exception& error) {
            diagnostics << " snapshot_error=" << std::string(error.what()).substr(0, 256);
        }
    }
    SCOPED_TRACE(diagnostics.str());
    EXPECT_FALSE(result.timedOut);
    EXPECT_TRUE(seeding);
    EXPECT_TRUE(receiverFinished);
    EXPECT_GT(uploaded, 0);
    EXPECT_EQ(result.terminals, 1);
    EXPECT_EQ(result.terminal.outcome, maskpilot::TransferOutcome::Paused) << result.terminal.error;
    if (receiverFinished) {
        EXPECT_GE(std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count(), 3.0);
        fixture.expectPayload(received);
    }
    fixture.expectPayload(task.spec.destination);
    factory()->discard(task, fixture.temporary.path / "work" / task.id);
    fixture.expectPayload(task.spec.destination);
}

TEST(TorrentEngine, RejectsMalformedMetadataAndUnsafeOrCollidingFileNamesBeforeNetwork) {
    TorrentFixture fixture;
    EXPECT_THROW(maskpilot::describeTorrent(specWithMetadata({'x'})), std::exception);
    EXPECT_THROW(maskpilot::describeTorrent(
                     specWithMetadata(std::vector<char>(maskpilot::maxTorrentMetadataBytes + 1, 'x'))),
                 std::exception);
    auto traversal = fixture.metadataEntry;
    traversal["info"]["files"].list()[0]["path"] =
        lt::entry::list_type{lt::entry(".."), lt::entry("escape.bin")};
    EXPECT_THROW(maskpilot::describeTorrent(specWithMetadata(encode(traversal))), std::exception);
    auto collision = fixture.metadataEntry;
    collision["info"]["files"].list()[0]["path"] = lt::entry::list_type{lt::entry("Same.bin")};
    collision["info"]["files"].list()[1]["path"] = lt::entry::list_type{lt::entry("same.bin")};
    EXPECT_THROW(maskpilot::describeTorrent(specWithMetadata(encode(collision))), std::exception);
    auto reserved = fixture.metadataEntry;
    reserved["info"]["files"].list()[0]["path"] = lt::entry::list_type{lt::entry(".maskpilot-owner")};
    EXPECT_THROW(maskpilot::describeTorrent(specWithMetadata(encode(reserved))), std::exception);
    auto prefixCollision = fixture.metadataEntry;
    prefixCollision["info"]["files"].list()[0]["path"] = lt::entry::list_type{lt::entry("nested")};
    prefixCollision["info"]["files"].list()[1]["path"] =
        lt::entry::list_type{lt::entry("nested"), lt::entry("child.bin")};
    EXPECT_THROW(maskpilot::describeTorrent(specWithMetadata(encode(prefixCollision))), std::exception);
    auto symlink = fixture.metadataEntry;
    bool changedEmptyFile = false;
    for (auto& file : symlink["info"]["files"].list()) {
        if (file["length"].integer() != 0)
            continue;
        file["attr"] = "l";
        file["symlink path"] = lt::entry::list_type{lt::entry("alpha.bin")};
        changedEmptyFile = true;
        break;
    }
    ASSERT_TRUE(changedEmptyFile);
    EXPECT_THROW(maskpilot::describeTorrent(specWithMetadata(encode(symlink))), std::exception);
    auto mismatched = fixture.metadataEntry;
    mismatched["info"]["name"] = "different-identity";
    auto boundToMagnet = specWithMetadata(encode(mismatched));
    boundToMagnet.url = fixture.magnet;
    EXPECT_THROW(maskpilot::describeTorrent(boundToMagnet), std::exception);
}

TEST(TorrentEngine, FinishRequestEndsSeedingAsCompletedAndPreservesPublishedPayload) {
    TorrentFixture fixture;
    auto task = fixture.task(fixture.temporary.path / "finished-seed");
    task.spec.torrent.seedAfterDownload = true;
    maskpilot::TransferControl control;
    bool seeding = false;
    const auto factory = localFactory();
    const auto result =
        run(factory, task, fixture.temporary.path / "work" / task.id, control, [&](const auto& event) {
            if (event.outcome == maskpilot::TransferOutcome::Progress &&
                event.phase == maskpilot::TaskState::Seeding) {
                seeding = true;
                control.finish = true;
            }
        });
    expectComplete(result);
    EXPECT_TRUE(seeding);
    fixture.expectPayload(task.spec.destination);
    factory()->discard(task, fixture.temporary.path / "work" / task.id);
    fixture.expectPayload(task.spec.destination);
}

TEST(TorrentEngine, DoesNotSilentlyBypassAnExplicitProxy) {
    TorrentFixture fixture;
    auto task = fixture.task(fixture.temporary.path / "proxy");
    task.spec.proxy = "http://127.0.0.1:1";
    maskpilot::TransferControl control;
    const auto result = run(localFactory(), task, fixture.temporary.path / "work" / task.id, control);
    EXPECT_FALSE(result.timedOut);
    EXPECT_EQ(result.terminals, 1);
    EXPECT_NE(result.terminal.outcome, maskpilot::TransferOutcome::Complete);
    EXPECT_FALSE(result.terminal.error.empty());
    EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
}

TEST(TorrentEngine, InvalidListenerFailsClearlyAndStartupRemainsPausableAndCancellable) {
    TorrentFixture fixture;
    const std::array outcomes{maskpilot::TransferOutcome::Attention, maskpilot::TransferOutcome::Paused,
                              maskpilot::TransferOutcome::Cancelled};
    for (std::size_t n = 0; n < outcomes.size(); ++n) {
        SCOPED_TRACE(n);
        maskpilot::TorrentRuntimeOptions options;
        options.enableDiscovery = false;
        options.listenInterfaces = "127.0.0.1:not-a-port";
        const auto factory = maskpilot::makeTorrentTransferFactory(options);
        auto task = fixture.task(fixture.temporary.path / ("invalid-listener-" + std::to_string(n)));
        const auto stage = task.spec.destination.parent_path() / (".maskpilot-torrent-" + task.id);
        const std::vector<char> preserved{'k', 'e', 'e', 'p'};
        maskpilot::TransferControl control;
        std::jthread interrupt;
        bool probing = false;
        const auto started = std::chrono::steady_clock::now();
        const auto result = run(
            factory, task, fixture.temporary.path / "work" / task.id, control,
            [&](const auto& event) {
                if (probing || event.outcome != maskpilot::TransferOutcome::Progress ||
                    event.phase != maskpilot::TaskState::Probing)
                    return;
                probing = true;
                writeBytes(stage / "preserved.bin", preserved);
                if (outcomes[n] != maskpilot::TransferOutcome::Attention) {
                    interrupt = std::jthread([&](std::stop_token stop) {
                        const auto deadline = std::chrono::steady_clock::now() + 200ms;
                        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
                            std::this_thread::sleep_for(5ms);
                        if (!stop.stop_requested()) {
                            if (outcomes[n] == maskpilot::TransferOutcome::Paused)
                                control.pause = true;
                            else
                                control.cancel = true;
                        }
                    });
                }
            },
            8s);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        interrupt.request_stop();
        if (interrupt.joinable())
            interrupt.join();
        EXPECT_TRUE(probing);
        EXPECT_FALSE(result.timedOut) << result.diagnostics();
        EXPECT_EQ(result.terminals, 1) << result.diagnostics();
        EXPECT_EQ(result.terminal.outcome, outcomes[n]) << result.diagnostics();
        if (outcomes[n] == maskpilot::TransferOutcome::Attention) {
            EXPECT_NE(result.terminal.error.find("network listener"), std::string::npos)
                << result.diagnostics();
            EXPECT_LT(elapsed, 7s);
        } else {
            EXPECT_TRUE(result.terminal.error.empty()) << result.diagnostics();
            EXPECT_LT(elapsed, 2s);
        }
        EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
        EXPECT_EQ(contents(stage / "preserved.bin"), preserved);
        for (const auto& [path, bytes] : fixture.payload)
            EXPECT_FALSE(std::filesystem::exists(stage / nativePath(path)));
    }
}

TEST(TorrentEngine, TorrentMetadataIsDurableBeforeAckAndSurvivesOriginalFileRemoval) {
    TorrentFixture fixture;
    const auto source = fixture.temporary.path / "source.torrent";
    writeBytes(source, *fixture.metadata);
    const auto data = fixture.temporary.path / "service";
    std::string id;
    {
        maskpilot::DownloadService service(maskpilot::makeSqliteRepository(data / "tasks.db"), localFactory(),
                                           data / "work");
        auto task = fixture.task(fixture.temporary.path / "service-output");
        task.spec.torrent.metadata.reset();
        task.spec.torrent.allFiles = false;
        task.spec.startPaused = true;
        id = service.add(task.spec).get();
        const auto tasks = service.snapshot();
        ASSERT_EQ(tasks.size(), 1);
        ASSERT_NE(tasks[0].spec.torrent.metadata, nullptr);
        EXPECT_EQ(*tasks[0].spec.torrent.metadata, *fixture.metadata);
        ASSERT_NE(tasks[0].torrent.files, nullptr);
        EXPECT_FALSE(tasks[0].torrent.files->empty());
        ASSERT_TRUE(std::filesystem::remove(source));
    }
    maskpilot::DownloadService reopened(maskpilot::makeSqliteRepository(data / "tasks.db"), localFactory(),
                                        data / "work");
    const auto restored = reopened.snapshot();
    ASSERT_EQ(restored.size(), 1);
    EXPECT_EQ(restored[0].id, id);
    ASSERT_NE(restored[0].spec.torrent.metadata, nullptr);
    EXPECT_EQ(*restored[0].spec.torrent.metadata, *fixture.metadata);
    ASSERT_NE(restored[0].torrent.files, nullptr);
    EXPECT_FALSE(restored[0].torrent.files->empty());
    EXPECT_FALSE(std::filesystem::exists(source));
}

TEST(TorrentEngine, UnifiedRouterCapturesDurablyAndDispatchesQueuedTorrentToVerifiedCompletion) {
    TorrentFixture fixture;
    const auto source = fixture.temporary.path / "source.torrent";
    writeBytes(source, *fixture.metadata);
    const auto data = fixture.temporary.path / "unified-service";
    const auto database = data / "tasks.db";
    maskpilot::DownloadService service(maskpilot::makeSqliteRepository(database),
                                       maskpilot::makeUnifiedTransferFactory(localFactory()), data / "work");
    auto task = fixture.task(fixture.temporary.path / "unified-output");
    task.spec.torrent.metadata.reset();
    task.spec.startPaused = true;
    const auto id = service.add(task.spec).get();
    const auto acknowledged = service.snapshot();
    ASSERT_EQ(acknowledged.size(), 1);
    EXPECT_EQ(acknowledged[0].id, id);
    EXPECT_EQ(acknowledged[0].state, maskpilot::TaskState::Paused);
    ASSERT_NE(acknowledged[0].torrent.files, nullptr);
    EXPECT_FALSE(acknowledged[0].torrent.files->empty());
    EXPECT_FALSE(acknowledged[0].torrent.infoHash.empty());
    ASSERT_NE(acknowledged[0].spec.torrent.metadata, nullptr);
    EXPECT_EQ(*acknowledged[0].spec.torrent.metadata, *fixture.metadata);
    auto independentReader = maskpilot::makeSqliteRepository(database);
    const auto durable = independentReader->load();
    ASSERT_EQ(durable.size(), 1);
    EXPECT_EQ(durable[0].id, id);
    EXPECT_EQ(durable[0].spec.kind, maskpilot::TransferKind::BitTorrent);
    ASSERT_NE(durable[0].spec.torrent.metadata, nullptr);
    EXPECT_EQ(*durable[0].spec.torrent.metadata, *fixture.metadata);
    EXPECT_FALSE(std::filesystem::exists(task.spec.destination));
    ASSERT_TRUE(std::filesystem::remove(source));
    service.resume(id);
    const auto finished = waitUntil(
        [&] {
            const auto tasks = service.snapshot();
            return !tasks.empty() && (tasks[0].state == maskpilot::TaskState::Completed ||
                                      tasks[0].state == maskpilot::TaskState::Failed ||
                                      tasks[0].state == maskpilot::TaskState::NeedsAttention);
        },
        30s);
    if (!finished)
        service.pause(id);
    ASSERT_TRUE(finished) << service.lastError();
    const auto completed = service.snapshot();
    ASSERT_EQ(completed.size(), 1);
    ASSERT_EQ(completed[0].state, maskpilot::TaskState::Completed) << completed[0].error;
    EXPECT_TRUE(completed[0].torrentPublished);
    fixture.expectPayload(completed[0].spec.destination);
    const auto saved = independentReader->load();
    ASSERT_EQ(saved.size(), 1);
    EXPECT_EQ(saved[0].state, maskpilot::TaskState::Completed);
    EXPECT_TRUE(saved[0].torrentPublished);
    service.remove(id);
    EXPECT_TRUE(waitUntil([&] { return service.snapshot().empty(); }, 5s));
    fixture.expectPayload(task.spec.destination);
}

#ifdef _WIN32
TEST(TorrentEngine, ActualCliCapturesAndDownloadsLocalTorrentWithoutPublicDiscovery) {
    TorrentFixture fixture;
    const auto source = fixture.temporary.path / "source.torrent";
    writeBytes(source, *fixture.metadata);
    std::filesystem::path cli;
    const auto configured = qEnvironmentVariable("MASKPILOT_TEST_CLI");
    if (!configured.isEmpty())
        cli = std::filesystem::path(configured.toStdWString());
    else {
        std::array<wchar_t, 32768> executable{};
        const auto length =
            GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        ASSERT_GT(length, 0u);
        ASSERT_LT(length, executable.size());
        const auto directory = std::filesystem::path(std::wstring(executable.data(), length)).parent_path();
        cli = directory / "maskpilot_cli.exe";
        if (!std::filesystem::is_regular_file(cli))
            cli = directory.parent_path() / "maskpilot_cli.exe";
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(cli)) << pathText(cli);
    const auto destination = fixture.temporary.path / "cli-output";
    const auto data = fixture.temporary.path / "cli-state";
    QProcess process;
    if (qEnvironmentVariable("MASKPILOT_TEST_CLEAN_CLI_ENV") == "1") {
        auto environment = QProcessEnvironment::systemEnvironment();
        const auto systemRoot = environment.value("SystemRoot");
        ASSERT_FALSE(systemRoot.isEmpty());
        environment.insert("PATH", systemRoot + "\\System32;" + systemRoot);
        for (const auto* name : {"QT_ROOT_DIR", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                                 "QML2_IMPORT_PATH", "QT_QPA_PLATFORM"})
            environment.remove(name);
        process.setProcessEnvironment(environment);
    }
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* arguments) { arguments->flags |= CREATE_NO_WINDOW; });
    struct Cleanup {
        QProcess& child;
        ~Cleanup() {
            if (child.state() != QProcess::NotRunning) {
                child.kill();
                child.waitForFinished(5000);
            }
        }
    } cleanup{process};
    process.start(QString::fromStdWString(cli.wstring()),
                  {"--bt-no-discovery", QString::fromStdWString(source.wstring()),
                   QString::fromStdWString(destination.wstring()), QString::fromStdWString(data.wstring())});
    ASSERT_TRUE(process.waitForStarted(10000)) << process.errorString().toStdString();
    if (!process.waitForFinished(45000)) {
        process.kill();
        process.waitForFinished(5000);
        FAIL() << "Owned CLI fixture timed out: " << process.readAllStandardError().toStdString();
    }
    ASSERT_EQ(process.exitStatus(), QProcess::NormalExit) << process.readAllStandardError().toStdString();
    ASSERT_EQ(process.exitCode(), 0) << process.readAllStandardError().toStdString();
    EXPECT_TRUE(process.readAllStandardOutput().contains("TASK_ID "));
    fixture.expectPayload(destination);
    const auto persisted = maskpilot::makeSqliteRepository(data / "tasks.db")->load();
    ASSERT_EQ(persisted.size(), 1);
    EXPECT_EQ(persisted[0].state, maskpilot::TaskState::Completed);
    EXPECT_EQ(persisted[0].spec.kind, maskpilot::TransferKind::BitTorrent);
    EXPECT_TRUE(persisted[0].torrentPublished);
    ASSERT_NE(persisted[0].spec.torrent.metadata, nullptr);
    EXPECT_EQ(*persisted[0].spec.torrent.metadata, *fixture.metadata);
}

TEST(TorrentEngine, CaptureMetadataRejectsUncAndDeviceSourcesBeforeOpeningThem) {
    TemporaryDirectory temporary;
    maskpilot::DownloadSpec spec;
    spec.kind = maskpilot::TransferKind::BitTorrent;
    spec.destination = temporary.path / "unused";
    const auto factory = localFactory();
    // No server is started. All UNC-shaped names refer only to loopback and must fail syntactically.
    const std::array<const char*, 6> sources{R"(torrent:\\127.0.0.1\maskpilot-fixture\source.torrent)",
                                             "torrent://127.0.0.1/maskpilot-fixture/source.torrent",
                                             R"(torrent:\\?\UNC\127.0.0.1\maskpilot-fixture\source.torrent)",
                                             R"(torrent:\\.\pipe\maskpilot-fixture)",
                                             R"(torrent:\\?\C:\maskpilot-fixture.torrent)",
                                             R"(torrent:C:\maskpilot-fixture.torrent:alternate-stream)"};
    const auto began = std::chrono::steady_clock::now();
    for (const auto* source : sources) {
        SCOPED_TRACE(source);
        spec.url = source;
        EXPECT_THROW(factory()->captureMetadata(spec), std::invalid_argument);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - began, 2s);
    EXPECT_TRUE(std::filesystem::is_empty(temporary.path));
}
#endif
