#pragma once
#include "../process/owned_process.h"
#include <QByteArray>
#include <QProcess>
#include <QStringList>
#include <filesystem>
#include <functional>
#include <maskpilot/domain/task.h>
#include <memory>
namespace maskpilot::media_process {
using ProcessTree = process::OwnedProcessGroup;
QString pathString(const std::filesystem::path&);
QStringList commonArguments();
void validateRequest(const DownloadSpec&);
QByteArray privateInput(const DownloadSpec&);
MediaInventory parseInventory(const QByteArray&);
bool launch(QProcess&, ProcessTree&, const DownloadSpec&, const QStringList&, const std::filesystem::path&,
            const std::function<bool()>& stopped);
} // namespace maskpilot::media_process
