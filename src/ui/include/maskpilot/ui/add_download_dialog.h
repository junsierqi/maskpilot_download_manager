#pragma once
#include <QDialog>
#include <maskpilot/domain/task.h>
#include <memory>

namespace maskpilot {
class AddDownloadDialog final : public QDialog {
  public:
    explicit AddDownloadDialog(bool chinese, QWidget* parent = nullptr, QString initialUrl = {});
    ~AddDownloadDialog() override;
    DownloadSpec specification() const;
    void setSpecification(const DownloadSpec& spec);
    bool rememberProxyCredentials() const;
    void setStoredProxyCredentials(bool stored);
    void setQueues(const std::vector<DownloadQueue>&, std::string selected = defaultQueueId,
                   bool editable = true);
    std::string queueId() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot
