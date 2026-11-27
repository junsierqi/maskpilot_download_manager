#include "../../src/ui/src/dialogs/download_name.h"
#include "../../src/ui/src/models/task_list_codec.h"
#include "../../src/ui/src/style/appearance.h"
#include <QApplication>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <maskpilot/ui/add_download_dialog.h>
using namespace maskpilot;
TEST(Ed2kUi, RoundTripsControlsFilenameAndStrictTaskList) {
    if (!qEnvironmentVariableIsSet("MASKPILOT_TEST_NATIVE_UI"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char name[] = "Ed2kUi";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName("Ed2kUi");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    DownloadSpec spec;
    spec.url = "ed2k://|file|hello%20world.bin|3|a448017aaf21d8525fc10ae87aa6729d|/";
    spec.destination = std::filesystem::path(temporary.path().toStdU16String()) / "file.bin";
    spec.ed2k.servers = {parseEd2kEndpoint("index.example:4661", true)};
    spec.ed2k.sources = {parseEd2kEndpoint("127.0.0.1:4662")};
    AddDownloadDialog dialog(false, nullptr, QString::fromStdString(spec.url));
    dialog.setStyleSheet(ui::styleSheet());
    dialog.setSpecification(spec);
    EXPECT_EQ(dialog.specification().ed2k, spec.ed2k);
    EXPECT_EQ(dialog.specification().kind, TransferKind::Ed2k);
    EXPECT_EQ(ui::defaultDownloadName(QString::fromStdString(spec.url)), "hello world.bin");
    auto* tabs = dialog.findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    int ed2k = -1;
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i) == "ED2K")
            ed2k = i;
    ASSERT_GE(ed2k, 0);
    EXPECT_TRUE(tabs->isTabEnabled(ed2k));
    tabs->setCurrentIndex(ed2k);
    dialog.show();
    QApplication::processEvents();
    if (const auto output = qEnvironmentVariable("MASKPILOT_ED2K_UI_SCREENSHOT"); !output.isEmpty()) {
        EXPECT_TRUE(dialog.grab().save(output));
        dialog.resize(560, 610);
        QApplication::processEvents();
        EXPECT_TRUE(dialog.grab().save(output + ".narrow.png"));
    }
    Task task;
    task.spec = dialog.specification();
    const auto encoded = ui::encodeTaskList({task});
    const auto decoded = ui::decodeTaskList(encoded, [](const QString&) { return DownloadSpec{}; });
    ASSERT_EQ(decoded.downloads.size(), 1);
    EXPECT_EQ(decoded.downloads.front().ed2k, spec.ed2k);
    EXPECT_TRUE(decoded.downloads.front().startPaused);
    EXPECT_EQ(decoded.downloads.front().kind, TransferKind::Ed2k);
    auto bad = encoded;
    bad.replace("\"servers\":", "\"unknown\":");
    EXPECT_THROW(ui::decodeTaskList(bad, [](const QString&) { return DownloadSpec{}; }),
                 std::invalid_argument);
    std::string longLink = "ed2k://|file|large.bin|9728000001|a448017aaf21d8525fc10ae87aa6729d|p=";
    for (int i = 0; i < 1001; ++i) {
        if (i)
            longLink += ':';
        longLink += "a448017aaf21d8525fc10ae87aa6729d";
    }
    longLink += "|/";
    ASSERT_GT(longLink.size(), 32767);
    AddDownloadDialog large(false, nullptr, QString::fromStdString(longLink));
    EXPECT_EQ(large.specification().url, longLink);
    Task largeTask;
    largeTask.spec = large.specification();
    const auto largeDecoded =
        ui::decodeTaskList(ui::encodeTaskList({largeTask}), [](const QString&) { return DownloadSpec{}; });
    ASSERT_EQ(largeDecoded.downloads.size(), 1);
    EXPECT_EQ(largeDecoded.downloads.front().url, longLink);
    dialog.findChild<QPlainTextEdit*>("ed2kServers")->setPlainText("invalid");
    EXPECT_THROW(dialog.specification(), std::invalid_argument);
}
