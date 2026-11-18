#include <QApplication>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <maskpilot/ui/add_download_dialog.h>

TEST(TransferConstraintsUi, RequestEditingPreservesHiddenBoundsAndNewDialogKeepsDefaults) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    int argc = 1;
    char name[] = "TransferConstraintUi";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QCoreApplication::setOrganizationName("MaskPilotTests");
    QCoreApplication::setApplicationName("TransferConstraintUi");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    maskpilot::DownloadSpec source;
    source.url = "https://example.test/page";
    source.destination = std::filesystem::path(temporary.path().toStdWString()) / "page.html";
    source.maxStoredBytes = 4096;
    source.sameOriginRedirectOnly = true;
    maskpilot::AddDownloadDialog editor(false);
    editor.setSpecification(source);
    auto* address = editor.findChild<QLineEdit*>("downloadUrl");
    ASSERT_NE(address, nullptr);
    address->setText("https://example.test/changed");
    const auto edited = editor.specification();
    EXPECT_EQ(edited.maxStoredBytes, 4096);
    EXPECT_TRUE(edited.sameOriginRedirectOnly);
    EXPECT_EQ(edited.url, "https://example.test/changed");
    source.maxStoredBytes = 8192;
    source.sameOriginRedirectOnly = false;
    editor.setSpecification(source);
    EXPECT_EQ(editor.specification().maxStoredBytes, 8192);
    EXPECT_FALSE(editor.specification().sameOriginRedirectOnly);
    maskpilot::AddDownloadDialog fresh(false);
    EXPECT_EQ(fresh.specification().maxStoredBytes, 0);
    EXPECT_FALSE(fresh.specification().sameOriginRedirectOnly);
}
