#include "macos_keychain.h"
#include <CoreFoundation/CoreFoundation.h>
#include <QDir>
#include <QStandardPaths>
#include <Security/Security.h>
#include <algorithm>
#include <cerrno>
#include <maskpilot/infrastructure/credential_vault.h>
#include <mutex>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

namespace maskpilot::security {
namespace {
constexpr qsizetype limit = 2560;
struct OwnedCF {
    CFTypeRef value;
    ~OwnedCF() {
        if (value)
            CFRelease(value);
    }
    OwnedCF(const OwnedCF&) = delete;
    explicit OwnedCF(CFTypeRef value) : value(value) {}
};
[[noreturn]] void failure(OSStatus status) {
    if (status == errSecDuplicateItem)
        throw CredentialVaultError(CredentialVaultFailure::AlreadyExists,
                                   "Credential reference already exists");
    if (status == errSecAuthFailed)
        throw CredentialVaultError(CredentialVaultFailure::AccessDenied, "Keychain access denied");
    if (status == errSecInteractionNotAllowed || status == errSecNotAvailable || status == errSecUserCanceled)
        throw CredentialVaultError(CredentialVaultFailure::Unavailable,
                                   "Keychain is locked or unavailable; unlock it explicitly before retrying");
    throw CredentialVaultError(CredentialVaultFailure::Storage, "Keychain operation failed");
}
std::mutex keychainInteractionMutex;
template <class Function> OSStatus unattended(Function operation) {
    static_assert(std::is_nothrow_invocable_r_v<OSStatus, Function>);
    // The legacy file-Keychain shim can request UI independently of SecItem's
    // query option. Serialize the process setting and restore its exact value.
    std::lock_guard lock(keychainInteractionMutex);
    Boolean original = false;
    auto status = SecKeychainGetUserInteractionAllowed(&original);
    if (status != errSecSuccess)
        failure(status);
    status = SecKeychainSetUserInteractionAllowed(false);
    if (status != errSecSuccess)
        failure(status);
    const auto result = operation(); // Only nonthrowing framework calls run here.
    status = SecKeychainSetUserInteractionAllowed(original);
    if (status != errSecSuccess)
        failure(status);
    return result;
}
void id(const std::string& value) {
    if (value.size() != 32 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw CredentialVaultError(CredentialVaultFailure::InvalidInput, "Invalid keychain reference");
}
CFStringRef string(const std::string& value) {
    auto result = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(value.data()),
                                          static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8, false);
    if (!result)
        throw std::bad_alloc();
    return result;
}
CFMutableDictionaryRef query(const std::string& installation, const std::string& reference) {
    id(installation);
    id(reference);
    auto dictionary = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    if (!dictionary)
        throw std::bad_alloc();
    // Allocate strings before transferring dictionary ownership to the caller.
    CFStringRef serviceValue = nullptr, accountValue = nullptr;
    try {
        serviceValue = string("MaskPilot.DownloadManager/v1/" + installation);
        accountValue = string(reference);
    } catch (...) {
        if (serviceValue)
            CFRelease(serviceValue);
        CFRelease(dictionary);
        throw;
    }
    const OwnedCF service(serviceValue);
    const OwnedCF account(accountValue);
    CFDictionarySetValue(dictionary, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(dictionary, kSecAttrService, service.value);
    CFDictionarySetValue(dictionary, kSecAttrAccount, account.value);
    CFDictionarySetValue(dictionary, kSecAttrSynchronizable, kCFBooleanFalse);
    CFDictionarySetValue(dictionary, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
    return dictionary;
}
QString lockPath(const std::string& installation, const std::string& reference) {
    id(installation);
    id(reference);
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (cache.isEmpty())
        throw CredentialVaultError(CredentialVaultFailure::Unavailable,
                                   "Private credential lock directory is unavailable");
    const auto directory = QDir(cache).filePath("maskpilot-vault");
    const auto bytes = directory.toUtf8();
    if (mkdir(bytes.constData(), 0700) != 0 && errno != EEXIST)
        throw CredentialVaultError(CredentialVaultFailure::Storage,
                                   "Cannot create private credential lock directory");
    struct stat state{};
    if (lstat(bytes.constData(), &state) != 0 || !S_ISDIR(state.st_mode) || state.st_uid != getuid() ||
        (state.st_mode & 077))
        throw CredentialVaultError(CredentialVaultFailure::Storage,
                                   "Credential lock directory has unsafe ownership");
    return QDir(directory).filePath(QString::fromStdString(installation + "-" + reference + ".lock"));
}
} // namespace
MacKeychainLock::MacKeychainLock(const std::string& installation, const std::string& reference)
    : lock_(lockPath(installation, reference)) {
    if (!lock_.tryLock(5000))
        throw CredentialVaultError(CredentialVaultFailure::Storage, "Credential reference is busy");
}
void addKeychain(const std::string& installation, const std::string& reference, const QByteArray& bytes) {
    if (bytes.isEmpty() || bytes.size() > limit)
        throw CredentialVaultError(CredentialVaultFailure::InvalidInput,
                                   "Credential entry exceeds its bound");
    const auto dictionary = query(installation, reference);
    const OwnedCF owner(dictionary);
    const OwnedCF data(
        CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.constData()), bytes.size()));
    if (!data.value)
        throw std::bad_alloc();
    CFDictionarySetValue(dictionary, kSecValueData, data.value);
    const auto status = unattended([&]() noexcept { return SecItemAdd(dictionary, nullptr); });
    if (status != errSecSuccess)
        failure(status);
}
std::optional<KeychainEntry> readKeychain(const std::string& installation, const std::string& reference) {
    const auto dictionary = query(installation, reference);
    const OwnedCF owner(dictionary);
    CFDictionarySetValue(dictionary, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(dictionary, kSecReturnRef, kCFBooleanTrue);
    CFDictionarySetValue(dictionary, kSecMatchLimit, kSecMatchLimitOne);
    OwnedCF returned(nullptr);
    const auto status =
        unattended([&]() noexcept { return SecItemCopyMatching(dictionary, &returned.value); });
    const auto result = returned.value;
    if (status == errSecItemNotFound)
        return {};
    if (status != errSecSuccess)
        failure(status);
    if (!result || CFGetTypeID(result) != CFDictionaryGetTypeID())
        throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid keychain entry");
    const auto values = static_cast<CFDictionaryRef>(result);
    const auto data = static_cast<CFDataRef>(CFDictionaryGetValue(values, kSecValueData));
    const auto identity = CFDictionaryGetValue(values, kSecValueRef);
    if (!data || CFGetTypeID(data) != CFDataGetTypeID() || CFDataGetLength(data) <= 0 ||
        CFDataGetLength(data) > limit || !identity)
        throw CredentialVaultError(CredentialVaultFailure::Corrupt, "Invalid keychain entry");
    CFRetain(identity);
    const auto retained = std::shared_ptr<const void>(identity, [](const void* value) { CFRelease(value); });
    return KeychainEntry{
        QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), CFDataGetLength(data)), retained};
}
bool removeKeychain(const KeychainEntry& entry) {
    if (!entry.identity)
        throw CredentialVaultError(CredentialVaultFailure::InvalidInput, "Invalid keychain item identity");
    const auto dictionary = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                      &kCFTypeDictionaryValueCallBacks);
    if (!dictionary)
        throw std::bad_alloc();
    const OwnedCF owner(dictionary);
    const void* identity = entry.identity.get();
    const OwnedCF items(CFArrayCreate(nullptr, &identity, 1, &kCFTypeArrayCallBacks));
    if (!items.value)
        throw std::bad_alloc();
    CFDictionarySetValue(dictionary, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(dictionary, kSecMatchItemList, items.value);
    CFDictionarySetValue(dictionary, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
    const auto status = unattended([&]() noexcept { return SecItemDelete(dictionary); });
    if (status == errSecItemNotFound)
        return false;
    if (status != errSecSuccess)
        failure(status);
    return true;
}
} // namespace maskpilot::security
