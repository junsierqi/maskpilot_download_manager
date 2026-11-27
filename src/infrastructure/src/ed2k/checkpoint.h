#pragma once
#include "integrity.h"
#include "protocol.h"
#include <maskpilot/application/ports.h>
#include <maskpilot/domain/ed2k.h>
#include <memory>

namespace maskpilot::ed2k {
class Checkpoint {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Checkpoint(const Task& task, const std::filesystem::path& work, Ed2kFileLink link,
               TransferControl& control);
    ~Checkpoint();
    const Ed2kFileLink& link() const;
    const Hash& identity() const;
    const std::vector<Hash>& hashes() const;
    void setHashes(std::vector<Hash> hashes);
    std::size_t partCount() const;
    std::size_t partSize(std::size_t index) const;
    bool verified(std::size_t index) const;
    std::uint64_t verifiedBytes() const;
    void commitPart(std::size_t index, std::span<const std::uint8_t> bytes);
    Bytes readPart(std::size_t index) const;
    std::filesystem::path assembleStage();
    void cleanupStage() noexcept;
    static void discard(const Task& task, const std::filesystem::path& work);
};
} // namespace maskpilot::ed2k
