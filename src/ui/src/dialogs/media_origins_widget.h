#pragma once
#include <QWidget>
#include <functional>
#include <maskpilot/domain/task.h>
#include <memory>

namespace maskpilot::ui {
class MediaOriginsWidget final : public QWidget {
    Q_OBJECT
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    MediaOriginsWidget(bool chinese, std::function<std::string()> source, QWidget* parent = nullptr);
    ~MediaOriginsWidget() override;
    void readFrom(const DownloadSpec&);
    void writeTo(DownloadSpec&) const;
    void invalidateAccess();
  Q_SIGNALS:
    void changed();
};
} // namespace maskpilot::ui
