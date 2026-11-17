#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/media_tools.h>
#include <maskpilot/infrastructure/torrent_engine.h>

namespace maskpilot {
namespace {
class UnifiedTransfer final : public ITransferTransport {
    TransportFactory torrent_;

  public:
    explicit UnifiedTransfer(TransportFactory torrent) : torrent_(std::move(torrent)) {}
    TransferDescription describe(const DownloadSpec& spec) override {
        return sourceKind(spec) == TransferKind::BitTorrent ? torrent_()->describe(spec)
                                                            : TransferDescription{};
    }
    TorrentMetadata captureMetadata(const DownloadSpec& spec) override {
        return sourceKind(spec) == TransferKind::BitTorrent ? torrent_()->captureMetadata(spec)
                                                            : spec.torrent.metadata;
    }
    void discard(const Task& task, const std::filesystem::path& work) override {
        if (sourceKind(task.spec) == TransferKind::BitTorrent)
            torrent_()->discard(task, work);
    }
    void run(const Task& task, const std::filesystem::path& work, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        std::unique_ptr<ITransferTransport> adapter;
        switch (sourceKind(task.spec)) {
        case TransferKind::BitTorrent:
            adapter = torrent_();
            break;
        case TransferKind::Media:
            adapter = makeMediaTransfer();
            break;
        case TransferKind::Ftp:
            adapter = makeFtpTransfer();
            break;
        default:
            adapter = makeCurlTransfer();
            break;
        }
        adapter->run(task, work, control, callback);
    }
};
} // namespace
TransportFactory makeUnifiedTransferFactory(TransportFactory torrent) {
    if (!torrent)
        torrent = makeTorrentTransferFactory();
    return [torrent = std::move(torrent)] { return std::make_unique<UnifiedTransfer>(torrent); };
}
std::unique_ptr<ITransferTransport> makeUnifiedTransfer() { return makeUnifiedTransferFactory()(); }
} // namespace maskpilot
