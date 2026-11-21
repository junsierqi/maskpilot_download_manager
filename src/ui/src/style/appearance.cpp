#include "appearance.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <initializer_list>

namespace maskpilot::ui {
namespace {
void drawIcon(QPainter& painter, Icon kind) {
    const auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) {
        painter.drawLine(QPointF(x1, y1), QPointF(x2, y2));
    };
    const auto path = [&](std::initializer_list<QPointF> points, bool close = false) {
        QPainterPath shape;
        bool first = true;
        for (const auto& point : points) {
            if (first)
                shape.moveTo(point);
            else
                shape.lineTo(point);
            first = false;
        }
        if (close)
            shape.closeSubpath();
        painter.drawPath(shape);
    };
    switch (kind) {
    case Icon::Start:
        path({{8, 4}, {19, 12}, {8, 20}}, true);
        break;
    case Icon::Pause:
        line(8, 5, 8, 19);
        line(16, 5, 16, 19);
        break;
    case Icon::Cancel:
        painter.drawRoundedRect(QRectF(5, 5, 14, 14), 2, 2);
        break;
    case Icon::Remove:
        line(5, 7, 19, 7);
        path({{9, 7}, {9, 4}, {15, 4}, {15, 7}});
        path({{7, 7}, {8, 20}, {16, 20}, {17, 7}});
        line(10, 11, 10, 16);
        line(14, 11, 14, 16);
        break;
    case Icon::Details:
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        line(12, 11, 12, 17);
        painter.drawPoint(QPointF(12, 7));
        break;
    case Icon::Batch:
        for (int y : {6, 12, 18}) {
            path({{3, qreal(y)}, {5, qreal(y + 2)}, {8, qreal(y - 2)}});
            line(12, y, 21, y);
        }
        break;
    case Icon::Settings:
        line(4, 6, 20, 6);
        line(4, 12, 20, 12);
        line(4, 18, 20, 18);
        line(8, 3, 8, 9);
        line(16, 9, 16, 15);
        line(10, 15, 10, 21);
        break;
    case Icon::Download:
    case Icon::Upload:
        if (kind == Icon::Download) {
            line(12, 3, 12, 15);
            path({{7, 10}, {12, 15}, {17, 10}});
        } else {
            line(12, 15, 12, 3);
            path({{7, 8}, {12, 3}, {17, 8}});
        }
        path({{4, 15}, {4, 20}, {20, 20}, {20, 15}});
        break;
    case Icon::Clock:
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        path({{12, 6}, {12, 12}, {16, 14}});
        break;
    case Icon::Check:
        path({{4, 12}, {9, 17}, {20, 6}});
        break;
    case Icon::Alert:
        path({{12, 3}, {22, 20}, {2, 20}}, true);
        line(12, 9, 12, 13);
        painter.drawPoint(QPointF(12, 17));
        break;
    case Icon::Torrent:
        line(12, 6, 5, 17);
        line(12, 6, 19, 17);
        line(5, 17, 19, 17);
        for (const auto& point : {QPointF(12, 5), QPointF(4, 18), QPointF(20, 18)}) {
            painter.save();
            painter.setBrush(painter.pen().color());
            painter.drawEllipse(point, 2, 2);
            painter.restore();
        }
        break;
    case Icon::Folder:
        path({{3, 7}, {3, 5}, {9, 5}, {12, 8}, {21, 8}, {19, 20}, {3, 20}, {3, 7}}, true);
        line(3, 10, 20, 10);
        break;
    case Icon::Media:
        painter.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
        path({{10, 8}, {16, 12}, {10, 16}}, true);
        break;
    }
}
} // namespace

QIcon icon(Icon kind) {
    QIcon result;
    for (const auto mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active}) {
        for (const int ratio : {1, 2}) {
            QPixmap image(18 * ratio, 18 * ratio);
            image.setDevicePixelRatio(ratio);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(18.0 / 24.0, 18.0 / 24.0);
            const QColor color(mode == QIcon::Disabled ? "#a8b1c1"
                               : mode == QIcon::Active ? "#2563eb"
                                                       : "#475569");
            painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            drawIcon(painter, kind);
            painter.end();
            result.addPixmap(image, mode);
        }
    }
    return result;
}

QString styleSheet() {
    return QStringLiteral(R"qss(
QWidget {
    font-family: "Segoe UI", "Microsoft YaHei UI", sans-serif;
    font-size: 14px;
    color: #1e293b;
}
QMainWindow, QDialog { background: #f8fafc; }
QToolTip { background: #1e293b; color: white; border: none; padding: 6px 9px; }
QWidget#appHeader { background: white; border-bottom: 1px solid #e2e8f0; }
QToolBar { background: white; border: none; padding: 8px; spacing: 6px; }
QToolBar#taskToolbar { background: transparent; padding: 0px 0px 4px 0px; spacing: 6px; }
QToolBar::separator { background: #e2e8f0; width: 1px; margin: 5px 6px; }
QPushButton, QToolButton {
    background: white; border: 1px solid #cbd5e1; border-radius: 6px;
    padding: 6px 11px; color: #334155;
}
QToolButton { padding: 6px 9px; }
QPushButton:hover, QToolButton:hover { background: #f1f5f9; border-color: #94a3b8; }
QPushButton:pressed, QToolButton:pressed, QToolButton:checked { background: #e8efff; border-color: #93b4fb; }
QPushButton:focus, QToolButton:focus { border: 1px solid #2563eb; }
QPushButton:disabled, QToolButton:disabled { color: #94a3b8; background: #f1f5f9; border-color: #e2e8f0; }
QPushButton#newDownloadButton, QPushButton#emptyStateAddButton {
    background: #2563eb; border-color: #2563eb; color: white; font-weight: 600;
}
QPushButton#newDownloadButton:hover, QPushButton#emptyStateAddButton:hover { background: #1d4ed8; }
QPushButton#newDownloadButton:pressed, QPushButton#emptyStateAddButton:pressed { background: #1e40af; }
QPushButton#newDownloadButton:focus, QPushButton#emptyStateAddButton:focus { border: 2px solid #93c5fd; }
QPushButton#newDownloadButton:disabled, QPushButton#emptyStateAddButton:disabled { background: #cbd5e1; border-color: #cbd5e1; color: #64748b; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QTextEdit, QPlainTextEdit, QDateTimeEdit {
    background: white; border: 1px solid #cbd5e1; border-radius: 6px; padding: 6px 8px;
    selection-background-color: #2563eb; selection-color: white;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus,
QTextEdit:focus, QPlainTextEdit:focus, QDateTimeEdit:focus { border-color: #2563eb; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled,
QTextEdit:disabled, QPlainTextEdit:disabled { background: #f1f5f9; color: #94a3b8; }
QLineEdit#taskSearch { background: #f8fafc; padding: 7px 11px; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView { background: white; selection-background-color: #e8efff; selection-color: #1e40af; }
QListWidget#downloadSidebar { background: #f3f6fb; border: none; outline: none; padding: 8px; }
QListWidget#downloadSidebar::item { padding: 0px 10px; margin: 2px 0px; border-radius: 6px; color: #475569; }
QListWidget#downloadSidebar::item:hover { background: #eaf0f8; }
QListWidget#downloadSidebar::item:selected { background: #e8efff; color: #1d4ed8; font-weight: 600; }
QListWidget#downloadSidebar::item:focus { border: 1px solid #93b4fb; }
QTableView, QTreeView, QListView {
    background: white; alternate-background-color: #fafcff; border: 1px solid #e2e8f0;
    border-radius: 8px; gridline-color: #eef2f7; selection-background-color: #e8efff;
    selection-color: #1e293b; outline: none;
}
QTableView#downloadTable { border: none; border-radius: 0px; }
QTableView::item, QTreeView::item { padding: 5px 8px; border-bottom: 1px solid #f1f5f9; }
QTableView::item:hover, QTreeView::item:hover { background: #f4f7fc; }
QTableView::item:selected, QTreeView::item:selected { background: #e8efff; color: #1e293b; }
QTableView::item:focus, QTreeView::item:focus { border: 1px solid #93b4fb; }
QHeaderView::section { background: #f8fafc; color: #64748b; border: none; border-bottom: 1px solid #e2e8f0; padding: 10px 8px; font-weight: 600; }
QTableCornerButton::section { background: #f8fafc; border: none; }
QLabel#sectionTitle { color: #0f172a; font-size: 20px; font-weight: 600; }
QLabel#taskSummary { color: #64748b; }
QLabel#emptyStateFrame { background: white; border: 1px solid #e2e8f0; border-radius: 10px; }
QLabel#emptyHeading { color: #1e293b; font-size: 20px; font-weight: 600; }
QLabel#emptyHint { color: #64748b; }
QLabel#throughput { color: #475569; font-size: 12px; }
QStatusBar { background: white; border-top: 1px solid #e2e8f0; color: #64748b; }
QStatusBar::item { border: none; }
QTabWidget::pane { border: 1px solid #e2e8f0; border-radius: 6px; background: white; }
QTabBar::tab { background: #f1f5f9; color: #64748b; padding: 9px 15px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { background: white; color: #1d4ed8; border-bottom-color: #2563eb; }
QTabBar::tab:hover { color: #2563eb; background: #e8efff; }
QTabBar QToolButton { padding: 0px; }
QGroupBox { border: 1px solid #e2e8f0; border-radius: 8px; margin-top: 14px; padding: 14px 10px 10px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0px 5px; }
QCheckBox, QRadioButton { spacing: 7px; }
QCheckBox { padding: 1px 0px 1px 2px; }
QCheckBox:disabled, QRadioButton:disabled, QLabel:disabled { color: #94a3b8; }
QProgressBar { border: none; border-radius: 4px; background: #e8edf5; text-align: center; min-height: 8px; }
QProgressBar::chunk { background: #2563eb; border-radius: 4px; }
QMenu { background: white; border: 1px solid #e2e8f0; padding: 5px; }
QMenu::item { padding: 7px 26px 7px 12px; border-radius: 4px; }
QMenu::item:selected { background: #e8efff; color: #1d4ed8; }
QMenu::item:disabled { color: #94a3b8; }
QMenu::separator { height: 1px; background: #e2e8f0; margin: 5px; }
QSplitter::handle { background: #e2e8f0; }
QScrollBar:vertical { background: #f8fafc; width: 11px; margin: 0px; }
QScrollBar::handle:vertical { background: #cbd5e1; border-radius: 5px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: #94a3b8; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
)qss");
}
} // namespace maskpilot::ui
