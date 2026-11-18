#include "media_process.h"
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QUrl>
#include <array>
#include <chrono>
#include <maskpilot/domain/proxy.h>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace maskpilot::media_process {
struct ProcessTree::Impl {
#ifdef _WIN32
    struct Handle {
        HANDLE value = nullptr;
        ~Handle() {
            if (value)
                CloseHandle(value);
        }
    } job_;
    struct Attributes {
        std::vector<unsigned char> memory;
        LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
        ~Attributes() {
            if (value)
                DeleteProcThreadAttributeList(value);
        }
    } attributes_;
    STARTUPINFOEXW startup_{};
    std::array<HANDLE, 3> inherited_{};
#endif
  public:
    Impl() {
#ifdef _WIN32
        job_.value = CreateJobObjectW(nullptr, nullptr);
        if (!job_.value)
            throw std::runtime_error("Cannot create media process group.");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("Cannot isolate media process group.");
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        attributes_.memory.resize(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_.memory.data());
        if (!InitializeProcThreadAttributeList(list, 2, 0, &size))
            throw std::runtime_error("Cannot initialize media launch policy.");
        attributes_.value = list;
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_.value, sizeof(HANDLE),
                                       nullptr, nullptr))
            throw std::runtime_error("Cannot bind media process group.");
#endif
    }
    void prepare(QProcess& process) {
#ifdef _WIN32
        process.setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments* args) {
            startup_.StartupInfo = *reinterpret_cast<STARTUPINFOW*>(args->startupInfo);
            startup_.StartupInfo.cb = sizeof(startup_);
            startup_.lpAttributeList = attributes_.value;
            inherited_ = {startup_.StartupInfo.hStdInput, startup_.StartupInfo.hStdOutput,
                          startup_.StartupInfo.hStdError};
            if (!UpdateProcThreadAttribute(attributes_.value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           inherited_.data(), sizeof(HANDLE) * inherited_.size(), nullptr,
                                           nullptr)) {
                args->applicationName = L"";
                return;
            }
            args->startupInfo = reinterpret_cast<Q_STARTUPINFO*>(&startup_);
            args->flags |= EXTENDED_STARTUPINFO_PRESENT;
            args->inheritHandles = true;
        });
#else
        (void)process;
#endif
    }
    void stop(QProcess& process) {
#ifdef _WIN32
        TerminateJobObject(job_.value, 130);
#else
        process.kill();
#endif
        process.waitForFinished(5000);
    }
};

ProcessTree::ProcessTree() : impl_(std::make_unique<Impl>()) {}
ProcessTree::~ProcessTree() = default;
void ProcessTree::prepare(QProcess& process) { impl_->prepare(process); }
void ProcessTree::stop(QProcess& process) { impl_->stop(process); }
QString pathString(const std::filesystem::path& path) { return QString::fromStdWString(path.wstring()); }
QStringList commonArguments() {
    return {"--ignore-config",
            "--config-locations",
            "-",
            "--no-plugin-dirs",
            "--no-cache-dir",
            "--no-playlist",
            "--encoding",
            "utf-8",
            "--socket-timeout",
            "20",
            "--downloader",
            "native",
            "--hls-prefer-native",
            "--downloader-args",
            "ffmpeg_i:-protocol_whitelist pipe"};
}
void validateRequest(const DownloadSpec& spec) {
    auto copy = spec;
    if (copy.destination.empty())
        copy.destination = std::filesystem::temp_directory_path() / L"maskpilot-media-inspection.tmp";
    if (copy.kind != TransferKind::Media)
        throw std::invalid_argument("A media request is required.");
    if (auto error = validateSpec(copy); !error.empty())
        throw std::invalid_argument(error);
    if (auto error = validateOriginAuthentication(copy, true); !error.empty())
        throw std::invalid_argument(error);
    if (!copy.cookie.empty() || !copy.authorization.empty() || !copy.headers.empty())
        throw std::invalid_argument("Origin authentication is unavailable for media extraction.");
    if (auto error = validateProxy(copy, true); !error.empty())
        throw std::invalid_argument(error);
}
namespace {
std::filesystem::path ownedGuardDirectory() {
#ifdef _WIN32
    std::array<wchar_t, 32768> module{};
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size())
        throw std::runtime_error("Cannot locate the owned media guard.");
    const auto directory = std::filesystem::path(std::wstring(module.data(), length)).parent_path();
    std::vector<std::filesystem::path> candidates{directory / L"media_guard"};
    if (QFileInfo(pathString(directory)).fileName().compare("tests", Qt::CaseInsensitive) == 0)
        candidates.push_back(directory.parent_path() / L"media_guard");
    for (const auto& candidate : candidates) {
        const auto ffmpeg = QFileInfo(pathString(candidate / L"ffmpeg.exe"));
        const auto ffprobe = QFileInfo(pathString(candidate / L"ffprobe.exe"));
        if (ffmpeg.isFile() && ffmpeg.isExecutable() && ffprobe.isFile() && ffprobe.isExecutable()) {
            const auto canonical = QFileInfo(pathString(candidate)).canonicalFilePath();
            if (!canonical.isEmpty())
                return std::filesystem::path(canonical.toStdWString());
        }
    }
#endif
    throw std::runtime_error("Media safety components are missing. Repair the application installation.");
}
QByteArray quoted(const QByteArray& value) {
    for (const auto ch : value)
        if (static_cast<unsigned char>(ch) < 32 || ch == 127)
            throw std::invalid_argument("Media request contains invalid control characters.");
    auto out = value;
    out.replace("'", "'\\''");
    return "'" + out + "'";
}
} // namespace
QByteArray privateInput(const DownloadSpec& spec) {
    validateRequest(spec);
    const auto endpoint = parseProxyEndpoint(spec.proxy);
    QByteArray proxy;
    if (endpoint.type != ProxyType::Direct) {
        proxy = endpoint.type == ProxyType::Socks5
                    ? "socks5h://"
                    : (endpoint.type == ProxyType::Https ? "https://" : "http://");
        if (!spec.proxyUsername.empty() || !spec.proxyPassword.empty())
            proxy += QUrl::toPercentEncoding(QString::fromUtf8(spec.proxyUsername)) + ":" +
                     QUrl::toPercentEncoding(QString::fromUtf8(spec.proxyPassword)) + "@";
        const auto host = QByteArray::fromStdString(endpoint.host);
        proxy += (host.contains(':') ? "[" + host + "]" : host) + ":" + QByteArray::number(endpoint.port);
    }
    QByteArray config = "--proxy " + quoted(proxy) + "\n";
    if (!spec.userAgent.empty())
        config += "--user-agent " + quoted(QByteArray::fromStdString(spec.userAgent)) + "\n";
    if (!spec.referrer.empty()) {
        auto referrer = QUrl(QString::fromUtf8(spec.referrer));
        referrer.setPath("/");
        referrer.setQuery(QString());
        referrer.setFragment(QString());
        referrer.setUserInfo(QString());
        config += "--referer " + quoted(referrer.toEncoded()) + "\n";
    }
    config += quoted(QByteArray::fromStdString(spec.url)) + "\n";
    if (config.size() > 128 * 1024)
        throw std::invalid_argument("Media private request exceeds its size limit.");
    return config;
}
bool launch(QProcess& process, ProcessTree& tree, const DownloadSpec& spec, const QStringList& arguments,
            const std::filesystem::path& work, const std::function<bool()>& stopped) {
    const auto input = privateInput(spec);
    if (stopped())
        return false;
    if (!QFileInfo(pathString(spec.mediaDownloader)).isExecutable())
        throw std::runtime_error("Media parser is unavailable. Select its executable in media settings.");
    const auto guard = ownedGuardDirectory();
    const QFileInfo selected(pathString(spec.mediaMuxer));
    const auto realFfmpeg = selected.canonicalFilePath();
    if (!selected.isFile() || !selected.isExecutable() || realFfmpeg.isEmpty() ||
        realFfmpeg.compare(QFileInfo(pathString(guard / L"ffmpeg.exe")).canonicalFilePath(),
                           Qt::CaseInsensitive) == 0 ||
        realFfmpeg.compare(QFileInfo(pathString(guard / L"ffprobe.exe")).canonicalFilePath(),
                           Qt::CaseInsensitive) == 0)
        throw std::runtime_error("Select a real media processor executable in media settings.");
    const QFileInfo workspace(pathString(work));
    const auto canonicalWork = workspace.canonicalFilePath();
    if (!workspace.isDir() || canonicalWork.isEmpty())
        throw std::runtime_error("Cannot establish the owned media workspace.");
    const QFileInfo probe(QDir(selected.absolutePath()).filePath("ffprobe.exe"));
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& name : environment.keys()) {
        const auto lower = name.toLower();
        if (lower == "http_proxy" || lower == "https_proxy" || lower == "ftp_proxy" || lower == "all_proxy" ||
            lower == "no_proxy" || lower == "ffreport" || lower.startsWith("maskpilot_media_"))
            environment.remove(name);
    }
    environment.insert("MASKPILOT_MEDIA_REAL_FFMPEG", QDir::toNativeSeparators(realFfmpeg));
    if (probe.isFile() && probe.isExecutable() && !probe.canonicalFilePath().isEmpty())
        environment.insert("MASKPILOT_MEDIA_REAL_FFPROBE",
                           QDir::toNativeSeparators(probe.canonicalFilePath()));
    environment.insert("MASKPILOT_MEDIA_WORK_DIR", QDir::toNativeSeparators(canonicalWork));
    process.setProcessEnvironment(environment);
    process.setProgram(pathString(spec.mediaDownloader));
    auto guardedArguments = arguments;
    guardedArguments << "--ffmpeg-location" << pathString(guard);
    process.setArguments(guardedArguments);
    process.setWorkingDirectory(pathString(work));
    process.setProcessChannelMode(QProcess::SeparateChannels);
    tree.prepare(process);
    process.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (process.state() == QProcess::Starting) {
        if (stopped()) {
            tree.stop(process);
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            tree.stop(process);
            throw std::runtime_error("Media parser did not start before its deadline.");
        }
        process.waitForStarted(50);
    }
    if (process.state() == QProcess::NotRunning && process.error() == QProcess::FailedToStart)
        throw std::runtime_error("Unable to start the media parser. Check its configured path.");
    if (process.write(input) != input.size()) {
        tree.stop(process);
        throw std::runtime_error("Cannot send the private media request.");
    }
    process.closeWriteChannel();
    return true;
}
} // namespace maskpilot::media_process
