#pragma once
#include <QStringList>
#include <maskpilot/application/media.h>
#include <maskpilot/infrastructure/adapters.h>

namespace maskpilot {
QStringList mediaArguments(const Task& task, const std::filesystem::path& workDirectory,
                           std::int64_t speedLimit);
std::unique_ptr<ITransferTransport> makeMediaTransfer();
std::shared_ptr<IMediaInspector> makeMediaInspector();
} // namespace maskpilot
