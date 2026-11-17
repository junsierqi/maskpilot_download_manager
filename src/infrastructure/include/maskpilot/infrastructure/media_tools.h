#pragma once
#include <QStringList>
#include <maskpilot/application/ports.h>

namespace maskpilot {
QStringList mediaArguments(const Task& task, const std::filesystem::path& workDirectory,
                           std::int64_t speedLimit);
std::unique_ptr<ITransferTransport> makeUnifiedTransfer();
} // namespace maskpilot
