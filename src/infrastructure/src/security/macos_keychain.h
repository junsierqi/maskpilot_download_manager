#pragma once
#include <QByteArray>
#include <QLockFile>
#include <memory>
#include <optional>
#include <string>

namespace maskpilot::security {
struct KeychainEntry {
    QByteArray bytes;
    std::shared_ptr<const void> identity;
};
class MacKeychainLock {
    QLockFile lock_;

  public:
    MacKeychainLock(const std::string& installation, const std::string& reference);
};
void addKeychain(const std::string& installation, const std::string& reference, const QByteArray&);
std::optional<KeychainEntry> readKeychain(const std::string& installation, const std::string& reference);
bool removeKeychain(const KeychainEntry&);
} // namespace maskpilot::security
