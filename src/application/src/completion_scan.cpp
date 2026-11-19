#include <algorithm>
#include <maskpilot/application/completion_scan.h>
#include <stdexcept>

namespace maskpilot {
std::string validateCompletionScanOptions(const CompletionScanOptions& options) {
    if (!options.enabled)
        return {}; // Inactive tool settings never prevent startup; persisted task requirements still block
                   // publication.
    if (options.timeoutSeconds < 1 || options.timeoutSeconds > 3600 || options.arguments.size() > 32)
        return "Use a scan timeout of 1..3600 seconds and at most 32 arguments.";
    std::size_t bytes = 0, placeholders = 0;
    for (const auto& argument : options.arguments) {
        bytes += argument.size();
        if (argument.size() > 4096 || bytes > 16384 ||
            std::any_of(argument.begin(), argument.end(), [](unsigned char c) { return c < 32 || c == 127; }))
            return "Scanner arguments must be bounded literal values without control characters.";
        if (argument == "{file}")
            ++placeholders;
        else if (argument.find("{file}") != std::string::npos)
            return "Use {file} as one complete scanner argument.";
    }
    if (placeholders != 1)
        return "Provide exactly one {file} argument for the verified file or directory.";
    const auto program = options.program.native();
    if (program.size() > 32768 ||
        std::any_of(program.begin(), program.end(), [](auto c) { return c < 32 || c == 127; }))
        return "Scanner program path is invalid.";
    if (options.enabled && (options.program.empty() || !options.program.is_absolute()))
        return "Choose an absolute trusted scanner executable path.";
    return {};
}
std::string validateCompletionScanRecord(const Task& task) {
    if (task.scanState < ScanState::NotRequested || task.scanState >= ScanState::Count || task.scanAt < 0 ||
        task.scanAt > 253402300799LL ||
        (!task.scanRequired && task.scanState != ScanState::NotRequested &&
         task.scanState != ScanState::Waived) ||
        (task.scanRequired && task.scanState == ScanState::Waived) ||
        (!task.scanRequired && task.scanState == ScanState::NotRequested && task.scanAt != 0) ||
        (task.state == TaskState::Completed && task.scanRequired && task.scanState != ScanState::Passed))
        return "Invalid persistent completion scan state.";
    return {};
}
namespace {
struct ScanOperation {
    TransferControl& control;
    std::shared_ptr<ICompletionScanner> scanner;
    std::function<void(TransferEvent)> observer;
    TransferEvent latest;
    ScanOperation(TransferControl& value, std::shared_ptr<ICompletionScanner> tool,
                  std::function<void(TransferEvent)> sink)
        : control(value), scanner(std::move(tool)), observer(std::move(sink)) {}
    void observe(TransferEvent event) {
        latest = event;
        observer(std::move(event));
    }
    CompletionScanResult scan(const std::filesystem::path& stage) {
        auto status = latest;
        status.outcome = TransferOutcome::Progress;
        status.phase = TaskState::Verifying;
        status.scanState = ScanState::Scanning;
        observe(status);
        CompletionScanResult result;
        try {
            result = scanner->scan(stage, control);
        } catch (...) {
            result = {TransferOutcome::Attention, ScanState::Error,
                      "Completion scanner failed. Output was not published."};
        }
        if (result.state <= ScanState::NotRequested || result.state >= ScanState::Waived ||
            (result.outcome != TransferOutcome::Complete && result.outcome != TransferOutcome::Attention &&
             result.outcome != TransferOutcome::Paused && result.outcome != TransferOutcome::Cancelled) ||
            (result.outcome == TransferOutcome::Complete && result.state != ScanState::Passed) ||
            (result.outcome == TransferOutcome::Attention && result.state != ScanState::Rejected &&
             result.state != ScanState::Error) ||
            ((result.outcome == TransferOutcome::Paused || result.outcome == TransferOutcome::Cancelled) &&
             result.state != ScanState::Pending))
            result = {TransferOutcome::Attention, ScanState::Error,
                      "Completion scanner returned an invalid result. Output was not published."};
        status = latest;
        status.outcome = TransferOutcome::Progress;
        status.scanState = result.state;
        observe(status);
        return result;
    }
};
} // namespace
std::function<void(TransferEvent)> attachCompletionScanner(TransferControl& control,
                                                           const CompletionScanDependencies& dependencies,
                                                           std::function<void(TransferEvent)> observer) {
    if (!dependencies.options.enabled || !dependencies.factory)
        throw std::runtime_error("Completion scanner is not configured.");
    auto scanner = dependencies.factory(dependencies.options);
    if (!scanner)
        throw std::runtime_error("Completion scanner is unavailable.");
    auto operation = std::make_shared<ScanOperation>(control, std::move(scanner), std::move(observer));
    // Operation holds a reference, not shared ownership, of control; there is no capture cycle.
    control.scanBeforePublish = [operation](const auto& stage) { return operation->scan(stage); };
    return [operation](TransferEvent event) { operation->observe(std::move(event)); };
}
} // namespace maskpilot
