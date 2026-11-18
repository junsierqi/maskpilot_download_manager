#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <maskpilot/domain/task.h>

namespace maskpilot::ui {
struct DecodedTaskList {
    int version = 0;
    std::vector<DownloadSpec> downloads;
    std::size_t contextBoundCount = 0;
};
QByteArray encodeTaskList(const std::vector<Task>& tasks);
// Pure bounded format adapter. Does not perform network, filesystem, vault or task mutations.
DecodedTaskList decodeTaskList(const QByteArray& bytes,
                               const std::function<DownloadSpec(const QString&)>& trustedDefaults);
} // namespace maskpilot::ui
