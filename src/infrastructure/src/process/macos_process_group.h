#pragma once
#include <QProcess>

namespace maskpilot::process {
// EOF is the lifetime capability; no stale numeric process-group signaling in the parent.
class MacProcessGroup {
    int reader_ = -1, writer_ = -1, descriptorLimit_ = 0;
    bool prepared_ = false;
    QMetaObject::Connection finished_;
    void releaseWriter() noexcept;

  public:
    MacProcessGroup();
    ~MacProcessGroup();
    MacProcessGroup(const MacProcessGroup&) = delete;
    MacProcessGroup& operator=(const MacProcessGroup&) = delete;
    void prepare(QProcess&);
    void stop(QProcess&);
};
} // namespace maskpilot::process
