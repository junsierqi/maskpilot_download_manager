#pragma once
#include <maskpilot/application/ports.h>

namespace maskpilot {
struct TorrentRuntimeOptions {
    bool enableDiscovery = true;
    std::string listenInterfaces = "0.0.0.0:0,[::]:0";
};
// One lazy session and one alert dispatcher are shared by all transports from this factory.
TransportFactory makeTorrentTransferFactory(TorrentRuntimeOptions options = {});
// Deterministic local parser; no session, sockets or library types escape this adapter.
TransferDescription describeTorrent(const DownloadSpec& spec);
} // namespace maskpilot
