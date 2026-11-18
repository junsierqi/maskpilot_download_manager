#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUuid>
#include <functional>
#include <gtest/gtest.h>
#include <maskpilot/infrastructure/credential_vault.h>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#include <wincred.h>
#endif

using namespace maskpilot;
namespace {
std::string randomId() { return QUuid::createUuid().toString(QUuid::Id128).toStdString(); }
void expectFailure(CredentialVaultFailure expected, const std::function<void()>& operation) {
    try {
        operation();
        FAIL() << "Expected a typed vault failure";
    } catch (const CredentialVaultError& error) {
        EXPECT_EQ(error.failure(), expected);
        EXPECT_EQ(std::string(error.what()).find("fixture-password"), std::string::npos);
    }
}
#ifdef _WIN32
struct OwnedNativeCleanup {
    // Construct only after this fixture created the exact target successfully.
    std::wstring target;
    ~OwnedNativeCleanup() {
        if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
            EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_NOT_FOUND))
                << "Exact owned corruption fixture cleanup failed";
    }
};
class CredentialVault : public ::testing::Test {
  protected:
    std::string installation = randomId();
    std::unique_ptr<ICredentialVault> vault = makeNativeCredentialVault(installation);
    ProxyCredentialBinding binding{"standalone", "http://proxy.example:8080"};
    ProxyCredentials secrets{"fixture-user", "fixture-password"};
    std::vector<std::pair<ProxyCredentialBinding, std::string>> entries;
    std::string remember(const ProxyCredentialBinding& owner, const ProxyCredentials& credentials) {
        auto reference = vault->remember(owner, credentials);
        entries.emplace_back(owner, reference);
        return reference;
    }
    void TearDown() override {
        for (const auto& [owner, reference] : entries) {
            try {
                vault->forget(owner, reference);
            } catch (const std::exception&) {
                ADD_FAILURE() << "Owned vault fixture cleanup failed";
            }
        }
    }
};
TEST_F(CredentialVault, NativeRoundTripReopenAndExactDeletion) {
    const auto reference = remember(binding, secrets);
    EXPECT_EQ(reference.size(), 32u);
    auto reopened = makeNativeCredentialVault(installation);
    auto restored = reopened->recall(binding, reference);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->username, secrets.username);
    EXPECT_EQ(restored->password, secrets.password);
    EXPECT_TRUE(reopened->forget(binding, reference));
    EXPECT_FALSE(vault->recall(binding, reference));
    EXPECT_FALSE(vault->forget(binding, reference));
}
TEST_F(CredentialVault, UnicodeAndCanonicalRoutePreserveSecrets) {
    const ProxyCredentials unicode{"用户-fixture", "密码-🔐-fixture"};
    const ProxyCredentialBinding first{randomId(), "SOCKS5://Proxy.Example"};
    const auto reference = remember(first, unicode);
    const ProxyCredentialBinding equivalent{first.ownerId, "socks5h://proxy.example:1080"};
    const auto restored = vault->recall(equivalent, reference);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->username, unicode.username);
    EXPECT_EQ(restored->password, unicode.password);
}
TEST_F(CredentialVault, OwnerRouteAndInstallationAreBound) {
    const auto reference = remember(binding, secrets);
    auto wrongOwner = binding;
    wrongOwner.ownerId = randomId();
    auto wrongRoute = binding;
    wrongRoute.proxyEndpoint = "https://proxy.example:8080";
    for (const auto& mismatch : {wrongOwner, wrongRoute}) {
        expectFailure(CredentialVaultFailure::Binding, [&] { vault->recall(mismatch, reference); });
        expectFailure(CredentialVaultFailure::Binding, [&] { vault->forget(mismatch, reference); });
    }
    auto unrelatedInstallation = makeNativeCredentialVault(randomId());
    EXPECT_FALSE(unrelatedInstallation->recall(binding, reference));
    EXPECT_FALSE(unrelatedInstallation->forget(binding, reference));
    ASSERT_TRUE(vault->recall(binding, reference));
}
TEST_F(CredentialVault, ExactNativeBlobLimitIsEnforcedBeforeWrite) {
    const auto probe = remember(binding, {"fixture-user", "x"});
    auto target =
        QString::fromStdString("MaskPilot.DownloadManager/v1/" + installation + "/" + probe).toStdWString();
    PCREDENTIALW native = nullptr;
    ASSERT_TRUE(CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &native));
    const auto overhead = native->CredentialBlobSize - 1;
    CredFree(native);
    ASSERT_LT(overhead, static_cast<DWORD>(CRED_MAX_CREDENTIAL_BLOB_SIZE));
    const auto length = CRED_MAX_CREDENTIAL_BLOB_SIZE - overhead;
    const ProxyCredentials exact{"fixture-user", std::string(length, 'x')};
    const auto reference = remember(binding, exact);
    const auto restored = vault->recall(binding, reference);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->password.size(), length);
    expectFailure(CredentialVaultFailure::InvalidInput,
                  [&] { vault->remember(binding, {"fixture-user", std::string(length + 1, 'x')}); });
}
TEST_F(CredentialVault, InvalidInputAndOversizedBlobNeverWrite) {
    expectFailure(CredentialVaultFailure::InvalidInput,
                  [&] { makeNativeCredentialVault("../invalid-installation"); });
    for (const auto& reference : {std::string{}, std::string("../other"), std::string(32, 'G')}) {
        expectFailure(CredentialVaultFailure::InvalidInput,
                      [&] { vault->store(reference, binding, secrets); });
        expectFailure(CredentialVaultFailure::InvalidInput, [&] { vault->recall(binding, reference); });
        expectFailure(CredentialVaultFailure::InvalidInput, [&] { vault->forget(binding, reference); });
    }
    for (const auto& bad :
         {ProxyCredentialBinding{"../owner", binding.proxyEndpoint}, ProxyCredentialBinding{"standalone", ""},
          ProxyCredentialBinding{"standalone", "http://user:fixture-password@proxy.example"}}) {
        expectFailure(CredentialVaultFailure::InvalidInput, [&] { vault->remember(bad, secrets); });
    }
    expectFailure(CredentialVaultFailure::InvalidInput,
                  [&] { vault->remember(binding, {"fixture-user", std::string(4096, 'x')}); });
    expectFailure(CredentialVaultFailure::InvalidInput,
                  [&] { vault->remember(binding, {"fixture-user", ""}); });
    expectFailure(CredentialVaultFailure::InvalidInput,
                  [&] { vault->remember(binding, {"fixture-user", std::string(1, '\xff')}); });
}
TEST_F(CredentialVault, StoreUsesTheReservedReferenceAndNeverOverwritesAnExistingEntry) {
    const auto reference = randomId();
    EXPECT_FALSE(vault->recall(binding, reference));
    vault->store(reference, binding, secrets);
    entries.emplace_back(binding, reference);
    auto reopened = makeNativeCredentialVault(installation);
    const auto restored = reopened->recall(binding, reference);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->username, secrets.username);
    EXPECT_EQ(restored->password, secrets.password);
    expectFailure(CredentialVaultFailure::AlreadyExists,
                  [&] { reopened->store(reference, binding, {"replacement-user", "replacement-password"}); });
    auto anotherOwner = binding;
    anotherOwner.ownerId = randomId();
    expectFailure(CredentialVaultFailure::AlreadyExists,
                  [&] { reopened->store(reference, anotherOwner, secrets); });
    const auto unchanged = vault->recall(binding, reference);
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->username, secrets.username);
    EXPECT_EQ(unchanged->password, secrets.password);
    const auto absent = randomId();
    EXPECT_FALSE(reopened->forget(binding, absent));
    EXPECT_FALSE(reopened->recall(binding, absent));
}
TEST_F(CredentialVault, OwnershipNonceProtectsOlderEntriesFromFailedWriteCleanup) {
    auto owned = binding;
    owned.ownershipNonce = randomId();
    const auto reference = remember(owned, secrets);
    auto failedIntent = owned;
    failedIntent.ownershipNonce = randomId();
    expectFailure(CredentialVaultFailure::AlreadyExists,
                  [&] { vault->store(reference, failedIntent, {"other-user", "other-password"}); });
    expectFailure(CredentialVaultFailure::Binding, [&] { vault->recall(failedIntent, reference); });
    expectFailure(CredentialVaultFailure::Binding, [&] { vault->forget(failedIntent, reference); });
    expectFailure(CredentialVaultFailure::Binding, [&] { vault->forget(binding, reference); });
    const auto preserved = vault->recall(owned, reference);
    ASSERT_TRUE(preserved);
    EXPECT_EQ(preserved->username, secrets.username);
    EXPECT_EQ(preserved->password, secrets.password);
    auto invalidNonce = binding;
    invalidNonce.ownershipNonce = "../invalid";
    expectFailure(CredentialVaultFailure::InvalidInput,
                  [&] { vault->store(randomId(), invalidNonce, secrets); });
}
TEST_F(CredentialVault, LegacyBlobWithoutNonceRemainsBoundToAnEmptyNonce) {
    const auto reference = remember(binding, secrets);
    auto target = QString::fromStdString("MaskPilot.DownloadManager/v1/" + installation + "/" + reference)
                      .toStdWString();
    PCREDENTIALW native = nullptr;
    ASSERT_TRUE(CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &native));
    auto object = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(native->CredentialBlob),
                                                     static_cast<qsizetype>(native->CredentialBlobSize)))
                      .object();
    CredFree(native);
    object.remove("nonce");
    auto legacy = QJsonDocument(object).toJson(QJsonDocument::Compact);
    CREDENTIALW entry{};
    entry.Type = CRED_TYPE_GENERIC;
    entry.TargetName = target.data();
    entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
    entry.CredentialBlob = reinterpret_cast<LPBYTE>(legacy.data());
    entry.CredentialBlobSize = static_cast<DWORD>(legacy.size());
    ASSERT_TRUE(CredWriteW(&entry, 0));
    ASSERT_TRUE(vault->recall(binding, reference));
    auto newIntent = binding;
    newIntent.ownershipNonce = randomId();
    expectFailure(CredentialVaultFailure::Binding, [&] { vault->forget(newIntent, reference); });
    EXPECT_TRUE(vault->recall(binding, reference));
}
TEST_F(CredentialVault, NativePersistenceAndCorruptionFailClosed) {
    const auto reference = remember(binding, secrets);
    auto target = QString::fromStdString("MaskPilot.DownloadManager/v1/" + installation + "/" + reference)
                      .toStdWString();
    const OwnedNativeCleanup cleanup{target};
    PCREDENTIALW native = nullptr;
    ASSERT_TRUE(CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &native));
    EXPECT_EQ(native->Persist, CRED_PERSIST_LOCAL_MACHINE);
    EXPECT_EQ(native->Type, CRED_TYPE_GENERIC);
    auto original = QByteArray(reinterpret_cast<const char*>(native->CredentialBlob),
                               static_cast<qsizetype>(native->CredentialBlobSize));
    CredFree(native);
    auto corrupt = QByteArray("{broken-json");
    CREDENTIALW entry{};
    entry.Type = CRED_TYPE_GENERIC;
    entry.TargetName = target.data();
    entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
    entry.CredentialBlob = reinterpret_cast<LPBYTE>(corrupt.data());
    entry.CredentialBlobSize = static_cast<DWORD>(corrupt.size());
    ASSERT_TRUE(CredWriteW(&entry, 0));
    expectFailure(CredentialVaultFailure::Corrupt, [&] { vault->recall(binding, reference); });
    expectFailure(CredentialVaultFailure::Corrupt, [&] { vault->forget(binding, reference); });
    entry.CredentialBlob = reinterpret_cast<LPBYTE>(original.data());
    entry.CredentialBlobSize = static_cast<DWORD>(original.size());
    EXPECT_TRUE(CredWriteW(&entry, 0));
}
TEST_F(CredentialVault, ReopenAcrossProcess) {
    const auto environment = QProcessEnvironment::systemEnvironment();
    const auto childInstallation = environment.value("MASKPILOT_TEST_VAULT_INSTALLATION");
    if (!childInstallation.isEmpty()) {
        auto child = makeNativeCredentialVault(childInstallation.toStdString());
        auto restored =
            child->recall(binding, environment.value("MASKPILOT_TEST_VAULT_REFERENCE").toStdString());
        ASSERT_TRUE(restored);
        EXPECT_EQ(restored->username, secrets.username);
        EXPECT_EQ(restored->password, secrets.password);
        return;
    }
    const auto reference = remember(binding, secrets);
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, executable.size());
    executable.resize(length);
    auto childEnvironment = environment;
    childEnvironment.insert("MASKPILOT_TEST_VAULT_INSTALLATION", QString::fromStdString(installation));
    childEnvironment.insert("MASKPILOT_TEST_VAULT_REFERENCE", QString::fromStdString(reference));
    QProcess child;
    child.setProcessEnvironment(childEnvironment);
    child.start(QString::fromStdWString(executable), {"--gtest_filter=CredentialVault.ReopenAcrossProcess"});
    ASSERT_TRUE(child.waitForStarted(10000));
    ASSERT_TRUE(child.waitForFinished(20000));
    EXPECT_EQ(child.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(child.exitCode(), 0) << child.readAllStandardOutput().toStdString();
}
#else
TEST(CredentialVault, UnsupportedPlatformFailsExplicitly) {
    auto vault = makeNativeCredentialVault(randomId());
    expectFailure(CredentialVaultFailure::Unavailable,
                  [&] { vault->remember({"standalone", "http://proxy.example"}, {"user", "password"}); });
    expectFailure(CredentialVaultFailure::Unavailable, [&] {
        vault->store(randomId(), {"standalone", "http://proxy.example"}, {"user", "password"});
    });
}
#endif
} // namespace
