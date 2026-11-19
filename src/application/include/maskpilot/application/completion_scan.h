#pragma once
#include <maskpilot/application/ports.h>

namespace maskpilot {
struct CompletionScanOptions {
    bool enabled = false;
    std::filesystem::path program;
    std::vector<std::string> arguments{"{file}"};
    int timeoutSeconds = 120;
};
std::string validateCompletionScanOptions(const CompletionScanOptions&);
std::string validateCompletionScanRecord(const Task&);
class ICompletionScanner {
  public:
    virtual ~ICompletionScanner() = default;
    virtual CompletionScanResult scan(const std::filesystem::path&, const TransferControl&) = 0;
};
using CompletionScannerFactory =
    std::function<std::shared_ptr<ICompletionScanner>(const CompletionScanOptions&)>;
struct CompletionScanDependencies {
    CompletionScanOptions options;
    CompletionScannerFactory factory;
};
// Installs a synchronous publication gate and returns the transport's progress observer.
std::function<void(TransferEvent)> attachCompletionScanner(TransferControl&,
                                                           const CompletionScanDependencies&,
                                                           std::function<void(TransferEvent)> observer);
} // namespace maskpilot
