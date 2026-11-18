#include "../filesystem/transfer_files.h"
#include "torrent_internal.h"
#include <algorithm>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/socks5_stream.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <limits>
#include <map>
#include <maskpilot/domain/download_context.h>
#include <maskpilot/domain/proxy.h>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#ifdef _WIN32
#include <windows.h>
#endif

namespace maskpilot {
namespace {
namespace lt = libtorrent;
namespace ti = torrent_internal;
namespace files = transfer_files;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t maxResumeBytes = 32 * 1024 * 1024;
bool stopped(const TransferControl& control) { return control.pause || control.cancel; }
struct ProxyCheckStopped {};
void checkProxyEndpoint(const DownloadSpec& spec, const TransferControl& control) {
    if (stopped(control))
        throw ProxyCheckStopped{};
    const auto proxy = parseProxyEndpoint(spec.proxy);
    if (proxy.type == ProxyType::Direct)
        return;
    // Only literal IP reachability is preflighted. Hostname proxies use the native
    // resolver, whose OS lookup latency is outside this five-second connect bound.
    // Destination DNS remains proxy-side; no direct origin connection is made.
    boost::system::error_code addressError;
    const auto address = boost::asio::ip::make_address(proxy.host, addressError);
    if (addressError)
        return;
    boost::asio::io_context context;
    boost::asio::ip::tcp::socket socket(context);
    bool complete = false;
    boost::system::error_code result;
    socket.async_connect({address, static_cast<unsigned short>(proxy.port)}, [&](const auto& error) {
        result = error;
        complete = true;
    });
    const auto deadline = Clock::now() + 5s;
    while (!complete && !stopped(control) && Clock::now() < deadline)
        context.run_for(10ms);
    boost::system::error_code ignored;
    socket.close(ignored);
    context.stop();
    if (stopped(control))
        throw ProxyCheckStopped{};
    if (!complete || result)
        throw std::runtime_error(
            "Cannot reach the configured proxy endpoint. Check its address and availability.");
}
bool validId(const std::string& id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
void noReparse(const std::filesystem::path& path) {
    auto cursor = std::filesystem::absolute(path).lexically_normal();
    while (!cursor.empty()) {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(cursor.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Torrent storage cannot contain junctions or symbolic links.");
        if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_FILE_NOT_FOUND &&
            GetLastError() != ERROR_PATH_NOT_FOUND)
            throw std::runtime_error("Cannot inspect torrent storage ownership.");
#else
        std::error_code error;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(cursor, error)))
            throw std::runtime_error("Torrent storage cannot contain symbolic links.");
#endif
        const auto parent = cursor.parent_path();
        if (parent == cursor)
            break;
        cursor = parent;
    }
}
std::vector<char> readBounded(const std::filesystem::path& path, std::size_t maximum) {
    noReparse(path);
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > maximum)
        return {};
    std::vector<char> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) || input.peek() != EOF)
        return {};
    return bytes;
}
std::string ownership(const Task& task, const std::string& identity) {
    if (!validId(task.id) || identity.empty() || identity.size() > 200)
        throw std::invalid_argument("Invalid torrent ownership identity.");
    return task.id + "\n" + identity + "\n";
}
bool isOwned(const std::filesystem::path& root, const std::string& marker) {
    if (!std::filesystem::exists(root))
        return false;
    noReparse(root);
    if (!std::filesystem::is_directory(root))
        return false;
    const auto bytes = readBounded(root / ".maskpilot-owner", 512);
    return std::string(bytes.begin(), bytes.end()) == marker;
}
void establishOwner(const std::filesystem::path& root, const std::string& marker) {
    noReparse(root);
    if (std::filesystem::exists(root)) {
        if (!isOwned(root, marker))
            throw std::runtime_error(
                "Torrent cache ownership could not be validated. Existing files were preserved.");
        return;
    }
    std::filesystem::create_directories(root);
    files::atomicText(root / ".maskpilot-owner", marker);
#ifdef _WIN32
    SetFileAttributesW((root / ".maskpilot-owner").c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif
}
struct Storage {
    std::filesystem::path stage, final, cache;
    std::string marker, parts;
    bool published = false;
};
void inspectTree(const std::filesystem::path& root);
Storage storageFor(const Task& task, const std::filesystem::path& work, const std::string& identity,
                   bool create, bool inspectDestination = true) {
    if (!validId(task.id) || task.spec.destination.empty())
        throw std::invalid_argument("Invalid torrent task storage.");
    Storage storage;
    storage.final = std::filesystem::absolute(task.spec.destination).lexically_normal();
    storage.stage = storage.final.parent_path() / (".maskpilot-torrent-" + task.id);
    storage.cache = std::filesystem::absolute(work).lexically_normal() / "torrent";
    storage.marker = ownership(task, identity);
    storage.parts = ".maskpilot-parts-" + task.id;
    if (storage.final == storage.final.root_path() ||
        storage.stage.parent_path() != storage.final.parent_path() ||
        storage.stage.filename() != ti::nativePath(".maskpilot-torrent-" + task.id))
        throw std::invalid_argument("Torrent staging path escaped its owning directory.");
    noReparse(inspectDestination ? storage.final : storage.final.parent_path());
    noReparse(storage.stage);
    noReparse(storage.cache);
    if (inspectDestination && std::filesystem::exists(storage.final)) {
        if (!isOwned(storage.final, storage.marker))
            throw std::runtime_error("Torrent destination already exists. Existing files were preserved.");
        storage.published = true;
        if (std::filesystem::exists(storage.stage))
            throw std::runtime_error(
                "Two torrent payload roots exist. Review the preserved files before continuing.");
    }
    if (inspectDestination && task.torrentPublished && !storage.published)
        throw std::runtime_error("Published torrent directory is missing. It was not downloaded again.");
    if (create) {
        establishOwner(storage.cache, storage.marker);
        if (!storage.published)
            establishOwner(storage.stage, storage.marker);
        inspectTree(storage.published ? storage.final : storage.stage);
    }
    return storage;
}
void inspectTree(const std::filesystem::path& root) {
    noReparse(root);
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        noReparse(entry.path());
        if (++entries > maxTorrentFiles * 65 + 512)
            throw std::runtime_error("Torrent staging directory contains too many entries.");
    }
}
void publish(Storage& storage) {
    if (!isOwned(storage.stage, storage.marker))
        throw std::runtime_error("Torrent stage ownership changed. Publication was stopped.");
    inspectTree(storage.stage);
    noReparse(storage.final);
#ifdef _WIN32
    if (!MoveFileExW(storage.stage.c_str(), storage.final.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error(
            "Cannot publish torrent directory. Existing destination and staged files were preserved.");
#else
    // This product currently targets Windows; require an atomic no-replace primitive before other ports.
    throw std::runtime_error("Atomic torrent publication is not supported on this platform.");
#endif
    storage.published = true;
}
std::int64_t wantedBytes(const TransferDescription& description, const TorrentOptions& options) {
    std::int64_t bytes = 0;
    if (description.files)
        for (const auto& file : *description.files)
            if (torrentFileWanted(options, file))
                bytes += file.size;
    return bytes;
}
bool hasSelection(const TransferDescription& description, const TorrentOptions& options) {
    return description.files &&
           std::any_of(description.files->begin(), description.files->end(),
                       [&](const auto& file) { return torrentFileWanted(options, file); });
}
void verifyPublishedFiles(const Storage& storage, const TransferDescription& description,
                          const TorrentOptions& options, bool prune) {
    const auto root = storage.published ? storage.final : storage.stage;
    if (!isOwned(root, storage.marker))
        throw std::runtime_error("Torrent payload ownership changed.");
    for (const auto& file : *description.files) {
        if (file.padding)
            continue; // Padding is virtual; never let a padding path become a filesystem cleanup target.
        const auto path = root / ti::nativePath(file.path);
        noReparse(path);
        if (!torrentFileWanted(options, file)) {
            if (prune && std::filesystem::exists(path)) {
                if (!std::filesystem::is_regular_file(path))
                    throw std::runtime_error("An unwanted torrent file changed its type.");
                std::filesystem::remove(path);
            }
            continue;
        }
        if (prune && file.size == 0 && !std::filesystem::exists(path)) {
            std::filesystem::create_directories(path.parent_path());
            auto* output = files::openFile(path, L"wb");
            if (!output)
                throw std::runtime_error("Cannot create an empty torrent output file.");
            try {
                files::syncFile(output);
                fclose(output);
            } catch (...) {
                fclose(output);
                throw;
            }
        }
        if (!std::filesystem::is_regular_file(path) ||
            std::filesystem::file_size(path) != static_cast<std::uint64_t>(file.size))
            throw std::runtime_error("Verified torrent output is missing or has changed size.");
#ifdef _WIN32
        if (prune) {
            auto* output = files::openFile(path, L"rb+");
            if (!output)
                throw std::runtime_error("Cannot synchronize verified torrent output.");
            try {
                files::syncFile(output);
                fclose(output);
            } catch (...) {
                fclose(output);
                throw;
            }
        }
#endif
    }
}
struct Job {
    lt::torrent_handle handle;
    std::mutex mutex;
    std::condition_variable changed;
    bool paused = false, removed = false, failed = false, proxyAuthenticationFailed = false;
    bool finiteHintSource = false, peerEverConnected = false, hintedTunnelsRejected = false;
    std::set<lt::tcp::endpoint> remainingHints;
    std::uint64_t resumeSerial = 0;
    std::shared_ptr<const std::vector<char>> resume;
    bool resumeFailed = false;
};
class Runtime {
    lt::session session_;
    std::mutex jobsMutex_, shutdownMutex_;
    std::map<lt::torrent_handle, std::shared_ptr<Job>> jobs_;
    std::atomic_bool alive_{true};
    const bool proxied_;
    const bool httpProxy_;
    const bool automaticListenPorts_;
    const std::string originalListenInterfaces_;
    std::atomic_uint listenRetries_{0};
    std::atomic_int bindError_{0}, peerError_{0}, trackerError_{0};
    std::atomic_uint bindFailures_{0}, peerConnects_{0}, peerDisconnects_{0}, trackerReplies_{0};
    std::jthread dispatcher_;

    static bool automaticListenPorts(std::string_view interfaces) {
        if (interfaces.empty())
            return false;
        while (!interfaces.empty()) {
            const auto comma = interfaces.find(',');
            auto entry = interfaces.substr(0, comma);
            const auto first = entry.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return false;
            entry = entry.substr(first, entry.find_last_not_of(" \t\r\n") - first + 1);
            const auto colon = entry.rfind(':');
            if (colon == std::string_view::npos || colon == 0 || entry.substr(colon + 1) != "0")
                return false;
            if (comma == std::string_view::npos)
                return true;
            interfaces.remove_prefix(comma + 1);
        }
        return false;
    }

    std::string fallbackListenInterfaces() const {
        // TCP's ephemeral range can overlap ports excluded only for UDP.
        // Native binding still validates each randomly selected candidate.
        std::random_device random;
        const auto port = std::to_string(std::uniform_int_distribution<unsigned>(10240, 49151)(random));
        std::string result;
        std::string_view remaining(originalListenInterfaces_);
        while (!remaining.empty()) {
            const auto comma = remaining.find(',');
            auto entry = remaining.substr(0, comma);
            const auto first = entry.find_first_not_of(" \t\r\n");
            entry = entry.substr(first, entry.find_last_not_of(" \t\r\n") - first + 1);
            if (!result.empty())
                result += ',';
            result.append(entry.substr(0, entry.rfind(':') + 1));
            result += port;
            if (comma == std::string_view::npos)
                break;
            remaining.remove_prefix(comma + 1);
        }
        return result;
    }

    static lt::settings_pack settings(const TorrentRuntimeOptions& options, const DownloadSpec& route) {
        lt::settings_pack settings;
        settings.set_str(lt::settings_pack::listen_interfaces, options.listenInterfaces);
        settings.set_str(lt::settings_pack::user_agent, defaultUserAgent);
        settings.set_bool(lt::settings_pack::enable_dht, options.enableDiscovery);
        settings.set_bool(lt::settings_pack::enable_lsd, options.enableDiscovery);
        settings.set_bool(lt::settings_pack::enable_upnp, options.enableDiscovery);
        settings.set_bool(lt::settings_pack::enable_natpmp, options.enableDiscovery);
        settings.set_int(
            lt::settings_pack::alert_mask,
            static_cast<int>(static_cast<std::uint32_t>(
                lt::alert_category::status | lt::alert_category::storage | lt::alert_category::error |
                lt::alert_category::peer | lt::alert_category::connect | lt::alert_category::tracker)));
        settings.set_int(lt::settings_pack::alert_queue_size, 10000);
        settings.set_int(lt::settings_pack::max_metadata_size, static_cast<int>(maxTorrentMetadataBytes));
        const auto proxy = parseProxyEndpoint(route.proxy);
        if (proxy.type != ProxyType::Direct) {
            // A proxy listener is a native logical UDP route, never an incoming TCP listener.
            // Port zero is allocated by UDP itself; no TCP/UDP ephemeral-range mismatch.
            settings.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:0");
            settings.set_str(lt::settings_pack::proxy_hostname, proxy.host);
            settings.set_int(lt::settings_pack::proxy_port, proxy.port);
            const bool authenticated = !route.proxyUsername.empty();
            settings.set_int(lt::settings_pack::proxy_type,
                             proxy.type == ProxyType::Socks5
                                 ? (authenticated ? lt::settings_pack::socks5_pw : lt::settings_pack::socks5)
                                 : (authenticated ? lt::settings_pack::http_pw : lt::settings_pack::http));
            settings.set_str(lt::settings_pack::proxy_username, route.proxyUsername);
            settings.set_str(lt::settings_pack::proxy_password, route.proxyPassword);
            settings.set_bool(lt::settings_pack::proxy_hostnames, true);
            settings.set_bool(lt::settings_pack::proxy_peer_connections, true);
            settings.set_bool(lt::settings_pack::proxy_tracker_connections, true);
            settings.set_bool(lt::settings_pack::proxy_send_host_in_connect, true);
            for (const auto option :
                 {lt::settings_pack::enable_dht, lt::settings_pack::enable_lsd,
                  lt::settings_pack::enable_upnp, lt::settings_pack::enable_natpmp,
                  lt::settings_pack::enable_incoming_utp, lt::settings_pack::enable_outgoing_utp})
                settings.set_bool(option, false);
        }
        return settings;
    }
    void dispatch(std::stop_token stop) try {
        while (!stop.stop_requested()) {
            session_.wait_for_alert(lt::milliseconds(100));
            std::vector<lt::alert*> alerts;
            session_.pop_alerts(&alerts);
            bool retryListeners = false;
            for (const auto* alert : alerts) {
                if (const auto* failed = lt::alert_cast<lt::listen_failed_alert>(alert)) {
                    bindError_ = failed->error.value();
                    ++bindFailures_;
                }
                if (lt::alert_cast<lt::peer_connect_alert>(alert))
                    ++peerConnects_;
                if (const auto* disconnected = lt::alert_cast<lt::peer_disconnected_alert>(alert)) {
                    ++peerDisconnects_;
                    peerError_ = disconnected->error.value();
                }
                if (const auto* failed = lt::alert_cast<lt::peer_error_alert>(alert)) {
                    peerError_ = failed->error.value();
                }
                if (const auto* failed = lt::alert_cast<lt::tracker_error_alert>(alert)) {
                    trackerError_ = failed->error.value();
                }
                if (lt::alert_cast<lt::tracker_reply_alert>(alert))
                    ++trackerReplies_;
                if (const auto* failed = lt::alert_cast<lt::listen_failed_alert>(alert);
                    failed && automaticListenPorts_ && listenRetries_ < 8 &&
                    failed->op == lt::operation_t::sock_bind &&
                    failed->error == boost::asio::error::access_denied) {
                    retryListeners = true;
                }
                if (lt::alert_cast<lt::alerts_dropped_alert>(alert)) {
                    alive_ = false;
                    std::lock_guard lock(jobsMutex_);
                    for (const auto& [handle, job] : jobs_) {
                        std::lock_guard jobLock(job->mutex);
                        job->failed = true;
                        job->changed.notify_all();
                    }
                    continue;
                }
                const auto* torrent = dynamic_cast<const lt::torrent_alert*>(alert);
                if (!torrent)
                    continue;
                std::shared_ptr<Job> job;
                {
                    std::lock_guard lock(jobsMutex_);
                    const auto found = jobs_.find(torrent->handle);
                    if (found != jobs_.end())
                        job = found->second;
                }
                if (!job)
                    continue;
                std::lock_guard lock(job->mutex);
                const auto* peerFailure = lt::alert_cast<lt::peer_error_alert>(alert);
                const auto* disconnect = lt::alert_cast<lt::peer_disconnected_alert>(alert);
                const auto connectionError = peerFailure  ? peerFailure->error
                                             : disconnect ? disconnect->error
                                                          : lt::error_code{};
                const auto* connected = lt::alert_cast<lt::peer_connect_alert>(alert);
                if (connected && connected->direction == lt::peer_connect_alert::direction_t::in) {
                    job->peerEverConnected = true;
                    job->finiteHintSource = false;
                    job->hintedTunnelsRejected = false;
                    job->remainingHints.clear();
                }
                const bool connectFailure = (peerFailure && peerFailure->op == lt::operation_t::connect) ||
                                            (disconnect && disconnect->op == lt::operation_t::connect);
                if (job->finiteHintSource && !job->peerEverConnected && connectFailure &&
                    connectionError == boost::asio::error::operation_not_supported) {
                    // Native HTTP CONNECT maps every non-200 response to this code,
                    // including both authentication and destination-policy failures.
                    const auto* peer = dynamic_cast<const lt::peer_alert*>(alert);
                    if (peer)
                        if (const auto* endpoint = std::get_if<0>(&peer->ep))
                            job->remainingHints.erase(*endpoint);
                    job->hintedTunnelsRejected = job->remainingHints.empty();
                }
                if (proxied_ && (connectionError == lt::socks_error::authentication_error ||
                                 connectionError == lt::socks_error::unsupported_authentication_method ||
                                 connectionError == lt::socks_error::username_required))
                    job->proxyAuthenticationFailed = true;
                if (const auto* trackerFailure = lt::alert_cast<lt::tracker_error_alert>(alert);
                    proxied_ && trackerFailure &&
                    (trackerFailure->error.value() == 407 ||
                     trackerFailure->error == lt::socks_error::authentication_error ||
                     trackerFailure->error == lt::socks_error::unsupported_authentication_method ||
                     trackerFailure->error == lt::socks_error::username_required))
                    job->proxyAuthenticationFailed = true;
                if (lt::alert_cast<lt::torrent_paused_alert>(alert))
                    job->paused = true;
                else if (lt::alert_cast<lt::torrent_removed_alert>(alert))
                    job->removed = true;
                else if (const auto* saved = lt::alert_cast<lt::save_resume_data_alert>(alert)) {
                    try {
                        auto bytes =
                            std::make_shared<std::vector<char>>(lt::write_resume_data_buf(saved->params));
                        job->resumeFailed = bytes->size() > maxResumeBytes;
                        if (!job->resumeFailed)
                            job->resume = std::move(bytes);
                    } catch (...) {
                        job->resumeFailed = true;
                    }
                    ++job->resumeSerial;
                } else if (lt::alert_cast<lt::save_resume_data_failed_alert>(alert)) {
                    job->resumeFailed = true;
                    ++job->resumeSerial;
                } else if (lt::alert_cast<lt::file_error_alert>(alert) ||
                           lt::alert_cast<lt::torrent_error_alert>(alert))
                    job->failed = true;
                job->changed.notify_all();
            }
            if (retryListeners && !stop.stop_requested() && alive_ && !session_.is_listening()) {
                ++listenRetries_;
                // Never replace any successful listener or a user-selected port.
                lt::settings_pack retry;
                retry.set_str(lt::settings_pack::listen_interfaces, fallbackListenInterfaces());
                session_.apply_settings(std::move(retry));
            }
        }
    } catch (...) {
        alive_ = false;
        std::lock_guard lock(jobsMutex_);
        for (const auto& [handle, job] : jobs_)
            job->changed.notify_all();
    }
    template <typename Predicate> bool wait(const std::shared_ptr<Job>& job, Predicate predicate) {
        std::unique_lock lock(job->mutex);
        return job->changed.wait_for(lock, 30s, [&] { return predicate(*job) || !alive_; });
    }

  public:
    Runtime(const TorrentRuntimeOptions& options, const DownloadSpec& route)
        : session_(lt::session_params(settings(options, route))), proxied_(!route.proxy.empty()),
          httpProxy_(parseProxyEndpoint(route.proxy).type == ProxyType::Http),
          automaticListenPorts_(route.proxy.empty() && automaticListenPorts(options.listenInterfaces)),
          originalListenInterfaces_(options.listenInterfaces),
          dispatcher_([this](std::stop_token stop) { dispatch(stop); }) {}
    ~Runtime() { abortAll(); }
    bool alive() const { return alive_; }
    bool proxyAuthenticationFailed(const std::shared_ptr<Job>& job) const {
        std::lock_guard lock(job->mutex);
        return job->proxyAuthenticationFailed;
    }
    bool hintedTunnelsRejected(const std::shared_ptr<Job>& job) const {
        {
            std::lock_guard lock(job->mutex);
            if (!job->finiteHintSource)
                return false;
        }
        // Outgoing peer_connect_alert is posted when async_connect is scheduled,
        // not when its tunnel succeeds. Query actual completed peer handshakes.
        std::vector<lt::peer_info> peers;
        job->handle.get_peer_info(peers);
        const bool established = std::any_of(peers.begin(), peers.end(), [](const auto& peer) {
            return !(peer.flags & (lt::peer_info::connecting | lt::peer_info::handshake));
        });
        std::lock_guard lock(job->mutex);
        if (established) {
            job->peerEverConnected = true;
            job->finiteHintSource = false;
            job->hintedTunnelsRejected = false;
            job->remainingHints.clear();
        }
        return !job->peerEverConnected && job->hintedTunnelsRejected;
    }
    std::string networkDiagnostics() const {
        // Shared-session counters contain no endpoints, URLs, passkeys or server text.
        return " [BT net listening=" + std::to_string(session_.is_listening()) +
               " port=" + std::to_string(session_.listen_port()) +
               " bind=" + std::to_string(bindError_.load()) +
               " bindFailures=" + std::to_string(bindFailures_.load()) +
               " retries=" + std::to_string(listenRetries_.load()) +
               " connectEvents=" + std::to_string(peerConnects_.load()) +
               " disconnected=" + std::to_string(peerDisconnects_.load()) +
               " peerError=" + std::to_string(peerError_.load()) +
               " trackerError=" + std::to_string(trackerError_.load()) +
               " trackerReplies=" + std::to_string(trackerReplies_.load()) + "]";
    }
    void abortAll() {
        std::lock_guard shutdownLock(shutdownMutex_);
        alive_ = false;
        if (dispatcher_.joinable()) {
            dispatcher_.request_stop();
            dispatcher_.join();
            // The proxy waits for network and disk workers. No terminal callback may bypass this barrier.
            auto shutdownProxy = session_.abort();
        }
        std::lock_guard lock(jobsMutex_);
        for (const auto& [handle, job] : jobs_) {
            std::lock_guard jobLock(job->mutex);
            job->removed = true;
            job->failed = true;
            job->changed.notify_all();
        }
        jobs_.clear();
    }
    std::shared_ptr<Job> add(lt::add_torrent_params params, const TransferControl& control,
                             bool allowFiniteHintSource = true) {
        // Tracker endpoints require a native route socket, including the logical
        // UDP socket of proxy sessions (which never accepts direct TCP peers).
        // Do not let initial peer attempts acquire reconnect backoff during startup.
        const auto deadline = Clock::now() + 5s;
        for (;;) {
            if (stopped(control))
                return {};
            if (!alive_)
                throw std::runtime_error("Torrent engine was stopped. Retry to create a fresh session.");
            if (session_.is_listening())
                break;
            if (Clock::now() >= deadline)
                throw std::runtime_error("Cannot open a torrent network listener. Check local network "
                                         "permissions and retry.");
            std::this_thread::sleep_for(10ms);
        }
        if (stopped(control))
            return {};
        auto job = std::make_shared<Job>();
        // Under ABI 100 all metainfo tracker/web-seed sources live in these
        // add parameters, including sources loaded from an authoritative ti.
        job->finiteHintSource = allowFiniteHintSource && httpProxy_ && params.trackers.empty() &&
                                params.url_seeds.empty() && !params.peers.empty() &&
                                params.peers.size() <= 256 &&
                                !(params.flags & lt::torrent_flags::upload_mode);
        if (job->finiteHintSource) {
            // Match native peer_list::add_peer's unconditional ignored entries;
            // these never become candidates and never emit connect-rejection alerts.
            for (const auto& peer : params.peers)
                if (peer.address() != lt::address{} && peer.port() > 1)
                    job->remainingHints.insert(peer);
            job->finiteHintSource = !job->remainingHints.empty();
        }
        lt::error_code error;
        // add_torrent posts alerts but does not await our dispatcher. Keep its
        // fast failures queued behind registration instead of dropping them.
        std::lock_guard lock(jobsMutex_);
        job->handle = session_.add_torrent(std::move(params), error);
        if (error || !job->handle.is_valid())
            throw std::runtime_error(
                "Cannot add torrent. It may already be active or its metadata is invalid.");
        jobs_.emplace(job->handle, job);
        return job;
    }
    std::shared_ptr<const std::vector<char>> snapshot(const std::shared_ptr<Job>& job) {
        std::uint64_t requested;
        {
            std::lock_guard lock(job->mutex);
            requested = job->resumeSerial + 1;
        }
        job->handle.save_resume_data(lt::torrent_handle::save_info_dict);
        if (!wait(job,
                  [&](const Job& current) { return current.resumeSerial >= requested || current.removed; }))
            throw std::runtime_error("Timed out saving torrent recovery state.");
        std::lock_guard lock(job->mutex);
        if (!alive_ || job->removed || job->resumeFailed || !job->resume)
            throw std::runtime_error("Cannot save torrent recovery state.");
        return job->resume;
    }
    void remove(std::shared_ptr<Job>& job, const std::filesystem::path& resumePath, bool save) {
        if (!job)
            return;
        if (!alive_) {
            abortAll();
            job.reset();
            return;
        }
        try {
            bool paused;
            {
                std::lock_guard lock(job->mutex);
                paused = job->paused;
            }
            if (!paused)
                job->handle.pause();
            if (!wait(job, [](const Job& current) { return current.paused || current.removed; }))
                throw std::runtime_error("Timed out closing torrent files.");
            if (!alive_)
                throw std::runtime_error("Torrent alert delivery failed before files were closed.");
            if (save) {
                bool failed;
                {
                    std::lock_guard lock(job->mutex);
                    failed = job->failed;
                }
                if (failed || job->handle.status().errc)
                    throw std::runtime_error(
                        "Torrent storage reported an error before publication. Output was preserved.");
            }
            if (save && alive_) {
                const auto bytes = snapshot(job);
                files::atomicText(resumePath, std::string(bytes->begin(), bytes->end()));
            }
            if (alive_) {
                session_.remove_torrent(job->handle);
                if (!wait(job, [](const Job& current) { return current.removed; }))
                    throw std::runtime_error("Timed out releasing torrent storage.");
                if (!alive_)
                    throw std::runtime_error("Torrent alert delivery failed before storage was released.");
            }
            {
                std::lock_guard lock(jobsMutex_);
                jobs_.erase(job->handle);
            }
            job.reset();
        } catch (...) {
            // A lost alert or failed disk barrier cannot make cleanup race live disk jobs.
            abortAll();
            job.reset();
            throw;
        }
    }
};
struct LazyRuntime {
    TorrentRuntimeOptions options;
    std::mutex mutex;
    // Private process-local keys are never serialized/logged. Idle sessions expire;
    // their credential-bearing keys are pruned on the next acquisition or destruction.
    using RouteKey = std::tuple<std::string, ProxyType, std::string, int, std::string, std::string>;
    std::map<RouteKey, std::weak_ptr<Runtime>> runtimes;
    explicit LazyRuntime(TorrentRuntimeOptions configured) : options(std::move(configured)) {}
    std::shared_ptr<Runtime> get(const Task& task) {
        const auto& route = task.spec;
        std::lock_guard lock(mutex);
        std::erase_if(runtimes, [](const auto& entry) { return entry.second.expired(); });
        const auto proxy = parseProxyEndpoint(route.proxy);
        RouteKey key{task.ownerId, proxy.type,          proxy.host,
                     proxy.port,   route.proxyUsername, route.proxyPassword};
        auto runtime = runtimes[key].lock();
        if (!runtime || !runtime->alive()) {
            runtime = std::make_shared<Runtime>(options, route);
            runtimes[std::move(key)] = runtime;
        }
        return runtime;
    }
};
lt::add_torrent_params parameters(const ti::ParsedTorrent& parsed, const Task& task, const Storage& storage,
                                  bool published, const TransferControl& control) {
    auto params = parsed.params;
    params.save_path = ti::pathText(published ? storage.final : storage.stage);
    params.part_file_dir = storage.parts;
    params.renamed_files.clear();
    params.have_pieces.clear();
    params.verified_pieces.clear();
    params.unfinished_pieces.clear();
    params.flags &=
        ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused | lt::torrent_flags::seed_mode);
    params.flags |= lt::torrent_flags::default_dont_download;
    params.flags |= lt::torrent_flags::duplicate_is_error;
    if (published)
        params.flags |= lt::torrent_flags::upload_mode;
    else
        params.flags &= ~lt::torrent_flags::upload_mode;
    params.file_priorities.clear();
    if (parsed.description.files)
        for (const auto& file : *parsed.description.files) {
            params.file_priorities.push_back(torrentFileWanted(task.spec.torrent, file) ? lt::default_priority
                                                                                        : lt::dont_download);
            params.renamed_files[lt::file_index_t(file.index)] = file.path;
        }
    params.piece_priorities.clear();
    params.upload_limit = static_cast<int>(
        std::clamp<std::int64_t>(control.uploadLimit.load(), 0, std::numeric_limits<int>::max()));
    params.download_limit = static_cast<int>(
        std::clamp<std::int64_t>(control.speedLimit.load(), 0, std::numeric_limits<int>::max()));
    params.total_uploaded = std::max<std::int64_t>(0, task.uploaded);
    params.max_connections = 100;
    return params;
}
void saveSnapshot(const std::shared_ptr<Runtime>& runtime, const std::shared_ptr<Job>& job,
                  const std::filesystem::path& cache) {
    const auto bytes = runtime->snapshot(job);
    files::atomicText(cache / "resume", std::string(bytes->begin(), bytes->end()));
}
class TorrentTransfer final : public ITransferTransport {
    std::shared_ptr<LazyRuntime> owner_;

  public:
    explicit TorrentTransfer(std::shared_ptr<LazyRuntime> runtime) : owner_(std::move(runtime)) {}
    TransferDescription describe(const DownloadSpec& spec) override { return describeTorrent(spec); }
    TorrentMetadata captureMetadata(const DownloadSpec& spec) override { return ti::readMetadata(spec); }
    void discard(const Task& task, const std::filesystem::path& work) override {
        const auto parsed = ti::parse(task.spec);
        const auto storage = storageFor(task, work, parsed.description.infoHash, false, false);
        // Published payload is never a cleanup target, even after canceling a seed.
        for (const auto& root : {storage.stage, storage.cache}) {
            if (!isOwned(root, storage.marker))
                continue;
            inspectTree(root);
            std::filesystem::remove_all(root);
        }
    }
    void run(const Task& task, const std::filesystem::path& work, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        TransferEvent event;
        event.phase = TaskState::Probing;
        event.torrentPublished = task.torrentPublished;
        std::shared_ptr<Runtime> runtime;
        std::shared_ptr<Job> job;
        std::optional<Storage> storage;
        TransferOutcome outcome = TransferOutcome::Attention;
        std::string error;
        try {
            if (const auto invalid = validateSpec(task.spec); !invalid.empty())
                throw std::invalid_argument(invalid);
            auto authenticatedSpec = task.spec;
            authenticatedSpec.originAuthenticationRequired =
                task.originAuthenticationRequired || requiresOriginAuthentication(task.spec);
            if (const auto originError = validateOriginAuthentication(authenticatedSpec, true);
                !originError.empty())
                throw std::invalid_argument(originError);
            if (task.ownerId != "standalone" && !isOpaqueId(task.ownerId))
                throw std::invalid_argument("Invalid task owner identity.");
            if (const auto invalid = validateProxy(task.spec, true); !invalid.empty())
                throw std::invalid_argument(invalid);
            if (parseProxyEndpoint(task.spec.proxy).type == ProxyType::Https)
                throw std::invalid_argument(
                    "BitTorrent does not support TLS-to-proxy connections. Use an HTTP or SOCKS5 proxy.");
            auto parsed = ti::parse(task.spec);
            storage = storageFor(task, work, parsed.description.infoHash, true);
            event.torrentPublished = storage->published;
            // Resume blobs are never trusted for paths, flags or verified pieces. All recovered bytes are
            // rechecked.
            if (!parsed.metadata) {
                const auto bytes = readBounded(storage->cache / "resume", maxResumeBytes);
                if (!bytes.empty()) {
                    try {
                        lt::error_code resumeError;
                        auto resume = lt::read_resume_data(lt::span<char const>(bytes.data(), bytes.size()),
                                                           resumeError);
                        if (!resumeError && resume.ti &&
                            ti::hashesMatch(parsed.params.info_hashes, resume.ti->info_hashes())) {
                            // Only the hash-bound metainfo is recovered; cached network destinations are not
                            // authority.
                            resume.trackers = parsed.params.trackers;
                            resume.tracker_tiers = parsed.params.tracker_tiers;
                            resume.url_seeds = parsed.params.url_seeds;
                            resume.dht_nodes.clear();
                            auto spec = task.spec;
                            spec.torrent.metadata = ti::metadataFrom(resume);
                            parsed = ti::parse(spec);
                            event.torrentMetadata = parsed.metadata;
                        }
                    } catch (const std::exception&) {
                        // Damaged recovery data falls back to validated metadata fetch and piece checking.
                    }
                }
            }
            if (parsed.description.files)
                event.description = std::make_shared<TransferDescription>(parsed.description);
            if (storage->published && parsed.description.files)
                verifyPublishedFiles(*storage, parsed.description, task.spec.torrent, false);
            event.phase = parsed.description.files ? TaskState::Probing : TaskState::FetchingMetadata;
            callback(event);
            event.description.reset();
            event.torrentMetadata.reset();
            if (stopped(control)) {
                outcome = control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused;
            } else if (parsed.description.files && !hasSelection(parsed.description, task.spec.torrent)) {
                outcome = TransferOutcome::SelectionRequired;
            } else {
                checkProxyEndpoint(task.spec, control);
                runtime = owner_->get(task);
                job = runtime->add(parameters(parsed, task, *storage, storage->published, control), control);
                bool haveMetadata = static_cast<bool>(parsed.description.files);
                bool prioritiesReady = haveMetadata;
                bool checking = true;
                auto lastEvent = Clock::now() - 1s, lastSave = Clock::now();
                std::int64_t lastDownloadLimit = -1, lastUploadLimit = -1;
                for (;;) {
                    if (stopped(control)) {
                        outcome = control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused;
                        break;
                    }
                    if (!runtime->alive())
                        throw std::runtime_error(
                            "Torrent engine stopped before storage ownership was released.");
                    if (runtime->proxyAuthenticationFailed(job))
                        throw std::runtime_error("Proxy authentication failed. Check the proxy credentials.");
                    if (runtime->hintedTunnelsRejected(job))
                        throw std::runtime_error("Proxy tunnels to all supplied peers were rejected. Check "
                                                 "proxy credentials and peer availability.");
                    const auto status = job->handle.status();
                    if (status.errc)
                        throw std::runtime_error("Torrent transfer or file verification failed. Check "
                                                 "storage and source availability.");
                    if (!haveMetadata && status.has_metadata) {
                        const auto snapshot = runtime->snapshot(job);
                        lt::error_code resumeError;
                        const auto params = lt::read_resume_data(
                            lt::span<char const>(snapshot->data(), snapshot->size()), resumeError);
                        if (resumeError || !params.ti)
                            throw std::runtime_error("Received torrent metadata could not be inspected.");
                        auto spec = task.spec;
                        spec.torrent.metadata = ti::metadataFrom(params);
                        parsed = ti::parse(spec);
                        // Preserve peers discovered by this live metadata handle for tracker-only magnets.
                        // Disk resume peer lists never enter this handoff or become routing authority.
                        auto hintedPeers = std::move(parsed.params.peers);
                        parsed.params.peers.clear();
                        auto appendPeers = [&](const auto& peers) {
                            for (const auto& peer : peers) {
                                if (parsed.params.peers.size() >= 256)
                                    break;
                                if (std::find(parsed.params.peers.begin(), parsed.params.peers.end(), peer) ==
                                        parsed.params.peers.end() &&
                                    std::find(params.banned_peers.begin(), params.banned_peers.end(), peer) ==
                                        params.banned_peers.end())
                                    parsed.params.peers.push_back(peer);
                            }
                        };
                        appendPeers(hintedPeers);
                        appendPeers(params.peers);
                        haveMetadata = true;
                        if (storage->published)
                            verifyPublishedFiles(*storage, parsed.description, task.spec.torrent, false);
                        // A metadata-only torrent has zero wanted bytes and can already have entered
                        // libtorrent's finished state, disconnecting seeds with a reconnect backoff.
                        // Close that handle before starting a distinct validated payload lifecycle.
                        runtime->remove(job, storage->cache / "resume", true);
                        event.description = std::make_shared<TransferDescription>(parsed.description);
                        event.torrentMetadata = parsed.metadata;
                        callback(event);
                        event.description.reset();
                        event.torrentMetadata.reset();
                        if (stopped(control)) {
                            outcome = control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused;
                            break;
                        }
                        if (!hasSelection(parsed.description, task.spec.torrent)) {
                            outcome = TransferOutcome::SelectionRequired;
                            break;
                        }
                        job = runtime->add(parameters(parsed, task, *storage, storage->published, control),
                                           control, false);
                        prioritiesReady = true;
                        lastDownloadLimit = lastUploadLimit = -1;
                        lastSave = Clock::now();
                        continue;
                    }
                    const auto downloadLimit = std::clamp<std::int64_t>(control.speedLimit.load(), 0,
                                                                        std::numeric_limits<int>::max());
                    const auto uploadLimit = std::clamp<std::int64_t>(control.uploadLimit.load(), 0,
                                                                      std::numeric_limits<int>::max());
                    if (downloadLimit != lastDownloadLimit) {
                        job->handle.set_download_limit(static_cast<int>(downloadLimit));
                        lastDownloadLimit = downloadLimit;
                    }
                    if (uploadLimit != lastUploadLimit) {
                        job->handle.set_upload_limit(static_cast<int>(uploadLimit));
                        lastUploadLimit = uploadLimit;
                    }
                    checking = status.state == lt::torrent_status::checking_files ||
                               status.state == lt::torrent_status::checking_resume_data;
                    event.phase = !haveMetadata                              ? TaskState::FetchingMetadata
                                  : checking                                 ? TaskState::Verifying
                                  : storage->published && status.is_finished ? TaskState::Seeding
                                                                             : TaskState::Downloading;
                    event.total = haveMetadata ? wantedBytes(parsed.description, task.spec.torrent) : -1;
                    event.downloaded = std::max<std::int64_t>(0, status.total_wanted_done);
                    event.speed = std::max(0, status.download_payload_rate);
                    event.uploaded = std::max<std::int64_t>(task.uploaded, status.all_time_upload);
                    event.uploadSpeed = std::max(0, status.upload_payload_rate);
                    event.peers = std::max(0, status.num_peers);
                    event.seeds = std::max(0, status.num_seeds);
                    event.resumable = true;
                    event.torrentPublished = storage->published;
                    if (Clock::now() - lastEvent >= 200ms) {
                        callback(event);
                        lastEvent = Clock::now();
                    }
                    if (storage->published && haveMetadata && !checking) {
                        if (!status.is_finished)
                            throw std::runtime_error("Published torrent bytes failed verification. They were "
                                                     "preserved without repair.");
                        if (!task.spec.torrent.seedAfterDownload || control.finish) {
                            outcome = TransferOutcome::Complete;
                            break;
                        }
                    } else if (haveMetadata && prioritiesReady && !checking && status.is_finished &&
                               status.total_wanted >= event.total) {
                        event.phase = TaskState::Verifying;
                        event.downloaded = event.total;
                        callback(event);
                        runtime->remove(job, storage->cache / "resume", true);
                        if (stopped(control)) {
                            outcome = control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused;
                            break;
                        }
                        verifyPublishedFiles(*storage, parsed.description, task.spec.torrent, true);
                        event.phase = TaskState::Finalizing;
                        callback(event);
                        if (stopped(control)) {
                            outcome = control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused;
                            break;
                        }
                        publish(*storage);
                        event.torrentPublished = true;
                        if (!task.spec.torrent.seedAfterDownload) {
                            outcome = TransferOutcome::Complete;
                            break;
                        }
                        auto seed = parameters(parsed, task, *storage, true, control);
                        seed.total_uploaded = event.uploaded;
                        job = runtime->add(std::move(seed), control, false);
                        lastDownloadLimit = lastUploadLimit = -1;
                        lastSave = Clock::now();
                        continue;
                    }
                    if (Clock::now() - lastSave >= 5s) {
                        saveSnapshot(runtime, job, storage->cache);
                        lastSave = Clock::now();
                    }
                    std::this_thread::sleep_for(100ms);
                }
            }
        } catch (const ProxyCheckStopped&) {
            outcome = control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused;
        } catch (const std::exception& exception) {
            outcome = stopped(control)
                          ? (control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused)
                          : TransferOutcome::Attention;
            error = exception.what();
        }
        std::string networkDiagnostics;
        if (runtime && outcome != TransferOutcome::Complete) {
            try {
                networkDiagnostics = runtime->networkDiagnostics();
            } catch (...) {
                networkDiagnostics = " [BT network diagnostics unavailable]";
            }
        }
        if (job) {
            try {
                runtime->remove(job, storage->cache / "resume", outcome != TransferOutcome::Cancelled);
            } catch (const std::exception& exception) {
                outcome = TransferOutcome::Attention;
                error = exception.what();
            }
        }
        if (storage)
            event.torrentPublished = storage->published;
        event.description.reset();
        event.torrentMetadata.reset();
        event.outcome = outcome;
        if (outcome == TransferOutcome::Attention || outcome == TransferOutcome::Failed)
            error += networkDiagnostics;
        event.error = std::move(error);
        event.speed = event.uploadSpeed = 0;
        callback(event);
    }
};
} // namespace
TransportFactory makeTorrentTransferFactory(TorrentRuntimeOptions options) {
    auto runtime = std::make_shared<LazyRuntime>(std::move(options));
    return [runtime] { return std::make_unique<TorrentTransfer>(runtime); };
}
} // namespace maskpilot
