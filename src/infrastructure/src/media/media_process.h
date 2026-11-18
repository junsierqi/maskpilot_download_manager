#pragma once
#include <QByteArray>
#include <QProcess>
#include <QStringList>
#include <filesystem>
#include <functional>
#include <maskpilot/domain/task.h>
#include <memory>
namespace maskpilot::media_process {
class ProcessTree {
  public:
    ProcessTree();
    ~ProcessTree();
    ProcessTree(const ProcessTree&) = delete;
    ProcessTree& operator=(const ProcessTree&) = delete;
    void prepare(QProcess&);
    void stop(QProcess&);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
QString pathString(const std::filesystem::path&);
QStringList commonArguments();
void validateRequest(const DownloadSpec&);
QByteArray privateInput(const DownloadSpec&);
MediaInventory parseInventory(const QByteArray&);
bool launch(QProcess&, ProcessTree&, const DownloadSpec&, const QStringList&, const std::filesystem::path&,
            const std::function<bool()>& stopped);
} // namespace maskpilot::media_process
