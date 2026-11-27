#pragma once
#include "protocol.h"
#include <maskpilot/application/ports.h>
#include <maskpilot/domain/ed2k.h>
#include <memory>

namespace maskpilot::ed2k {
// Worker-owned socket. No Qt event loop, inherited proxy policy or cross-task state.
class Connection {
    struct Impl;
    std::shared_ptr<void> endpointLease_;
    std::unique_ptr<Impl> impl_;

  public:
    Connection(const Ed2kEndpoint& endpoint, const DownloadSpec& route,
               std::shared_ptr<void> endpointLease = {});
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    bool connected() const;
    // Each tick is nonblocking and has bounded receive/send work.
    std::size_t tick(bool receive = true, std::size_t readLimit = 16 * 1024);
    void send(const Frame& frame);
    std::optional<Frame> next();
};
} // namespace maskpilot::ed2k
