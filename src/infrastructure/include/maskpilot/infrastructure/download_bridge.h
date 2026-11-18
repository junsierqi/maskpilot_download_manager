#pragma once
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <maskpilot/application/download_service.h>
#include <memory>

namespace maskpilot {
enum class BridgeAuthority { Legacy, Scoped, Administrative };
struct BridgeAccess {
    BridgeAuthority authority = BridgeAuthority::Scoped;
    std::string sessionToken;
};
// The desktop authenticates transport authority; application commands still enforce ownership.
// No request JSON field may construct Administrative authority.
class DownloadBridge final : public QObject {
  public:
    DownloadBridge(DownloadService& service, std::function<DownloadSpec(const QString&)> legacyDefaults,
                   std::function<void()> acceptedActivity = {}, QObject* parent = nullptr);
    ~DownloadBridge() override;
    void handle(QJsonObject request, BridgeAccess access, std::function<void(QJsonObject)> respond);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot
