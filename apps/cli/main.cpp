#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/media_tools.h>
#include <maskpilot/infrastructure/torrent_engine.h>
#include <memory>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
std::int64_t integerValue(const std::string& text) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("Expected an integer option value.");
    return value;
}
std::string credentialFromEnvironment(const std::string& name) {
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t size = 0;
    const auto result = _dupenv_s(&raw, &size, name.c_str());
    const std::unique_ptr<char, decltype(&std::free)> secret(raw, std::free);
    if (result != 0 || !raw)
        throw std::invalid_argument("Proxy credential environment variable is unavailable.");
    return raw;
#else
    const auto* secret = std::getenv(name.c_str());
    if (!secret)
        throw std::invalid_argument("Proxy credential environment variable is unavailable.");
    return secret;
#endif
}
bool readOptions(maskpilot::DownloadSpec& spec, int argc, char** argv, int start) {
    bool inspection = false;
    for (int n = start; n < argc; ++n) {
        const std::string option = argv[n];
        if (option == "--media")
            spec.kind = maskpilot::TransferKind::Media;
        else if (option == "--inspect-media") {
            spec.kind = maskpilot::TransferKind::Media;
            inspection = true;
        } else if (option == "--audio-only") {
            spec.kind = maskpilot::TransferKind::Media;
            spec.audioOnly = true;
        } else if (option == "--origin-auth-required")
            spec.originAuthenticationRequired = true;
        else if (option == "--allow-anonymous")
            spec.originAuthenticationRequired = false;
        else if (option == "--proxy-auth-required")
            spec.proxyAuthenticationRequired = true;
        else if (option == "--ftp-explicit-tls")
            spec.ftpExplicitTls = true;
        else if (option == "--ftp-active")
            spec.ftpActiveMode = true;
        else if (option == "--seed")
            spec.torrent.seedAfterDownload = true;
        else if (option == "--no-seed")
            spec.torrent.seedAfterDownload = false;
        else if (option == "--choose-files") {
            spec.torrent.allFiles = false;
            spec.torrent.selectedFiles.clear();
        } else if (option == "--all-files") {
            spec.torrent.allFiles = true;
            spec.torrent.selectedFiles.clear();
        } else {
            if (++n >= argc)
                throw std::invalid_argument("Missing option value.");
            const std::string value = argv[n];
            if (option.starts_with("--media-"))
                spec.kind = maskpilot::TransferKind::Media;
            if (option == "--media-parser" || option == "--media-muxer") {
                (option == "--media-parser" ? spec.mediaDownloader : spec.mediaMuxer) =
                    std::filesystem::path(std::u8string(value.begin(), value.end()));
            } else if (option == "--media-format-id")
                spec.mediaSelection.primaryFormat = value;
            else if (option == "--media-audio-id")
                spec.mediaSelection.audioFormat = value;
            else if (option == "--media-mode") {
                using Mode = maskpilot::MediaSelectionMode;
                if (value == "auto")
                    spec.mediaSelection.mode = Mode::Automatic;
                else if (value == "muxed")
                    spec.mediaSelection.mode = Mode::Muxed;
                else if (value == "video-audio")
                    spec.mediaSelection.mode = Mode::VideoAudio;
                else if (value == "audio")
                    spec.mediaSelection.mode = Mode::AudioOnly;
                else if (value == "video")
                    spec.mediaSelection.mode = Mode::VideoOnly;
                else
                    throw std::invalid_argument("Unknown media selection mode.");
            } else if (option == "--media-container") {
                using Container = maskpilot::MediaContainer;
                if (value == "auto")
                    spec.mediaSelection.container = Container::Automatic;
                else if (value == "mkv")
                    spec.mediaSelection.container = Container::Mkv;
                else if (value == "mp4")
                    spec.mediaSelection.container = Container::Mp4;
                else if (value == "webm")
                    spec.mediaSelection.container = Container::Webm;
                else if (value == "m4a")
                    spec.mediaSelection.container = Container::M4a;
                else if (value == "mp3")
                    spec.mediaSelection.container = Container::Mp3;
                else if (value == "opus")
                    spec.mediaSelection.container = Container::Opus;
                else if (value == "flac")
                    spec.mediaSelection.container = Container::Flac;
                else
                    throw std::invalid_argument("Unknown media output container.");
            } else if (option == "--connections") {
                const auto count = integerValue(value);
                if (count < 1 || count > 8)
                    throw std::invalid_argument("Connections must be between 1 and 8.");
                spec.connections = static_cast<int>(count);
            } else if (option == "--sha256")
                spec.expectedSha256 = value;
            else if (option == "--proxy")
                spec.proxy = value;
            else if (option == "--proxy-user")
                spec.proxyUsername = value;
            else if (option == "--proxy-password")
                spec.proxyPassword = value;
            else if (option == "--proxy-user-env" || option == "--proxy-password-env") {
                if (value.empty() || value.size() > 128 ||
                    !std::all_of(value.begin(), value.end(),
                                 [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
                    throw std::invalid_argument("Invalid proxy credential environment variable name.");
                (option == "--proxy-user-env" ? spec.proxyUsername : spec.proxyPassword) =
                    credentialFromEnvironment(value);
            } else if (option == "--ftp-user")
                spec.ftpUsername = value;
            else if (option == "--ftp-password")
                spec.ftpPassword = value;
            else if (option == "--ftp-ca")
                spec.ftpCaBundle = std::filesystem::path(std::u8string(value.begin(), value.end()));
            else if (option == "--upload-limit")
                spec.torrent.uploadLimit = integerValue(value);
            else if (option == "--select-files") {
                spec.torrent.allFiles = false;
                spec.torrent.selectedFiles.clear();
                std::size_t begin = 0;
                do {
                    const auto end = value.find(',', begin);
                    const auto index =
                        integerValue(value.substr(begin, end == std::string::npos ? end : end - begin));
                    if (index < 0 || index >= static_cast<std::int64_t>(maskpilot::maxTorrentFiles))
                        throw std::invalid_argument("Invalid torrent file index.");
                    spec.torrent.selectedFiles.push_back(static_cast<int>(index));
                    if (end == std::string::npos)
                        break;
                    begin = end + 1;
                } while (begin <= value.size());
            } else
                throw std::invalid_argument("Unknown option.");
        }
    }
    return inspection;
}
void inspectMedia(maskpilot::DownloadService& service, maskpilot::DownloadSpec spec,
                  const std::string& taskId = {}) {
    auto resolved = service.resolveMediaInspection({std::move(spec), taskId}).get();
    auto inventory = maskpilot::makeMediaInspector()->inspect(resolved, {});
    QJsonArray formats;
    for (const auto& format : inventory.formats)
        formats.append(QJsonObject{{"id", QString::fromStdString(format.id)},
                                   {"extension", QString::fromStdString(format.extension)},
                                   {"videoCodec", QString::fromStdString(format.videoCodec)},
                                   {"audioCodec", QString::fromStdString(format.audioCodec)},
                                   {"protocol", QString::fromStdString(format.protocol)},
                                   {"width", format.width},
                                   {"height", format.height},
                                   {"fps", format.fps},
                                   {"bitrate", format.bitrate},
                                   {"bytes", static_cast<qint64>(format.bytes)},
                                   {"estimated", format.estimated},
                                   {"hasVideo", format.hasVideo},
                                   {"hasAudio", format.hasAudio}});
    const QJsonObject result{{"title", QString::fromStdString(inventory.title)}, {"formats", formats}};
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
}
} // namespace
int runClient(int argc, char** argv) {
    maskpilot::TorrentRuntimeOptions torrentRuntime;
    // Runtime options precede the command, so strings used as task option values are never reinterpreted.
    if (argc > 1 && std::string(argv[1]) == "--bt-no-discovery") {
        torrentRuntime.enableDiscovery = false;
        --argc;
        ++argv;
    }
    if (argc < 4) {
        std::cout
            << "MaskPilot " << maskpilot::applicationVersion
            << " verification client\nUsage: maskpilot_cli [--bt-no-discovery] URL DESTINATION "
               "DATA_DIRECTORY "
               "[connections] [sha256]\n       maskpilot_cli --resume TASK_ID DATA_DIRECTORY [options]\n"
               "Options: --connections N --sha256 HEX --proxy URL --ftp-user USER --ftp-password PASS\n"
               "         --ftp-explicit-tls --ftp-active --ftp-ca PEM_FILE\n";
        std::cout << "Proxy: --proxy-user-env NAME --proxy-password-env NAME --proxy-auth-required\n"
                     "       --proxy-user USER --proxy-password PASS (visible in process arguments)\n";
        std::cout << "Source: --origin-auth-required preserves a missing-login requirement; "
                     "--allow-anonymous explicitly clears it when no source credentials are supplied.\n";
        std::cout << "Torrent: local.torrent or magnet:? URI, destination is a new directory.\n"
                     "         --choose-files --select-files 0,2 --all-files --seed --no-seed --upload-limit "
                     "BYTES\n"
                     "Global --bt-no-discovery disables DHT, local discovery and automatic port mapping; "
                     "explicit trackers and peers remain enabled.\n"
                     "This verification client pauses unfinished transfers after 180 seconds.\n";
        std::cout << "Media: --media --inspect-media --media-parser EXE --media-muxer EXE\n"
                     "       --media-mode auto|muxed|video-audio|audio|video --media-format-id ID\n"
                     "       --media-audio-id ID --media-container auto|mkv|mp4|webm|m4a|mp3|opus|flac\n";
        return argc == 1 ? 0 : 2;
    }
    try {
        int qtArgc = 1;
        char qtName[] = "maskpilot_cli";
        char* qtArgv[] = {qtName, nullptr};
        QCoreApplication application(qtArgc, qtArgv);
        auto data = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[3])));
        std::filesystem::create_directories(data);
        QLockFile lock(QDir(QString::fromUtf8(argv[3])).absoluteFilePath("instance.lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            throw std::runtime_error(
                "Application data directory is already in use. Close its desktop or client first.");
        auto store = maskpilot::makeSqliteStore(data / "tasks.db");
        maskpilot::DownloadService service(
            std::move(store.tasks),
            maskpilot::makeUnifiedTransferFactory(maskpilot::makeTorrentTransferFactory(torrentRuntime)),
            data / "parts", {},
            maskpilot::QueueDependencies{std::move(store.queues), maskpilot::makeQueueCivilTime(),
                                         maskpilot::InitialQueueLimits{}, std::move(store.traffic)});
        std::string id;
        if (std::string(argv[1]) == "--resume" || std::string(argv[1]) == "--change-url") {
            id = argv[2];
            const auto tasks = service.snapshot();
            const auto existing =
                std::find_if(tasks.begin(), tasks.end(), [&](const auto& task) { return task.id == id; });
            if (existing == tasks.end())
                throw std::invalid_argument("Task was not found.");
            auto spec = existing->spec;
            int start = 4;
            if (std::string(argv[1]) == "--change-url") {
                if (argc < 5)
                    return 2;
                if (existing->spec.kind == maskpilot::TransferKind::BitTorrent ||
                    maskpilot::isTorrentUrl(argv[4]))
                    throw std::invalid_argument("Create a new task when changing a torrent source.");
                spec.url = argv[4];
                start = 5;
                spec.cookie.clear();
                spec.authorization.clear();
                spec.headers.clear();
                spec.referrer.clear();
                spec.ftpUsername.clear();
                spec.ftpPassword.clear();
                if (!maskpilot::isFtpUrl(spec.url)) {
                    spec.ftpExplicitTls = false;
                    spec.ftpActiveMode = false;
                    spec.ftpCaBundle.clear();
                }
            }
            if (readOptions(spec, argc, argv, start)) {
                inspectMedia(service, std::move(spec), id);
                return 0;
            }
            if (const auto error = maskpilot::validateSpec(spec); !error.empty())
                throw std::invalid_argument(error);
            const bool keepStored = !existing->proxyCredentialRef.empty() &&
                                    spec.proxy == existing->spec.proxy && spec.proxyUsername.empty() &&
                                    spec.proxyPassword.empty();
            service.updateRequestWithCredentials(id, std::move(spec), keepStored).get();
            service.resume(id);
        } else {
            maskpilot::DownloadSpec spec;
            spec.url = argv[1];
            if (!maskpilot::isTorrentUrl(spec.url) && spec.url.find("://") == std::string::npos) {
                const std::filesystem::path source(std::u8string(spec.url.begin(), spec.url.end()));
                // Normalize syntax only. The adapter validates and captures local bytes without first
                // querying a potentially untrusted UNC/device source here.
                const auto path = std::filesystem::absolute(source).lexically_normal().generic_u8string();
                spec.url = "torrent:" + std::string(path.begin(), path.end());
            }
            spec.destination =
                std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[2])));
            int next = 4;
            if (next < argc && !std::string(argv[next]).starts_with("--")) {
                const auto count = integerValue(argv[next++]);
                if (count < 1 || count > 8)
                    throw std::invalid_argument("Connections must be between 1 and 8.");
                spec.connections = static_cast<int>(count);
                for (auto* value : {&spec.expectedSha256, &spec.proxy, &spec.cookie, &spec.authorization}) {
                    if (next >= argc || std::string(argv[next]).starts_with("--"))
                        break;
                    *value = argv[next++];
                }
            }
            if (readOptions(spec, argc, argv, next)) {
                inspectMedia(service, std::move(spec));
                return 0;
            }
            id = service.add(spec).get();
        }
        std::cout << "TASK_ID " << id << std::endl;
        for (int i = 0; i < 1800; ++i) {
            if (const auto message = service.lastError(); !message.empty()) {
                std::cerr << message << '\n';
                return 1;
            }
            for (const auto& task : service.snapshot())
                if (task.id == id) {
                    if (task.state == maskpilot::TaskState::AwaitingSelection) {
                        std::cout
                            << "Select files with --resume TASK_ID DATA_DIRECTORY --select-files INDEX,...\n";
                        if (task.torrent.files)
                            for (const auto& file : *task.torrent.files)
                                if (!file.padding)
                                    std::cout << file.index << '\t' << file.size << '\t' << file.path << '\n';
                        return 4;
                    }
                    if (task.state == maskpilot::TaskState::Completed) {
                        std::cout << "Completed " << task.downloaded << " bytes\n";
                        return 0;
                    }
                    if (task.state == maskpilot::TaskState::PolicyWaiting) {
                        std::cout << "Waiting for queue policy or daily native payload allowance.\n";
                        return 4;
                    }
                    if (task.state == maskpilot::TaskState::Failed ||
                        task.state == maskpilot::TaskState::NeedsAttention) {
                        std::cerr << task.error << '\n';
                        return 1;
                    }
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        service.pause(id);
        std::cerr << "Verification timeout\n";
        return 3;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wideArguments) {
    std::vector<std::string> arguments;
    for (int n = 0; n < argc; ++n) {
        const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[n], -1, nullptr,
                                               0, nullptr, nullptr);
        if (count <= 0)
            return 2;
        std::string value(static_cast<size_t>(count), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[n], -1, value.data(), count,
                                 nullptr, nullptr))
            return 2;
        value.pop_back();
        arguments.push_back(std::move(value));
    }
    std::vector<char*> pointers;
    for (auto& argument : arguments)
        pointers.push_back(argument.data());
    return runClient(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return runClient(argc, argv); }
#endif
