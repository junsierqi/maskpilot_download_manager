#include "media_process.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
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
#include <thread>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace maskpilot {
namespace {
using media_process::pathString;
std::int64_t progressNumber(const QString& text, std::int64_t fallback = 0) {
    bool valid = false;
    auto value = text.toDouble(&valid);
    if (!valid || !std::isfinite(value) || value < 0 || value > 1e17)
        return fallback;
    return static_cast<std::int64_t>(value);
}
using media_process::ProcessTree;
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
        auto authenticatedSpec = task.spec;
        authenticatedSpec.originAuthenticationRequired =
            task.originAuthenticationRequired || requiresOriginAuthentication(task.spec);
        if (const auto error = validateOriginAuthentication(authenticatedSpec, true); !error.empty()) {
            finish(TransferOutcome::Attention, error);
            return;
        }
        try {
            media_process::validateRequest(authenticatedSpec);
        } catch (const std::exception& e) {
            finish(TransferOutcome::Attention, e.what());
            return;
        }
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
        if (task.spec.mediaSelection.mode != MediaSelectionMode::Automatic) {
            std::stop_source inspectionStop;
            std::jthread cancellation([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    if (control.pause || control.cancel) {
                        inspectionStop.request_stop();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            });
            try {
                auto inventory = makeMediaInspector()->inspect(authenticatedSpec, inspectionStop.get_token());
                if (auto error =
                        validateMediaChoice(task.spec.mediaSelection, inventory, task.spec.audioOnly);
                    !error.empty())
                    throw std::invalid_argument(error);
            } catch (const std::exception& e) {
                if (!stopped())
                    finish(TransferOutcome::Attention, e.what());
                return;
            }
            if (stopped())
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
            try {
                if (!media_process::launch(process, tree, authenticatedSpec,
                                           mediaArguments(task, media, limit), media,
                                           [&] { return control.pause.load() || control.cancel.load(); })) {
                    stopped();
                    return;
                }
            } catch (const std::exception& e) {
                if (!stopped())
                    finish(TransferOutcome::Attention, e.what());
                return;
            }
            event.phase = TaskState::Downloading;
            callback(event);
            QByteArray buffer, diagnosticTail;
            bool reconfigure = false;
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
                finish(
                    TransferOutcome::Attention,
                    "Media download failed. Check the source, proxy credentials and supported components.");
                return;
            }
            break;
        }
        QFileInfo result(resultPath);
        const auto expectedName =
            "media." + QString::fromStdString(mediaContainerExtension(
                           task.spec.mediaSelection.container,
                           mediaAudioOnly(task.spec.mediaSelection, task.spec.audioOnly)));
        if (!result.exists() || !result.isFile() || result.fileName() != expectedName ||
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
    auto args = media_process::commonArguments();
    args << "--no-continue" << "--match-filter" << "!is_live" << "--newline" << "--progress"
         << "--windows-filenames" << "--no-mtime" << "--retries" << "3" << "--fragment-retries" << "3"
         << "--progress-template"
         << "download:MASKPILOT_PROGRESS:%(info.format_id)s|%(progress.downloaded_bytes)s|%(progress.total_"
            "bytes,progress.total_bytes_estimate)s|%(progress.speed)s"
         << "--progress-template" << "postprocess:MASKPILOT_POSTPROCESS" << "--print"
         << "after_move:MASKPILOT_FILE:%(filepath)s";
    const auto node = QStandardPaths::findExecutable("node");
    if (!node.isEmpty())
        args << "--js-runtimes" << ("node:" + node);
    const auto& selection = task.spec.mediaSelection;
    const bool audio = mediaAudioOnly(selection, task.spec.audioOnly);
    const auto container = QString::fromStdString(mediaContainerExtension(selection.container, audio));
    auto format = selection.mode == MediaSelectionMode::Automatic
                      ? (audio ? std::string("bestaudio/best") : task.spec.mediaFormat)
                      : selectedMediaFormat(selection);
    args << "--format" << QString::fromStdString(format);
    if (audio)
        args << "--extract-audio" << "--audio-format" << container;
    else
        args << "--merge-output-format" << container << "--remux-video" << container;
    if (speedLimit > 0)
        args << "--limit-rate" << QString::number(speedLimit);
    args << "--output" << QDir(pathString(work)).filePath("media.%(ext)s");
    return args;
}
std::unique_ptr<ITransferTransport> makeMediaTransfer() { return std::make_unique<MediaTransfer>(); }
} // namespace maskpilot
