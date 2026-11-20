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
class MediaOriginGateway;
class ProcessTree final {
    std::unique_ptr<MediaOriginGateway> gateway_;
    process::OwnedProcessGroup group_;

  public:
    ProcessTree();
    ~ProcessTree();
    void prepare(QProcess& process);
    void stop(QProcess& process);
    DownloadSpec route(const DownloadSpec& specification);
};
QString pathString(const std::filesystem::path&);
QStringList commonArguments();
void validateRequest(const DownloadSpec&);
QByteArray privateInput(const DownloadSpec&);
MediaInventory parseInventory(const QByteArray&);
bool launch(QProcess&, ProcessTree&, const DownloadSpec&, const QStringList&, const std::filesystem::path&,
            const std::function<bool()>& stopped);
} // namespace maskpilot::media_process
