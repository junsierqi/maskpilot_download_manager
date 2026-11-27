#pragma once
#include <maskpilot/application/ports.h>
#include <maskpilot/domain/ed2k.h>

namespace maskpilot::ed2k {
// Final stored-file proof, including optional auxiliary tree and caller SHA256.
void verifyStage(const Task& task, const Ed2kFileLink& link, const std::filesystem::path& stage,
                 const TransferControl& control);
} // namespace maskpilot::ed2k
