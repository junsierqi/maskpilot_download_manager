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

#include "../models/task_model.h"
#include <windows.h>
namespace maskpilot {
inline QString systemExecutable(const wchar_t* name) {
    wchar_t directory[32768];
    const auto count = GetSystemDirectoryW(directory, static_cast<UINT>(std::size(directory)));
    if (!count || count >= std::size(directory))
        return {};
    return QDir(QString::fromWCharArray(directory, static_cast<int>(count)))
        .filePath(QString::fromWCharArray(name));
}
struct MainWindow::Impl {
    MainWindow* window;
    DownloadService& service;
    QSettings settings;
    bool chinese, quitting = false, wasBusy = false, refreshing = false;
    bool installingMedia = false;
    QLineEdit* search = nullptr;
    QListWidget* sidebar = nullptr;
    QTableView* table = nullptr;
    TaskModel* model = nullptr;
    QLabel *heading = nullptr, *summary = nullptr, *empty = nullptr, *throughput = nullptr;
    QSystemTrayIcon* tray = nullptr;
    QTimer* timer = nullptr;
    QPointer<QDialog> completionDialog;
    std::set<std::string> batchIds;
    std::set<std::string> seenCompleted;
    bool completionPrimed = false;
    QAction *resumeAction = nullptr, *pauseAction = nullptr, *cancelAction = nullptr, *removeAction = nullptr,
            *detailAction = nullptr;
    struct Pending {
        std::future<std::string> result;
        std::function<void(QString, QString)> completion;
    };
    std::vector<Pending> pending;
    std::vector<Task> all;
    explicit Impl(MainWindow* w, DownloadService& s);
    QString t(const char* en, const char* zh) const;
    std::vector<Task> selected() const;
    void notify(const QString& title, const QString& text);
    void updateActions();
    void refresh();
    void completion();
    void details();
    void torrentDetails(const Task& task);
    void settingsDialog();
    void mediaSupport();
    void exportTasks();
    void importTasks();
    void batch();
};
} // namespace maskpilot
