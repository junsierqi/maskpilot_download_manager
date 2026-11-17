#pragma once

#include <QIcon>
#include <QString>

namespace maskpilot::ui {
enum class Icon {
    Start,
    Pause,
    Cancel,
    Remove,
    Details,
    Batch,
    Settings,
    Download,
    Clock,
    Check,
    Alert,
    Torrent,
    Upload,
    Folder,
    Media
};

QIcon icon(Icon kind);
QString styleSheet();
} // namespace maskpilot::ui
