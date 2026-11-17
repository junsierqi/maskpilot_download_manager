#pragma once
#include <QJsonObject>
#include <QMainWindow>
#include <functional>
#include <maskpilot/application/download_service.h>
#include <memory>

namespace maskpilot {
class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(DownloadService& service, QWidget* parent = nullptr);
    ~MainWindow() override;
    void newDownload(QString url = {});
    DownloadSpec defaultSpec(const QString& url) const;
    void handleNative(const QJsonObject& request, std::function<void(QJsonObject)> respond);
    void enqueue(DownloadSpec spec, QString requestId = {},
                 std::function<void(QString, QString)> completion = {});
    void requestQuit();

  protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot
