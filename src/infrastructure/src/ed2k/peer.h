#pragma once
#include "connection.h"
#include "messages.h"
#include <chrono>

namespace maskpilot::ed2k {
struct CompletedPart {
    std::size_t index = 0;
    std::shared_ptr<void> memoryLease;
    Bytes bytes;
};
class Peer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Peer(Ed2kEndpoint endpoint, const DownloadSpec& route, Ed2kFileLink link, const Hash& identity,
         std::shared_ptr<void> endpointLease);
    ~Peer();
    std::size_t tick(TransferControl& control, bool receive, std::uint64_t& payloadBytes,
                     std::size_t readLimit = 16 * 1024);
    bool ready() const;
    bool queued() const;
    std::uint32_t rank() const;
    std::string status() const;
    bool hasPart(std::size_t index) const;
    std::optional<std::size_t> assigned() const;
    std::uint64_t received() const;
    void assign(std::size_t index, std::size_t size, std::shared_ptr<void> memoryLease);
    std::optional<CompletedPart> takePart();
    std::optional<std::vector<Hash>> takeHashes();
};
} // namespace maskpilot::ed2k
