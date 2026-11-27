#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/ed2k_engine.h>
#include <maskpilot/infrastructure/media_tools.h>
#include <maskpilot/infrastructure/torrent_engine.h>

namespace maskpilot {
namespace {
class UnifiedTransfer final : public ITransferTransport {
    TransportFactory torrent_;
    std::shared_ptr<Ed2kResources> ed2k_;

  public:
    UnifiedTransfer(TransportFactory torrent, std::shared_ptr<Ed2kResources> ed2k)
        : torrent_(std::move(torrent)), ed2k_(std::move(ed2k)) {}
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
        else if (sourceKind(task.spec) == TransferKind::Ed2k)
            makeEd2kTransfer(task.spec.ed2k, ed2k_)->discard(task, work);
    }
    void run(const Task& task, const std::filesystem::path& work, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        std::unique_ptr<ITransferTransport> adapter;
        switch (sourceKind(task.spec)) {
        case TransferKind::Ed2k:
            adapter = makeEd2kTransfer(task.spec.ed2k, ed2k_);
            break;
        case TransferKind::BitTorrent:
            adapter = torrent_();
            break;
        case TransferKind::Media:
            adapter = makeMediaTransfer();
            break;
        case TransferKind::Live:
            adapter = makeLiveTransfer();
            break;
        case TransferKind::Sftp:
            adapter = makeSftpTransfer();
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
    auto ed2k = makeEd2kResources();
    return [torrent = std::move(torrent), ed2k = std::move(ed2k)] {
        return std::make_unique<UnifiedTransfer>(torrent, ed2k);
    };
}
std::unique_ptr<ITransferTransport> makeUnifiedTransfer() { return makeUnifiedTransferFactory()(); }
} // namespace maskpilot
