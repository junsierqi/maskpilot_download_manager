#pragma once
#include <QProcess>
#include <memory>
namespace maskpilot::process {
class OwnedProcessGroup {
  public:
    OwnedProcessGroup();
    ~OwnedProcessGroup();
    OwnedProcessGroup(const OwnedProcessGroup&) = delete;
    OwnedProcessGroup& operator=(const OwnedProcessGroup&) = delete;
    void prepare(QProcess&);
    void stop(QProcess&);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot::process
