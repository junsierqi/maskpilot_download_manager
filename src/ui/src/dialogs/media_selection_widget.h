#pragma once
#include <QWidget>
#include <maskpilot/application/media.h>
#include <memory>

namespace maskpilot::ui {
class MediaSelectionWidget final : public QWidget {
    Q_OBJECT
  public:
    using Request = std::function<MediaInspectionRequest()>;
    explicit MediaSelectionWidget(bool chinese, Request, QWidget* parent = nullptr);
    ~MediaSelectionWidget() override;
    void setInspector(MediaInspectionFunction);
    void invalidate();
    void setSelection(const MediaSelection&, bool audioOnly, const std::string& legacyFormat);
    MediaSelection selection() const;
    bool audioOnly() const;
    std::string legacyFormat() const;
  signals:
    void changed();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace maskpilot::ui
