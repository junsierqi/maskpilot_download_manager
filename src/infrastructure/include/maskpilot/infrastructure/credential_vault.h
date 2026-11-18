#pragma once
#include <maskpilot/application/credential_vault.h>
#include <memory>

namespace maskpilot {
// installationId is a persistent random 32-character lowercase hexadecimal ID.
// This adapter never stores credentials in ordinary files or enumerates other entries.
std::unique_ptr<ICredentialVault> makeNativeCredentialVault(const std::string& installationId);
} // namespace maskpilot
