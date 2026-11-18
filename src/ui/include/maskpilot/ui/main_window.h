#pragma once
#include <QJsonObject>
#include <QMainWindow>
#include <functional>
#include <maskpilot/application/download_service.h>
#include <memory>

namespace maskpilot {
class SiteProjectService;
class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(DownloadService& service, QWidget* parent = nullptr);
    ~MainWindow() override;
    void newDownload(QString url = {});
    DownloadSpec defaultSpec(const QString& url) const;
    void cancelCompletionCountdown();
    void enqueue(DownloadSpec spec, QString requestId = {},
                 std::function<void(QString, QString)> completion = {}, bool rememberProxy = false,
                 std::string queueId = defaultQueueId);
    void requestQuit();
    void setMediaInspector(MediaInspectionFunction);
    void setSiteProjectService(SiteProjectService&,
                               std::function<void(const std::filesystem::path&)> opener = {});
    void setSiteProjectUnavailable();

  protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot
