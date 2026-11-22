#include "site_commands.h"
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
#include <vector>
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
    if (name.empty() || name.size() > 128 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }))
        throw std::invalid_argument("Invalid credential environment variable name.");
#ifdef _WIN32
    wchar_t* raw = nullptr;
    std::size_t size = 0;
    const auto result = _wdupenv_s(&raw, &size, QString::fromStdString(name).toStdWString().c_str());
    const std::unique_ptr<wchar_t, decltype(&std::free)> secret(raw, std::free);
    if (result != 0 || !raw)
        throw std::invalid_argument("Request credential environment variable is unavailable.");
    return QString::fromWCharArray(raw).toUtf8().toStdString();
#else
    const auto* secret = std::getenv(name.c_str());
    if (!secret)
        throw std::invalid_argument("Request credential environment variable is unavailable.");
    return secret;
#endif
}
bool readOptions(maskpilot::DownloadSpec& spec, int argc, char** argv, int start) {
    bool inspection = false;
    std::vector<std::string> privateEnvironmentInputs;
    for (int n = start; n < argc; ++n) {
        const std::string option = argv[n];
        if (option == "--live-hls" || option == "--live-flv") {
            spec.kind = maskpilot::TransferKind::Live;
            spec.live.format = option == "--live-hls" ? maskpilot::LiveFormat::HlsTs : maskpilot::LiveFormat::Flv;
        } else if (option == "--media")
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
        else if (option == "--post")
            spec.httpRequest.method = maskpilot::HttpMethod::Post;
        else if (option == "--confirm-empty-post") {
            spec.httpRequest.method = maskpilot::HttpMethod::Post;
            spec.httpRequest.body.clear();
            spec.httpRequest.bodyProvided = true;
        } else if (option == "--same-origin-redirects")
            spec.sameOriginRedirectOnly = true;
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
            if (option.ends_with("-env"))
                privateEnvironmentInputs.push_back(value);
            if (option.starts_with("--media-"))
                spec.kind = maskpilot::TransferKind::Media;
            if (option == "--live-duration") {
                const auto duration = integerValue(value);
                if (duration < 1 || duration > 172800)
                    throw std::invalid_argument("Live duration must be 1..172800 seconds.");
                spec.live.durationSeconds = static_cast<int>(duration);
            } else if (option == "--source-cookie-env" || option == "--source-authorization-env") {
                (option == "--source-cookie-env" ? spec.cookie : spec.authorization) = credentialFromEnvironment(value);
            } else if (option == "--media-origins-env") {
                if (!value.starts_with("MASKPILOT_MEDIA_"))
                    throw std::invalid_argument(
                        "Media origin input must use a MASKPILOT_MEDIA_ environment variable.");
                const auto raw = credentialFromEnvironment(value);
                if (raw.size() > 128 * 1024)
                    throw std::invalid_argument("Media origin input exceeds its limit.");
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(raw), &error);
                if (error.error != QJsonParseError::NoError || !document.isArray() ||
                    document.array().isEmpty() || document.array().size() > 16)
                    throw std::invalid_argument("Expected 1..16 media origin rules.");
                spec.mediaOrigins.clear();
                for (const auto& entry : document.array()) {
                    if (!entry.isObject())
                        throw std::invalid_argument("Invalid media origin rule.");
                    const auto object = entry.toObject();
                    for (auto field = object.begin(); field != object.end(); ++field)
                        if (field.key() != "origin" && field.key() != "cookie" &&
                            field.key() != "authorization" && field.key() != "headers" &&
                            field.key() != "authenticationRequired")
                            throw std::invalid_argument("Unknown media origin rule field.");
                    const auto text = [&](const char* name) {
                        if (object.contains(name) && !object[name].isString())
                            throw std::invalid_argument("Media origin field must be a string.");
                        return object[name].toString().toStdString();
                    };
                    maskpilot::MediaOriginAccess rule;
                    rule.origin = text("origin");
                    rule.cookie = text("cookie");
                    rule.authorization = text("authorization");
                    if (object.contains("authenticationRequired") &&
                        !object["authenticationRequired"].isBool())
                        throw std::invalid_argument("Invalid media origin authentication requirement.");
                    rule.authenticationRequired = object["authenticationRequired"].toBool();
                    if (object.contains("headers") && !object["headers"].isObject())
                        throw std::invalid_argument("Media origin headers must be an object.");
                    const auto headers = object["headers"].toObject();
                    for (auto field = headers.begin(); field != headers.end(); ++field) {
                        if (!field.value().isString())
                            throw std::invalid_argument("Media origin header must be a string.");
                        rule.headers.emplace_back(field.key().toStdString(),
                                                  field.value().toString().toStdString());
                    }
                    spec.mediaOrigins.push_back(std::move(rule));
                }
                spec.mediaOriginsConfirmed = true;
            } else if (option == "--live-max-bytes") {
                spec.live.maxBytes = integerValue(value);
            } else if (option == "--media-parser" || option == "--media-muxer") {
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
            } else if (option == "--sftp-host-key") {
                spec.sftp.hostKeySha256 = value;
            } else if (option == "--sftp-auth") {
                if (value == "password")
                    spec.sftp.authentication = maskpilot::SftpAuthentication::Password;
                else if (value == "key")
                    spec.sftp.authentication = maskpilot::SftpAuthentication::PrivateKey;
                else
                    throw std::invalid_argument("SFTP authentication must be password or key.");
            } else if (option == "--sftp-private-key") {
                spec.sftp.authentication = maskpilot::SftpAuthentication::PrivateKey;
                spec.sftp.privateKeyFile = std::filesystem::path(std::u8string(value.begin(), value.end()));
            } else if (option == "--sftp-user-env") {
                spec.sftp.username = credentialFromEnvironment(value);
            } else if (option == "--sftp-password-env") {
                spec.sftp.password = credentialFromEnvironment(value);
            } else if (option == "--sftp-key-passphrase-env") {
                spec.sftp.authentication = maskpilot::SftpAuthentication::PrivateKey;
                spec.sftp.keyPassphrase = credentialFromEnvironment(value);
            } else if (option == "--post-body-env") {
                if (value.empty() || value.size() > 128 ||
                    !std::all_of(value.begin(), value.end(), [](unsigned char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                               c == '_';
                    }))
                    throw std::invalid_argument("Invalid POST body environment variable name.");
                spec.httpRequest.method = maskpilot::HttpMethod::Post;
                spec.httpRequest.body = credentialFromEnvironment(value);
                spec.httpRequest.bodyProvided = true;
            } else if (option == "--post-content-type") {
                spec.httpRequest.contentType = value;
            } else if (option == "--max-stored-bytes") {
                spec.maxStoredBytes = integerValue(value);
                if (spec.maxStoredBytes < 0 || spec.maxStoredBytes > 9007199254740991LL)
                    throw std::invalid_argument("Stored body bound must be between 0 and 9007199254740991.");
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
    // Values have been captured in the request. Do not propagate consumed inputs to helper processes.
    for (const auto& name : privateEnvironmentInputs)
        if (!qunsetenv(name.c_str()))
            throw std::runtime_error("Cannot clear a private request environment input.");
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
    maskpilot::CompletionScanOptions scanOptions;
    bool customScanArguments = false, scanTimeoutSet = false, waiveScan = false;
    // Runtime options precede the command, so strings used as task option values are never reinterpreted.
    try {
        while (argc > 1) {
            const std::string option = argv[1];
            int consumed = 1;
            if (option == "--bt-no-discovery")
                torrentRuntime.enableDiscovery = false;
            else if (option == "--waive-scan")
                waiveScan = true;
            else if (option == "--scan-program" || option == "--scan-arg" || option == "--scan-timeout") {
                if (argc < 3)
                    throw std::invalid_argument("Missing scanner option value.");
                consumed = 2;
                const std::string value = argv[2];
                if (option == "--scan-program") {
                    scanOptions.enabled = true;
                    scanOptions.program = std::filesystem::path(std::u8string(value.begin(), value.end()));
                } else if (option == "--scan-arg") {
                    if (!customScanArguments)
                        scanOptions.arguments.clear();
                    customScanArguments = true;
                    scanOptions.arguments.push_back(value);
                } else {
                    const auto seconds = integerValue(value);
                    if (seconds < 1 || seconds > 3600)
                        throw std::invalid_argument("Scan timeout must be 1..3600 seconds.");
                    scanOptions.timeoutSeconds = static_cast<int>(seconds);
                    scanTimeoutSet = true;
                }
            } else
                break;
            argc -= consumed;
            argv += consumed;
        }
        if ((customScanArguments || scanTimeoutSet) && !scanOptions.enabled)
            throw std::invalid_argument("Specify --scan-program with scanner arguments or timeout.");
        if (waiveScan && (argc < 2 || std::string(argv[1]) != "--resume"))
            throw std::invalid_argument("--waive-scan applies only to an explicit --resume task.");
        if (argc > 1 && std::string(argv[1]) == "--project")
            return maskpilot::runSiteProjectCommand(argc, argv,
                                                    {scanOptions, maskpilot::makeCompletionScanner});
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
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
        std::cout << "HTTP: --max-stored-bytes N (0 unlimited) --same-origin-redirects\n";
        std::cout
            << "SFTP options: --sftp-host-key SHA256:BASE64 --sftp-user-env NAME\n"
               "  --sftp-password-env NAME or --sftp-private-key FILE [--sftp-key-passphrase-env NAME]\n"
               "  Password/key secrets are never saved; --sha256 DIGEST permits verified resume.\n"
            << "POST options: --post --post-body-env NAME --post-content-type MIME\n"
               "  --confirm-empty-post confirms an empty body. Each run needs new confirmation.\n"
            << "Trusted prefix options before URL/--resume/--project: --scan-program EXE\n"
               "  --scan-arg VALUE (repeat, including one literal {file}) --scan-timeout SECONDS\n"
               "  --waive-scan --resume TASK_ID DATA explicitly waives that stopped task's requirement.\n";
        std::cout << "Live: --live-hls|--live-flv --live-duration SECONDS --live-max-bytes BYTES\n"
                     "      --source-cookie-env NAME --source-authorization-env NAME (memory only)\n"
                     "Media: --media --inspect-media --media-parser EXE --media-muxer EXE\n"
                     "       --media-mode auto|muxed|video-audio|audio|video --media-format-id ID\n"
                     "       --media-audio-id ID --media-container auto|mkv|mp4|webm|m4a|mp3|opus|flac\n"
                     "       --media-origins-env MASKPILOT_MEDIA_NAME (approved JSON rules, memory only)\n";
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
                                         maskpilot::InitialQueueLimits{}, std::move(store.traffic)},
            maskpilot::CompletionScanDependencies{scanOptions, maskpilot::makeCompletionScanner});
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
                spec.httpRequest.body.clear();
                spec.httpRequest.bodyProvided = false;
                start = 5;
                spec.cookie.clear();
                spec.authorization.clear();
                spec.headers.clear();
                spec.referrer.clear();
                spec.ftpUsername.clear();
                spec.ftpPassword.clear();
                spec.sftp.username.clear();
                spec.sftp.password.clear();
                spec.sftp.keyPassphrase.clear();
                if (!maskpilot::isSftpUrl(spec.url))
                    spec.sftp = {};
                spec.kind = maskpilot::sourceKind(spec);
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
            if (waiveScan)
                service.setCompletionScanRequired(id, false).get();
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
        bool liveTask = false;
        for (const auto& task : service.snapshot())
            if (task.id == id && task.spec.kind == maskpilot::TransferKind::Live)
                liveTask = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        // Live owns its bounded recording clock; allow large final copies and the scanner to finish.
        while (liveTask || std::chrono::steady_clock::now() < deadline) {
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
                        std::cout << "Waiting for queue policy or native payload allowance.\n";
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
