#pragma once
#include <QAbstractTableModel>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleOptionProgressBar>
#include <QStyledItemDelegate>
#include <QSystemTrayIcon>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
#include <maskpilot/ui/add_download_dialog.h>
#include <maskpilot/ui/main_window.h>
#include <set>

namespace maskpilot {

inline QString qs(const std::string& s) { return QString::fromStdString(s); }
inline QString pathString(const std::filesystem::path& p) { return QString::fromStdWString(p.wstring()); }
inline QString bytes(std::int64_t value) {
    if (value < 0)
        return QStringLiteral("—");
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double number = static_cast<double>(value);
    int unit = 0;
    while (number >= 1024 && unit < 4) {
        number /= 1024;
        ++unit;
    }
    return QString::number(number, 'f', unit ? 1 : 0) + " " + units[unit];
}
inline QString filenameFor(const QString& url) {
    auto name = QFileInfo(QUrl(url).path()).fileName();
    name.replace(QRegularExpression(R"([<>:"/\\|?*\x00-\x1f])"), "_");
    while (name.endsWith(' ') || name.endsWith('.'))
        name.chop(1);
    if (name.isEmpty() || name == "." || name == "..")
        name = "download.bin";
    if (QRegularExpression(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$))",
                           QRegularExpression::CaseInsensitiveOption)
            .match(name)
            .hasMatch())
        name.prepend('_');
    return name.left(180);
}
inline QString stateText(TaskState state, bool chinese) {
    static const char* zh[] = {"排队中", "已计划",   "连接中", "下载中", "正在暂停", "已暂停", "等待重试",
                               "校验中", "正在完成", "已完成", "失败",   "正在取消", "已取消", "需要处理"};
    return chinese ? QString::fromUtf8(zh[static_cast<int>(state)]) : QString::fromUtf8(stateName(state));
}
class TaskModel final : public QAbstractTableModel {
  public:
    std::vector<Task> rows;
    bool chinese = false;
    explicit TaskModel(QObject* parent) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 7; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        static const char* en[] = {"Name", "Size", "Progress", "Speed", "Status", "Category", "Remaining"};
        static const char* zh[] = {"文件名", "大小", "进度", "速度", "状态", "分类", "剩余时间"};
        return QString::fromUtf8(chinese ? zh[section] : en[section]);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= static_cast<int>(rows.size()))
            return {};
        const auto& task = rows[static_cast<size_t>(index.row())];
        if (role == Qt::UserRole)
            return qs(task.id);
        if (role == Qt::UserRole + 1)
            return task.total > 0 ? static_cast<int>(std::clamp<long double>(
                                        static_cast<long double>(task.downloaded) * 100 / task.total, 0, 100))
                                  : (task.state == TaskState::Completed ? 100 : -1);
        if (role == Qt::ToolTipRole)
            return pathString(task.spec.destination) +
                   (task.error.empty() ? QString() : "\n" + qs(task.error));
        if (role == Qt::ForegroundRole) {
            if (task.state == TaskState::Failed || task.state == TaskState::NeedsAttention)
                return QColor("#c24135");
            if (task.state == TaskState::Completed)
                return QColor("#22835e");
            return QColor("#263247");
        }
        if (role != Qt::DisplayRole)
            return {};
        switch (index.column()) {
        case 0:
            return pathString(task.spec.destination.filename());
        case 1:
            return bytes(task.total);
        case 2:
            return bytes(task.downloaded);
        case 3:
            return task.bytesPerSecond > 0 ? bytes(task.bytesPerSecond) + "/s" : QStringLiteral("—");
        case 4:
            return stateText(task.state, chinese);
        case 5:
            return qs(task.spec.category);
        case 6:
            if (task.bytesPerSecond > 0 && task.total > task.downloaded) {
                auto secs = (task.total - task.downloaded) / task.bytesPerSecond;
                return QString::number(secs / 60) + "m " + QString::number(secs % 60) + "s";
            }
            return QStringLiteral("—");
        default:
            return {};
        }
    }
    void setRows(std::vector<Task> value) {
        bool same = value.size() == rows.size();
        if (same)
            for (size_t i = 0; i < value.size(); ++i)
                if (value[i].id != rows[i].id) {
                    same = false;
                    break;
                }
        if (same) {
            rows = std::move(value);
            if (!rows.empty())
                emit dataChanged(index(0, 0), index(rowCount() - 1, 6));
        } else {
            beginResetModel();
            rows = std::move(value);
            endResetModel();
        }
    }
};
class ProgressDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (index.column() != 2) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        auto background = option;
        initStyleOption(&background, index);
        background.text.clear();
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &background, painter);
        const int progress = index.data(Qt::UserRole + 1).toInt();
        auto rect = option.rect.adjusted(10, 14, -12, -14);
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor("#e8edf5"));
        painter->drawRoundedRect(rect, 3, 3);
        if (progress >= 0) {
            auto fill = rect;
            fill.setWidth(rect.width() * progress / 100);
            painter->setBrush(progress == 100 ? QColor("#32a77a") : QColor("#3976f2"));
            painter->drawRoundedRect(fill, 3, 3);
        }
        painter->restore();
    }
};
} // namespace maskpilot
