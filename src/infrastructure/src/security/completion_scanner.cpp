#include "../process/owned_process.h"
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <map>
#include <maskpilot/infrastructure/adapters.h>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace maskpilot {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
bool stopped(const TransferControl& control) { return control.pause || control.cancel; }
CompletionScanResult interrupted(const TransferControl& control) {
    return {control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused, ScanState::Pending, {}};
}
CompletionScanResult error(const char* text) { return {TransferOutcome::Attention, ScanState::Error, text}; }
void safePath(const fs::path& path) {
    if (!path.is_absolute() || path.lexically_normal() != path || path.native().size() > 32768 ||
        path.native().starts_with(fs::path("//").native()) ||
        path.native().starts_with(fs::path("\\\\").native()) ||
        std::any_of(path.native().begin(), path.native().end(), [](auto c) { return c < 32 || c == 127; }))
        throw std::invalid_argument("Choose a regular local scanner input.");
#ifdef _WIN32
    if (GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE)
        throw std::invalid_argument("Scanner paths must be on a local drive.");
#endif
    fs::path part;
    for (const auto& component : path) {
        part /= component;
        if (fs::is_symlink(fs::symlink_status(part)))
            throw std::invalid_argument("Scanner paths cannot traverse links.");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(part.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::invalid_argument("Scanner paths must exist without reparse points.");
#endif
    }
}
using Inventory = std::map<fs::path, std::pair<std::uintmax_t, std::string>>;
Inventory inventory(const fs::path& path, const TransferControl& control, Clock::time_point deadline) {
    safePath(path);
    Inventory result;
    const auto add = [&](const fs::path& file) {
        if (stopped(control) || Clock::now() >= deadline)
            throw std::runtime_error("Scan stopped.");
        safePath(file);
        if (!fs::is_regular_file(fs::symlink_status(file)) || result.size() >= maxTorrentFiles)
            throw std::runtime_error("Scanner input is unsupported or exceeds the file limit.");
        result.emplace(file.lexically_relative(path),
                       std::make_pair(fs::file_size(file),
                                      sha256File(file, &control, [&] { return Clock::now() >= deadline; })));
    };
    if (fs::is_regular_file(fs::symlink_status(path)))
        add(path);
    else if (fs::is_directory(fs::symlink_status(path))) {
        std::size_t entries = 0;
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (stopped(control) || Clock::now() >= deadline || ++entries > maxTorrentFiles * 16)
                throw std::runtime_error("Scanner directory inspection stopped or exceeded its limit.");
            safePath(entry.path());
            if (!entry.is_directory())
                add(entry.path());
        }
    } else
        throw std::runtime_error("Scanner input is not a regular file or directory.");
    return result;
}
class CompletionScanner final : public ICompletionScanner {
    CompletionScanOptions options_;

  public:
    explicit CompletionScanner(CompletionScanOptions options) : options_(std::move(options)) {
        if (const auto message = validateCompletionScanOptions(options_); !message.empty())
            throw std::invalid_argument(message);
        if (!options_.enabled)
            throw std::invalid_argument("Enable scanning before creating a scanner.");
        safePath(options_.program);
        if (!fs::is_regular_file(fs::symlink_status(options_.program)))
            throw std::invalid_argument("Choose a regular trusted scanner executable.");
#ifdef _WIN32
        auto extension = options_.program.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (extension != L".exe")
            throw std::invalid_argument("Choose a trusted .exe scanner; shell scripts are not supported.");
#endif
    }
    CompletionScanResult scan(const fs::path& path, const TransferControl& control) override {
        const auto deadline = Clock::now() + std::chrono::seconds(options_.timeoutSeconds);
        if (stopped(control))
            return interrupted(control);
        try {
            const auto before = inventory(path, control, deadline);
            safePath(options_.program);
            QProcess child;
            const auto system = QProcessEnvironment::systemEnvironment();
            QProcessEnvironment environment;
            for (const auto* name : {"SystemRoot", "WINDIR", "TEMP", "TMP", "USERPROFILE", "APPDATA",
                                     "LOCALAPPDATA", "ProgramData", "ProgramFiles", "ProgramFiles(x86)",
                                     "CommonProgramFiles", "CommonProgramFiles(x86)", "SystemDrive"})
                if (system.contains(QString::fromLatin1(name)))
                    environment.insert(QString::fromLatin1(name), system.value(QString::fromLatin1(name)));
            environment.insert("PATH", QString::fromStdWString(options_.program.parent_path().wstring()) +
                                           ";" + QDir(system.value("SystemRoot")).filePath("System32"));
            child.setProcessEnvironment(environment);
            child.setStandardOutputFile(QProcess::nullDevice());
            child.setStandardErrorFile(QProcess::nullDevice());
            process::OwnedProcessGroup group;
            group.prepare(child);
            child.setWorkingDirectory(QString::fromStdWString(options_.program.parent_path().wstring()));
            QStringList arguments;
            for (const auto& argument : options_.arguments)
                arguments.push_back(argument == "{file}" ? QString::fromStdWString(path.wstring())
                                                         : QString::fromUtf8(argument));
            child.start(QString::fromStdWString(options_.program.wstring()), arguments);
            while (child.state() == QProcess::Starting) {
                child.waitForStarted(50);
                if (stopped(control) || Clock::now() >= deadline) {
                    group.stop(child);
                    return stopped(control) ? interrupted(control)
                                            : error("Completion scan timed out. Output was not published.");
                }
            }
            if (child.state() == QProcess::NotRunning && child.error() == QProcess::FailedToStart)
                return error("Cannot start the configured completion scanner. Output was not published.");
            child.closeWriteChannel();
            while (child.state() != QProcess::NotRunning) {
                child.waitForFinished(50);
                if (stopped(control) || Clock::now() >= deadline) {
                    group.stop(child);
                    return stopped(control) ? interrupted(control)
                                            : error("Completion scan timed out. Output was not published.");
                }
            }
            if (stopped(control))
                return interrupted(control);
            if (child.exitStatus() != QProcess::NormalExit)
                return error("Completion scanner exited unexpectedly. Output was not published.");
            if (child.exitCode() != 0)
                return {TransferOutcome::Attention, ScanState::Rejected,
                        "Completion scanner did not approve this output. It was not published."};
            if (inventory(path, control, deadline) != before)
                return error("Scanner input changed during inspection. Output was not published.");
            return {};
        } catch (const std::exception&) {
            return stopped(control)           ? interrupted(control)
                   : Clock::now() >= deadline ? error("Completion scan timed out. Output was not published.")
                                              : error("Completion scan could not verify its input or "
                                                      "process. Output was not published.");
        }
    }
};
} // namespace
std::shared_ptr<ICompletionScanner> makeCompletionScanner(const CompletionScanOptions& options) {
    return std::make_shared<CompletionScanner>(options);
}
} // namespace maskpilot
