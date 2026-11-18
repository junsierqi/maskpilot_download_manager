#pragma once
#include <QDialog>
#include <filesystem>
#include <functional>
#include <memory>
namespace maskpilot {
class SiteProjectService;
class DownloadService;
class SiteProjectDialog final : public QDialog {
  public:
    SiteProjectDialog(SiteProjectService&, DownloadService&, bool chinese,
                      std::function<void(const std::filesystem::path&)> opener = {},
                      QWidget* parent = nullptr);
    ~SiteProjectDialog() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot
