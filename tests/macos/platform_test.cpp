#include "../../src/infrastructure/src/filesystem/platform_paths.h"
#include "../../src/infrastructure/src/process/owned_process.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <Security/Security.h>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/credential_vault.h>
#include <maskpilot/infrastructure/torrent_engine.h>

using namespace maskpilot;
namespace {
std::string id() { return QUuid::createUuid().toString(QUuid::Id128).toStdString(); }
const ProxyCredentials credentials{"owned-macos-user", "owned-test-secret-中文"};
Boolean interactionAllowed() {
    Boolean allowed = false;
    if (SecKeychainGetUserInteractionAllowed(&allowed) != errSecSuccess)
        throw std::runtime_error("Cannot inspect Keychain interaction state");
    return allowed;
}

TEST(MacPaths, OnlyVerifiedSystemDirectoryAliasesAreAllowed) {
    EXPECT_TRUE(platform_paths::isSystemDirectoryAlias("/var"));
    EXPECT_TRUE(platform_paths::isSystemDirectoryAlias("/tmp"));
    EXPECT_FALSE(platform_paths::isSystemDirectoryAlias("/private/var"));
    EXPECT_FALSE(platform_paths::isSystemDirectoryAlias("/var/folders"));
    EXPECT_FALSE(platform_paths::isSystemDirectoryAlias("/var/../tmp"));
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdU16String());
    std::filesystem::create_directory(root / "owned");
    std::filesystem::create_directory_symlink(root / "owned", root / "user-link");
    EXPECT_FALSE(platform_paths::isSystemDirectoryAlias(root / "user-link"));
    std::filesystem::create_directory_symlink(root / "owned", root / "var");
    EXPECT_FALSE(platform_paths::isSystemDirectoryAlias(root / "var"));
}
template <class F> void bindingFailure(F action) {
    try {
        action();
        FAIL() << "Unexpected credential access";
    } catch (const CredentialVaultError& error) {
        EXPECT_EQ(error.failure(), CredentialVaultFailure::Binding);
    }
}
TEST(MacVault, NativeRoundTripBindingCreateOnlyAndOwnedRemoval) {
    const auto interaction = interactionAllowed();
    const auto installation = id();
    auto vault = makeNativeCredentialVault(installation);
    ProxyCredentialBinding binding{"standalone", "http://127.0.0.1:3128", id()};
    const auto reference = vault->remember(binding, credentials);
    // Ensure assertion failures cannot leave an owned fake entry behind.
    struct Cleanup {
        ICredentialVault& vault;
        ProxyCredentialBinding binding;
        std::string reference;
        ~Cleanup() {
            try {
                vault.forget(binding, reference);
            } catch (...) {
            }
        }
    } cleanup{*vault, binding, reference};
    EXPECT_EQ(interactionAllowed(), interaction);
    auto reopened = makeNativeCredentialVault(installation);
    const auto restored = reopened->recall(binding, reference);
    EXPECT_EQ(interactionAllowed(), interaction);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->username, credentials.username);
    EXPECT_EQ(restored->password, credentials.password);
    try {
        reopened->store(reference, binding, credentials);
        FAIL() << "Existing entry overwritten";
    } catch (const CredentialVaultError& error) {
        EXPECT_EQ(error.failure(), CredentialVaultFailure::AlreadyExists);
    }
    EXPECT_EQ(interactionAllowed(), interaction);
    for (int change = 0; change < 3; ++change) {
        auto other = binding;
        if (change == 0)
            other.ownerId = id();
        if (change == 1)
            other.proxyEndpoint = "http://127.0.0.1:3129";
        if (change == 2)
            other.ownershipNonce = id();
        bindingFailure([&] { reopened->recall(other, reference); });
        bindingFailure([&] { reopened->forget(other, reference); });
        EXPECT_TRUE(reopened->recall(binding, reference));
    }
    EXPECT_FALSE(makeNativeCredentialVault(id())->recall(binding, reference));
    EXPECT_TRUE(reopened->forget(binding, reference));
    EXPECT_FALSE(reopened->recall(binding, reference));
    EXPECT_FALSE(reopened->forget(binding, reference));
    EXPECT_EQ(interactionAllowed(), interaction);
}
TEST(MacVault, PreservesAlreadyDisabledInteraction) {
    const auto original = interactionAllowed();
    struct Restore {
        Boolean original;
        ~Restore() { SecKeychainSetUserInteractionAllowed(original); }
    } restore{original};
    ASSERT_EQ(SecKeychainSetUserInteractionAllowed(false), errSecSuccess);
    const auto installation = id();
    auto vault = makeNativeCredentialVault(installation);
    const ProxyCredentialBinding binding{"standalone", "http://127.0.0.1:3128", id()};
    const auto reference = vault->remember(binding, credentials);
    EXPECT_FALSE(interactionAllowed());
    EXPECT_TRUE(vault->recall(binding, reference));
    EXPECT_FALSE(interactionAllowed());
    EXPECT_TRUE(vault->forget(binding, reference));
    EXPECT_FALSE(interactionAllowed());
}
TEST(MacFiles, ExclusiveDirectoryRenamePreservesExistingDestination) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto root = std::filesystem::path(directory.path().toStdString());
    const auto source = root / "source", destination = root / "destination";
    std::filesystem::create_directory(source);
    std::filesystem::create_directory(destination);
    std::ofstream(source / "sentinel") << "owned";
    EXPECT_EQ(renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL), -1);
    EXPECT_EQ(errno, EEXIST);
    EXPECT_TRUE(std::filesystem::exists(source / "sentinel"));
    std::filesystem::remove(destination);
    EXPECT_EQ(renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL), 0);
    EXPECT_TRUE(std::filesystem::exists(destination / "sentinel"));
}
TEST(MacTorrent, LocalMetadataAliasesAndUnsafeFiles) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto root = std::filesystem::path(directory.path().toStdString());
    const auto source = root / "owned.torrent";
    std::ofstream(source, std::ios::binary)
        << "d4:infod6:lengthi3e4:name7:abc.bin12:piece lengthi16384e6:pieces20:abcdefghijklmnopqrstee";
    DownloadSpec spec;
    spec.kind = TransferKind::BitTorrent;
    spec.url = "torrent:" + source.string();
    spec.destination = root / "output";
    EXPECT_NO_THROW(describeTorrent(spec));
    std::filesystem::create_symlink(source, root / "link.torrent");
    spec.url = "torrent:" + (root / "link.torrent").string();
    EXPECT_THROW(describeTorrent(spec), std::invalid_argument);
    spec.url = "torrent:/Network/owned.torrent";
    EXPECT_THROW(describeTorrent(spec), std::invalid_argument);
}
int guardian(const QStringList& arguments) {
    if (arguments.size() != 5)
        return 2;
    process::OwnedProcessGroup group;
    QProcess child;
    group.prepare(child);
    child.start(arguments[2], {arguments[3], arguments[4]});
    if (!child.waitForStarted(10000))
        return 3;
    child.write("owned-private-input\n");
    child.closeWriteChannel();
    QElapsedTimer timer;
    timer.start();
    while (!QFile::exists(arguments[3]) && timer.elapsed() < 20000) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    if (!QFile::exists(arguments[3])) {
        group.stop(child);
        return 4;
    }
    if (arguments[1] == "--guardian-stop") {
        group.stop(child);
        return 0;
    }
    while (timer.elapsed() < 180000 && child.state() != QProcess::NotRunning) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    group.stop(child);
    return 5;
}
int vaultMode(const QStringList& arguments) {
    if (arguments.size() != 3)
        return 2;
    QFile file(arguments[2]);
    const auto interaction = interactionAllowed();
    if (arguments[1] == "--seed-vault") {
        const auto installation = id();
        const ProxyCredentialBinding binding{"standalone", "http://127.0.0.1:3128", id()};
        const auto reference = makeNativeCredentialVault(installation)->remember(binding, credentials);
        if (interactionAllowed() != interaction)
            return 6;
        const QJsonObject record{{"installation", QString::fromStdString(installation)},
                                 {"reference", QString::fromStdString(reference)},
                                 {"nonce", QString::fromStdString(binding.ownershipNonce)}};
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return 3;
        return file.write(QJsonDocument(record).toJson()) > 0 ? 0 : 3;
    }
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096)
        return 3;
    const auto record = QJsonDocument::fromJson(file.readAll()).object();
    const ProxyCredentialBinding binding{"standalone", "http://127.0.0.1:3128",
                                         record["nonce"].toString().toStdString()};
    try {
        makeNativeCredentialVault(record["installation"].toString().toStdString())
            ->recall(binding, record["reference"].toString().toStdString());
    } catch (const CredentialVaultError& error) {
        if (interactionAllowed() != interaction)
            return 6;
        return error.failure() == CredentialVaultFailure::Unavailable ||
                       error.failure() == CredentialVaultFailure::AccessDenied
                   ? 0
                   : 4;
    }
    return 5;
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    try {
        if (arguments.size() > 1 && arguments[1].startsWith("--guardian-"))
            return guardian(arguments);
        if (arguments.size() > 1 && (arguments[1] == "--seed-vault" || arguments[1] == "--locked-vault"))
            return vaultMode(arguments);
        testing::InitGoogleTest(&argc, argv);
        return RUN_ALL_TESTS();
    } catch (const std::exception&) {
        return 10;
    }
}
