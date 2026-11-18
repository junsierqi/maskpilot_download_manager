#pragma once
#include "../dialogs/download_name.h"
#include "../style/appearance.h"
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
#include <QFontMetrics>
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
#include <QUrlQuery>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <map>
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
inline QString filenameFor(const QString& url) { return ui::defaultDownloadName(url); }
inline QString stateText(TaskState state, bool chinese) {
    static const char* zh[] = {"排队中",   "已计划",   "连接中",     "下载中",   "正在暂停",    "已暂停",
                               "等待重试", "校验中",   "正在完成",   "已完成",   "失败",        "正在取消",
                               "已取消",   "需要处理", "获取元数据", "正在做种", "等待选择文件"};
    const auto index = static_cast<int>(state);
    return chinese && index >= 0 && index < static_cast<int>(std::size(zh))
               ? QString::fromUtf8(zh[index])
               : QString::fromUtf8(stateName(state));
}
class TaskModel final : public QAbstractTableModel {
    std::array<QIcon, 4> icons_;

  public:
    std::vector<Task> rows;
    std::map<std::string, QString> contextLabels;
    bool chinese = false;
    explicit TaskModel(QObject* parent)
        : QAbstractTableModel(parent), icons_{ui::icon(ui::Icon::Download), ui::icon(ui::Icon::Download),
                                              ui::icon(ui::Icon::Media), ui::icon(ui::Icon::Torrent)} {}
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 8; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
            section >= columnCount())
            return {};
        static const char* en[] = {"Name",   "Size",     "Progress",  "Speed",
                                   "Status", "Category", "Remaining", "Upload"};
        static const char* zh[] = {"文件名", "大小", "进度", "速度", "状态", "分类", "剩余时间", "上传"};
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
                                  : (task.state == TaskState::Completed || task.torrentPublished ? 100 : -1);
        if (role == Qt::DecorationRole && index.column() == 0) {
            const auto kind = static_cast<std::size_t>(task.spec.kind);
            return icons_[kind < icons_.size() ? kind : 0];
        }
        if (role == Qt::UserRole + 2) {
            const auto scope =
                task.ownerId == "standalone"
                    ? (chinese ? QStringLiteral("独立下载区") : QStringLiteral("Standalone"))
                    : (contextLabels.contains(task.ownerId)
                           ? contextLabels.at(task.ownerId)
                           : (chinese ? QStringLiteral("未知上下文") : QStringLiteral("Unknown context")));
            if (task.spec.kind == TransferKind::BitTorrent)
                return scope + QStringLiteral(" · BT · ") +
                       (task.spec.url.starts_with("magnet:?")
                            ? (chinese ? QStringLiteral("磁力链接") : QStringLiteral("Magnet link"))
                            : (chinese ? QStringLiteral("种子文件") : QStringLiteral("Torrent file")));
            const QUrl source(qs(task.spec.url));
            auto protocol = source.scheme().toUpper();
            if (task.spec.kind == TransferKind::Media)
                protocol = chinese ? QStringLiteral("媒体") : QStringLiteral("MEDIA");
            else if (task.spec.ftpExplicitTls)
                protocol = QStringLiteral("FTPS");
            return scope + " · " + protocol + (source.host().isEmpty() ? QString() : " · " + source.host());
        }
        if (role == Qt::TextAlignmentRole)
            return static_cast<int>(
                (index.column() == 1 || index.column() == 3 || index.column() == 6 || index.column() == 7)
                    ? Qt::AlignRight | Qt::AlignVCenter
                    : Qt::AlignLeft | Qt::AlignVCenter);
        if (role == Qt::ToolTipRole)
            return pathString(task.spec.destination) +
                   (task.error.empty() ? QString() : "\n" + qs(task.error));
        if (role == Qt::ForegroundRole) {
            if (index.column() == 4 &&
                (task.state == TaskState::Failed || task.state == TaskState::NeedsAttention))
                return QColor("#c24135");
            if (index.column() == 4 &&
                (task.state == TaskState::Completed || task.state == TaskState::Seeding))
                return QColor("#22835e");
            if (index.column() == 4 && task.state == TaskState::AwaitingSelection)
                return QColor("#946200");
            return QColor("#263247");
        }
        if (role != Qt::DisplayRole)
            return {};
        switch (index.column()) {
        case 0:
            return task.spec.kind == TransferKind::BitTorrent && !task.torrent.name.empty()
                       ? qs(task.torrent.name)
                       : pathString(task.spec.destination.filename());
        case 1:
            return bytes(task.total);
        case 2:
            return bytes(task.downloaded);
        case 3:
            return task.bytesPerSecond > 0 ? bytes(task.bytesPerSecond) + "/s" : QStringLiteral("—");
        case 4:
            return stateText(task.state, chinese);
        case 5:
            if (chinese) {
                static const char* keys[] = {"Documents", "Software", "Video",
                                             "Music",     "Archives", "General"};
                static const char* labels[] = {"文档", "软件", "视频", "音乐", "压缩包", "常规"};
                for (std::size_t n = 0; n < std::size(keys); ++n)
                    if (task.spec.category == keys[n])
                        return QString::fromUtf8(labels[n]);
            }
            return qs(task.spec.category);
        case 6:
            if (task.bytesPerSecond > 0 && task.total > task.downloaded) {
                auto secs = (task.total - task.downloaded) / task.bytesPerSecond;
                return QString::number(secs / 60) + "m " + QString::number(secs % 60) + "s";
            }
            return QStringLiteral("—");
        case 7:
            return task.spec.kind == TransferKind::BitTorrent ? bytes(task.uploadSpeed) + "/s"
                                                              : QStringLiteral("—");
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
                emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
        } else {
            beginResetModel();
            rows = std::move(value);
            endResetModel();
        }
    }
};
class TaskDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (index.column() != 0 && index.column() != 2) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        auto background = option;
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = QIcon();
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &background, painter);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        painter->save();
        if (index.column() == 0) {
            const auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            icon.paint(painter, QRect(option.rect.left() + 12, option.rect.center().y() - 9, 18, 18));
            auto font = option.font;
            font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            painter->setPen(QColor(selected ? "#1d4ed8" : "#1f2937"));
            const auto title = option.rect.adjusted(40, 7, -12, -26);
            painter->drawText(title, Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight,
                                                            std::max(0, title.width())));
            font.setWeight(QFont::Normal);
            font.setPixelSize(12);
            painter->setFont(font);
            painter->setPen(QColor(selected ? "#315b9c" : "#64748b"));
            const auto subtitle = option.rect.adjusted(40, 29, -12, -7);
            painter->drawText(subtitle, Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(font).elidedText(index.data(Qt::UserRole + 2).toString(),
                                                            Qt::ElideRight, std::max(0, subtitle.width())));
            painter->restore();
            return;
        }
        const int progress = index.data(Qt::UserRole + 1).toInt();
        auto font = option.font;
        font.setPixelSize(12);
        painter->setFont(font);
        painter->setPen(QColor(selected ? "#1d4ed8" : "#475569"));
        const auto label = option.rect.adjusted(10, 7, -10, -26);
        painter->drawText(label, Qt::AlignLeft | Qt::AlignVCenter, index.data().toString());
        painter->drawText(label, Qt::AlignRight | Qt::AlignVCenter,
                          progress >= 0 ? QString::number(progress) + "%" : QStringLiteral("—"));
        const QRect rect(option.rect.left() + 10, option.rect.top() + 36,
                         std::max(0, option.rect.width() - 20), 4);
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
