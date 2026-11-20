#include "../../src/infrastructure/src/process/owned_process.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <array>
#include <atomic>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <libsecret/secret.h>
#include <maskpilot/application/completion_scan.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/credential_vault.h>
#include <maskpilot/infrastructure/torrent_engine.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace maskpilot;
namespace fs = std::filesystem;
namespace {
const ProxyCredentials secrets{"owned-linux-user", "owned-linux-vault-secret-中文-!42"};
std::string id() { return QUuid::createUuid().toString(QUuid::Id128).toStdString(); }
fs::path path(const QString& value) { return fs::path(value.toStdU16String()); }
std::string utf8(const fs::path& value) {
    const auto bytes = value.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
template <class F> void failure(CredentialVaultFailure expected, F action) {
    try {
        action();
        FAIL() << "Unexpected credential success";
    } catch (const CredentialVaultError& error) {
        EXPECT_EQ(error.failure(), expected);
    }
}
TEST(LinuxHash, IndependentVectorsUnicodeConcurrentCancellationAndReadFailure) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = path(temporary.path()) / u"目录";
    fs::create_directory(root);
    const auto payload = root / u"校验.bin";
    {
        std::ofstream out(payload, std::ios::binary);
        out << "abc";
    }
    const auto empty = root / "empty";
    std::ofstream(empty).close();
    std::atomic_int errors{0};
    {
        std::vector<std::jthread> workers;
        for (int n = 0; n < 8; ++n)
            workers.emplace_back([&] {
                for (int i = 0; i < 32; ++i) {
                    if (sha256File(payload) !=
                        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
                        ++errors;
                    if (sha256File(empty) !=
                        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
                        ++errors;
                }
            });
    }
    EXPECT_EQ(errors.load(), 0);
    TransferControl stop;
    stop.cancel = true;
    EXPECT_THROW(sha256File(payload, &stop), std::runtime_error);
    EXPECT_THROW(sha256File(root / "missing"), std::runtime_error);
    EXPECT_THROW(sha256File(payload, nullptr, [] { return true; }), std::runtime_error);
    EXPECT_EQ(sha256File(payload), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
TEST(LinuxTorrent, RealLocalUnicodeMetadataAndUnsafeSources) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = path(temporary.path());
    const auto source = root / u"种子.torrent";
    const auto digest = QCryptographicHash::hash("abc", QCryptographicHash::Sha1);
    const std::string metadata =
        "d4:infod6:lengthi3e4:name7:abc.bin12:piece lengthi16384e6:pieces20:" + digest.toStdString() + "ee";
    {
        std::ofstream out(source, std::ios::binary);
        out << metadata;
    }
    DownloadSpec spec;
    spec.kind = TransferKind::BitTorrent;
    spec.url = "torrent:" + utf8(source);
    spec.destination = root / "output";
    const auto description = describeTorrent(spec);
    ASSERT_TRUE(description.files);
    ASSERT_EQ(description.files->size(), 1U);
    EXPECT_EQ((*description.files)[0].size, 3);
    const auto link = root / "link.torrent";
    fs::create_symlink(source, link);
    spec.url = "torrent:" + utf8(link);
    EXPECT_THROW(describeTorrent(spec), std::invalid_argument);
    fs::create_directory_symlink(root, root / "linked-directory");
    spec.url = "torrent:" + utf8(root / "linked-directory" / source.filename());
    EXPECT_THROW(describeTorrent(spec), std::invalid_argument);
    const auto fifo = root / "pipe";
    ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
    spec.url = "torrent:" + utf8(fifo);
    EXPECT_THROW(describeTorrent(spec), std::invalid_argument);
    for (const auto* invalid :
         {"torrent://remote/share/file.torrent", "torrent:/proc/self/status", "torrent:relative.torrent"}) {
        spec.url = invalid;
        EXPECT_THROW(describeTorrent(spec), std::invalid_argument) << invalid;
    }
}
TEST(LinuxScanner, ActualUnicodeExecutableAndInputAreValidatedAndScanned) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = path(temporary.path());
    const auto program = root / u"扫描程序";
    const auto input = root / u"待扫描.bin";
    fs::copy_file(path(QCoreApplication::applicationFilePath()), program);
    fs::permissions(program, fs::perms::owner_all);
    {
        std::ofstream output(input);
        output << "owned scan bytes";
    }
    CompletionScanOptions options;
    options.enabled = true;
    options.program = program;
    options.arguments = {"--scan-pass", "{file}"};
    EXPECT_TRUE(validateCompletionScanOptions(options).empty());
    TransferControl control;
    const auto result = makeCompletionScanner(options)->scan(input, control);
    EXPECT_EQ(result.outcome, TransferOutcome::Complete) << result.error;
    EXPECT_EQ(result.state, ScanState::Passed);
    options.program = root / u"invalid\nscanner";
    EXPECT_FALSE(validateCompletionScanOptions(options).empty());
}
TEST(LinuxVault, ActualServiceBindingsReopenAndExactRemoval) {
    const auto installation = id();
    auto vault = makeNativeCredentialVault(installation);
    ProxyCredentialBinding binding{"standalone", "http://proxy.example:8080", id()};
    const auto reference = vault->remember(binding, secrets);
    auto reopened = makeNativeCredentialVault(installation);
    const auto value = reopened->recall(binding, reference);
    ASSERT_TRUE(value);
    EXPECT_EQ(value->username, secrets.username);
    EXPECT_EQ(value->password, secrets.password);
    for (int n = 0; n < 3; ++n) {
        auto wrong = binding;
        if (n == 0)
            wrong.ownerId = id();
        if (n == 1)
            wrong.proxyEndpoint = "socks5://proxy.example:1080";
        if (n == 2)
            wrong.ownershipNonce = id();
        failure(CredentialVaultFailure::Binding, [&] { reopened->recall(wrong, reference); });
        failure(CredentialVaultFailure::Binding, [&] { reopened->forget(wrong, reference); });
    }
    EXPECT_FALSE(makeNativeCredentialVault(id())->recall(binding, reference));
    EXPECT_TRUE(vault->forget(binding, reference));
    EXPECT_FALSE(reopened->recall(binding, reference));
    EXPECT_FALSE(reopened->forget(binding, reference));
}
TEST(LinuxVault, ConcurrentCreateOnlyAndInvalidInput) {
    const auto installation = id(), reference = id();
    ProxyCredentialBinding binding{"standalone", "http://proxy.example:8080", id()};
    std::atomic_int written{0}, duplicate{0}, unexpected{0};
    {
        std::vector<std::jthread> threads;
        for (int n = 0; n < 8; ++n)
            threads.emplace_back([&] {
                try {
                    makeNativeCredentialVault(installation)->store(reference, binding, secrets);
                    ++written;
                } catch (const CredentialVaultError& error) {
                    if (error.failure() == CredentialVaultFailure::AlreadyExists)
                        ++duplicate;
                    else
                        ++unexpected;
                }
            });
    }
    EXPECT_EQ(written.load(), 1);
    EXPECT_EQ(duplicate.load(), 7);
    EXPECT_EQ(unexpected.load(), 0);
    auto vault = makeNativeCredentialVault(installation);
    ASSERT_TRUE(vault->recall(binding, reference));
    failure(CredentialVaultFailure::InvalidInput, [&] { vault->store("../invalid", binding, secrets); });
    failure(CredentialVaultFailure::InvalidInput,
            [&] { vault->remember(binding, {"user", std::string(3000, 'x')}); });
    EXPECT_TRUE(vault->forget(binding, reference));
}
TEST(LinuxVault, CorruptNativeEntryIsNeverReadOrRemovedAsOwned) {
    const auto installation = id(), reference = id();
    const SecretSchema schema = {"org.maskpilot.DownloadManager.Credentials.v1",
                                 SECRET_SCHEMA_NONE,
                                 {{"installation", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                  {"reference", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                  {nullptr, {}}}};
    GError* error = nullptr;
    ASSERT_TRUE(secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, "Owned corruption fixture",
                                           "{bad-json", nullptr, &error, "installation", installation.c_str(),
                                           "reference", reference.c_str(), nullptr));
    ASSERT_EQ(error, nullptr);
    auto vault = makeNativeCredentialVault(installation);
    ProxyCredentialBinding binding{"standalone", "http://proxy.example:8080", id()};
    failure(CredentialVaultFailure::Corrupt, [&] { vault->recall(binding, reference); });
    failure(CredentialVaultFailure::Corrupt, [&] { vault->forget(binding, reference); });
    EXPECT_TRUE(secret_password_clear_sync(&schema, nullptr, &error, "installation", installation.c_str(),
                                           "reference", reference.c_str(), nullptr));
    EXPECT_EQ(error, nullptr);
}
void processCase(bool finishNormally) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto marker = path(temporary.path()) / u"后代.pid";
    QProcess child;
    process::OwnedProcessGroup group;
    group.prepare(child);
    child.start(QCoreApplication::applicationFilePath(),
                {finishNormally ? "--owned-group-finish" : "--owned-group-wait",
                 QString::fromStdU16String(marker.u16string())});
    ASSERT_TRUE(child.waitForStarted(5000));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fs::exists(marker) && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    ASSERT_TRUE(fs::exists(marker));
    pid_t pid = 0;
    {
        std::ifstream input(marker);
        input >> pid;
    }
    ASSERT_GT(pid, 1);
    const auto descriptor = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    ASSERT_GE(descriptor, 0);
    pollfd state{descriptor, POLLIN, 0};
    EXPECT_EQ(poll(&state, 1, 0), 0);
    if (finishNormally) {
        child.write("x");
        child.closeWriteChannel();
        EXPECT_TRUE(child.waitForFinished(5000));
        EXPECT_EQ(child.exitCode(), 0);
    } else {
        group.stop(child);
        EXPECT_EQ(child.state(), QProcess::NotRunning);
    }
    EXPECT_GT(poll(&state, 1, 3000), 0) << "Owned descendant survived group cleanup";
    close(descriptor);
}
TEST(LinuxProcesses, CancelKillsActualDescendant) { processCase(false); }
TEST(LinuxProcesses, SuccessfulLeaderCannotLeaveActualDescendant) { processCase(true); }
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() == 3 && arguments[1] == "--scan-pass")
        return fs::is_regular_file(path(arguments[2])) ? 0 : 16;
    if (arguments.size() == 3 && arguments[1].startsWith("--owned-group-")) {
        const auto marker = path(arguments[2]);
        const auto pid = fork();
        if (pid < 0)
            return 11;
        if (pid == 0) {
            {
                std::ofstream output(marker);
                output << getpid();
            }
            for (;;)
                pause();
        }
        if (arguments[1] == "--owned-group-finish") {
            std::cin.get();
            return 0;
        }
        for (;;)
            pause();
    }
    if (arguments.size() == 2 && arguments[1] == "--no-service") {
        try {
            makeNativeCredentialVault(id())->remember({"standalone", "http://proxy.example"}, secrets);
        } catch (const CredentialVaultError& error) {
            return error.failure() == CredentialVaultFailure::Unavailable ? 0 : 12;
        }
        return 13;
    }
    if (arguments.size() == 5 && arguments[1].startsWith("--vault-")) {
        auto vault = makeNativeCredentialVault(arguments[2].toStdString());
        const auto reference = arguments[3].toStdString();
        const ProxyCredentialBinding binding{"standalone", "http://proxy.example:8080",
                                             arguments[4].toStdString()};
        if (arguments[1] == "--vault-store")
            vault->store(reference, binding, secrets);
        else {
            const auto value = vault->recall(binding, reference);
            if (!value || value->password != secrets.password || value->username != secrets.username)
                return 14;
            if (!vault->forget(binding, reference))
                return 15;
        }
        return 0;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
