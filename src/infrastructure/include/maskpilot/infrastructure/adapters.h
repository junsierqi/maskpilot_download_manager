#pragma once
#include <maskpilot/application/download_context.h>
#include <maskpilot/application/ports.h>
#include <maskpilot/application/queue.h>
#include <maskpilot/application/site_content.h>

namespace maskpilot {
struct SqliteStore {
    std::unique_ptr<ITaskRepository> tasks;
    std::unique_ptr<IContextRepository> contexts;
    std::unique_ptr<IQueueRepository> queues;
    std::shared_ptr<IQueueTrafficRepository> traffic;
};
SqliteStore makeSqliteStore(const std::filesystem::path& path);
std::unique_ptr<ISiteContentProcessor> makeSiteContentProcessor();
std::shared_ptr<IQueueCivilTime> makeQueueCivilTime();
std::unique_ptr<ITaskRepository> makeSqliteRepository(const std::filesystem::path& path);
std::unique_ptr<ITransferTransport> makeCurlTransfer();
std::unique_ptr<ITransferTransport> makeFtpTransfer();
std::unique_ptr<ITransferTransport> makeUnifiedTransfer();
// Keep one factory per service so all BT workers share its lazily created session.
TransportFactory makeUnifiedTransferFactory(TransportFactory torrent = {});
std::string sha256File(const std::filesystem::path& path, const TransferControl* control = nullptr);
} // namespace maskpilot
