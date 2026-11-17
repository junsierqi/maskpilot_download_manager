#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/media_tools.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace maskpilot {
namespace {
QString pathString(const std::filesystem::path& path) { return QString::fromStdWString(path.wstring()); }
std::int64_t progressNumber(const QString& text, std::int64_t fallback = 0) {
    bool valid = false;
    auto value = text.toDouble(&valid);
    if (!valid || !std::isfinite(value) || value < 0 || value > 1e17)
        return fallback;
    return static_cast<std::int64_t>(value);
}
class ProcessTree {
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
    ProcessTree() {
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
    ProcessTree(const ProcessTree&) = delete;
    ProcessTree& operator=(const ProcessTree&) = delete;
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
void copyStage(const std::filesystem::path& source, const std::filesystem::path& stage,
               TransferControl& control) {
#ifdef _WIN32
    FILE* input = nullptr;
    FILE* output = nullptr;
    _wfopen_s(&input, source.c_str(), L"rb");
    _wfopen_s(&output, stage.c_str(), L"wb");
    if (!input || !output) {
        if (input)
            fclose(input);
        if (output)
            fclose(output);
        throw std::runtime_error("Cannot open media output file.");
    }
    try {
        std::array<char, 128 * 1024> data{};
        size_t count;
        while ((count = fread(data.data(), 1, data.size(), input)) > 0) {
            if (control.pause || control.cancel)
                throw std::runtime_error("Media operation cancelled.");
            if (fwrite(data.data(), 1, count, output) != count)
                throw std::runtime_error("Cannot write media output. Check disk space.");
        }
        if (ferror(input) || fflush(output) != 0 || _commit(_fileno(output)) != 0)
            throw std::runtime_error("Cannot synchronize media output.");
        fclose(input);
        input = nullptr;
        fclose(output);
        output = nullptr;
    } catch (...) {
        if (input)
            fclose(input);
        if (output)
            fclose(output);
        throw;
    }
#else
    (void)source;
    (void)stage;
    (void)control;
    throw std::runtime_error("This media adapter currently supports Windows.");
#endif
}
class MediaTransfer final : public ITransferTransport {
  public:
    void run(const Task& task, const std::filesystem::path& work, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        if (auto error =
                task.spec.kind == TransferKind::Media ? validateSpec(task.spec) : "A media task is required.";
            !error.empty()) {
            TransferEvent failed;
            failed.outcome = TransferOutcome::Attention;
            failed.error = error;
            callback(failed);
            return;
        }
        TransferEvent event;
        event.phase = TaskState::Probing;
        auto finish = [&](TransferOutcome outcome, const std::string& error = {}) {
            event.outcome = outcome;
            event.error = error;
            event.speed = 0;
            callback(event);
        };
        auto stopped = [&]() {
            if (control.cancel) {
                finish(TransferOutcome::Cancelled);
                return true;
            }
            if (control.pause) {
                finish(TransferOutcome::Paused);
                return true;
            }
            return false;
        };
        callback(event);
        if (!QFileInfo(pathString(task.spec.mediaDownloader)).isExecutable() ||
            !QFileInfo(pathString(task.spec.mediaMuxer)).isExecutable()) {
            finish(TransferOutcome::Attention,
                   "Media support components are missing. Open media settings to install or locate them.");
            return;
        }
        const auto media = work / L"media";
        event.resumable = false;
        QString resultPath;
        std::map<QString, std::pair<std::int64_t, std::int64_t>> streams;
        auto limit = control.speedLimit.load();
        for (;;) {
            if (stopped())
                return;
            std::error_code clearError;
            std::filesystem::remove_all(media, clearError);
            if (clearError)
                throw std::runtime_error("Cannot reset owned media cache.");
            std::filesystem::create_directories(media);
            streams.clear();
            resultPath.clear();
            event.downloaded = 0;
            event.total = -1;
            QProcess process;
            ProcessTree tree;
            process.setProgram(pathString(task.spec.mediaDownloader));
            process.setArguments(mediaArguments(task, media, limit));
            process.setWorkingDirectory(pathString(media));
            process.setProcessChannelMode(QProcess::SeparateChannels);
            tree.prepare(process);
            process.start();
            if (!process.waitForStarted(15000)) {
                finish(TransferOutcome::Attention,
                       "Unable to start the media parser. Check its configured path.");
                return;
            }
            process.closeWriteChannel();
            QByteArray buffer, diagnosticTail;
            bool reconfigure = false;
            event.phase = TaskState::Downloading;
            callback(event);
            const auto started = std::chrono::steady_clock::now();
            auto lastActivity = started;
            while (process.state() != QProcess::NotRunning) {
                process.waitForReadyRead(100);
                const auto output = process.readAllStandardOutput();
                const auto diagnostic = process.readAllStandardError();
                diagnosticTail = (diagnosticTail + diagnostic).right(16384);
                if (!output.isEmpty() || !diagnostic.isEmpty())
                    lastActivity = std::chrono::steady_clock::now();
                buffer += output;
                if (std::chrono::steady_clock::now() - lastActivity > std::chrono::minutes(10) ||
                    std::chrono::steady_clock::now() - started > std::chrono::hours(48)) {
                    tree.stop(process);
                    finish(TransferOutcome::Attention, "Media helper exceeded its idle or total deadline.");
                    return;
                }
                if (buffer.size() > 1024 * 1024) {
                    tree.stop(process);
                    finish(TransferOutcome::Attention, "Media helper output exceeded the protocol limit.");
                    return;
                }
                int end;
                while ((end = buffer.indexOf('\n')) >= 0) {
                    const auto line = QString::fromUtf8(buffer.left(end)).trimmed();
                    buffer.remove(0, end + 1);
                    if (line.startsWith("MASKPILOT_FILE:"))
                        resultPath = line.mid(15);
                    else if (line.startsWith("MASKPILOT_POSTPROCESS")) {
                        event.phase = TaskState::Verifying;
                        event.speed = 0;
                        callback(event);
                    } else if (line.startsWith("MASKPILOT_PROGRESS:")) {
                        auto values = line.mid(19).split('|');
                        if (values.size() != 4)
                            continue;
                        if (streams.size() >= 64 && !streams.contains(values[0])) {
                            tree.stop(process);
                            finish(TransferOutcome::Attention, "Media progress exceeded the stream limit.");
                            return;
                        }
                        auto& stream = streams[values[0]];
                        stream.first = progressNumber(values[1]);
                        stream.second = progressNumber(values[2], -1);
                        event.downloaded = 0;
                        event.total = 0;
                        for (const auto& [id, value] : streams) {
                            (void)id;
                            event.downloaded += value.first;
                            if (value.second > 0)
                                event.total += value.second;
                        }
                        if (event.total == 0)
                            event.total = -1;
                        event.speed = progressNumber(values[3]);
                        event.phase = TaskState::Downloading;
                        callback(event);
                    }
                }
                if (control.pause || control.cancel) {
                    tree.stop(process);
                    stopped();
                    return;
                }
                if (event.phase == TaskState::Downloading && control.speedLimit.load() != limit &&
                    std::chrono::steady_clock::now() - started > std::chrono::seconds(1)) {
                    limit = control.speedLimit.load();
                    tree.stop(process);
                    reconfigure = true;
                    break;
                }
            }
            buffer += process.readAllStandardOutput();
            diagnosticTail = (diagnosticTail + process.readAllStandardError()).right(16384);
            for (const auto& line : QString::fromUtf8(buffer).split('\n'))
                if (line.trimmed().startsWith("MASKPILOT_FILE:"))
                    resultPath = line.trimmed().mid(15);
            if (reconfigure)
                continue;
            if (stopped())
                return;
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
                auto detail = QString::fromUtf8(diagnosticTail).trimmed().split('\n').last().trimmed();
                detail.replace(QRegularExpression("https?://[^\\s]+"), "[address]");
                finish(TransferOutcome::Attention,
                       "Media download failed: " + detail.left(700).toStdString());
                return;
            }
            break;
        }
        QFileInfo result(resultPath);
        if (!result.exists() || !result.isFile() ||
            result.fileName() != (task.spec.audioOnly ? "media.m4a" : "media.mkv") ||
            result.canonicalPath() != QFileInfo(pathString(media)).canonicalFilePath()) {
            finish(TransferOutcome::Attention, "Media helper did not return a valid output file.");
            return;
        }
        try {
            if (stopped())
                return;
            event.phase = TaskState::Verifying;
            event.downloaded = result.size();
            event.total = result.size();
            callback(event);
            std::filesystem::create_directories(task.spec.destination.parent_path());
            auto stage = task.spec.destination;
            stage += L".maskpilot-" + std::filesystem::path(task.id).wstring() + L".tmp";
            copyStage(std::filesystem::path(result.canonicalFilePath().toStdWString()), stage, control);
            if (!task.spec.expectedSha256.empty()) {
                auto expected = task.spec.expectedSha256;
                std::transform(expected.begin(), expected.end(), expected.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (sha256File(stage, &control) != expected) {
                    finish(TransferOutcome::Attention,
                           "SHA-256 verification failed. Media output was not published.");
                    return;
                }
            }
            if (stopped())
                return;
            event.phase = TaskState::Finalizing;
            callback(event);
            if (stopped())
                return;
#ifdef _WIN32
            if (!MoveFileExW(stage.c_str(), task.spec.destination.c_str(), MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot publish media output. Existing files were preserved.");
#endif
            finish(TransferOutcome::Complete);
        } catch (const std::exception& e) {
            if (!stopped())
                finish(TransferOutcome::Attention, e.what());
        }
    }
};
} // namespace
QStringList mediaArguments(const Task& task, const std::filesystem::path& work, std::int64_t speedLimit) {
    QStringList args{"--no-continue",
                     "--match-filter",
                     "!is_live",
                     "--ignore-config",
                     "--no-plugin-dirs",
                     "--no-playlist",
                     "--no-cache-dir",
                     "--newline",
                     "--progress",
                     "--encoding",
                     "utf-8",
                     "--windows-filenames",
                     "--no-mtime",
                     "--retries",
                     "3",
                     "--fragment-retries",
                     "3",
                     "--socket-timeout",
                     "20",
                     "--progress-template",
                     "download:MASKPILOT_PROGRESS:%(info.format_id)s|%(progress.downloaded_bytes)s|%("
                     "progress.total_bytes,progress.total_bytes_estimate)s|%(progress.speed)s",
                     "--progress-template",
                     "postprocess:MASKPILOT_POSTPROCESS",
                     "--print",
                     "after_move:MASKPILOT_FILE:%(filepath)s"};
    args << "--ffmpeg-location" << pathString(task.spec.mediaMuxer);
    const auto node = QStandardPaths::findExecutable("node");
    if (!node.isEmpty())
        args << "--js-runtimes" << ("node:" + node);
    if (task.spec.audioOnly)
        args << "--extract-audio" << "--audio-format" << "m4a" << "--format" << "bestaudio/best";
    else
        args << "--format" << QString::fromStdString(task.spec.mediaFormat) << "--merge-output-format"
             << "mkv" << "--remux-video" << "mkv";
    if (speedLimit > 0)
        args << "--limit-rate" << QString::number(speedLimit);
    if (!task.spec.proxy.empty())
        args << "--proxy" << QString::fromStdString(task.spec.proxy);
    if (!task.spec.referrer.empty()) {
        auto referrer = QUrl(QString::fromStdString(task.spec.referrer));
        if ((referrer.scheme() == "http" || referrer.scheme() == "https") && !referrer.host().isEmpty()) {
            referrer.setPath("/");
            referrer.setQuery(QString());
            referrer.setFragment(QString());
            referrer.setUserInfo(QString());
            args << "--referer" << referrer.toString();
        }
    }
    if (!task.spec.userAgent.empty())
        args << "--user-agent" << QString::fromStdString(task.spec.userAgent);
    auto output = QDir(pathString(work)).filePath("media.%(ext)s");
    args << "--output" << output << "--" << QString::fromStdString(task.spec.url);
    return args;
}
std::unique_ptr<ITransferTransport> makeMediaTransfer() { return std::make_unique<MediaTransfer>(); }
} // namespace maskpilot
