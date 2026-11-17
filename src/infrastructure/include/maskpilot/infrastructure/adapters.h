#pragma once
#include <maskpilot/application/ports.h>

namespace maskpilot {
std::unique_ptr<ITaskRepository> makeSqliteRepository(const std::filesystem::path& path);
std::unique_ptr<ITransferTransport> makeCurlTransfer();
std::string sha256File(const std::filesystem::path& path, const TransferControl* control = nullptr);
} // namespace maskpilot
