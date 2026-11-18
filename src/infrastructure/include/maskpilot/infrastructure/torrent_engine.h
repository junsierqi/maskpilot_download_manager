#pragma once
#include <maskpilot/application/ports.h>

namespace maskpilot {
struct TorrentRuntimeOptions {
    bool enableDiscovery = true;
    std::string listenInterfaces = "0.0.0.0:0,[::]:0";
};
// Active transports share a lazy session/dispatcher only for identical immutable
// proxy routes and process-local credentials. Direct sessions remain separate.
TransportFactory makeTorrentTransferFactory(TorrentRuntimeOptions options = {});
// Deterministic local parser; no session, sockets or library types escape this adapter.
TransferDescription describeTorrent(const DownloadSpec& spec);
} // namespace maskpilot
