#include "../filesystem/transfer_files.h"
#include "checkpoint.h"
#include "connection.h"
#include "identity.h"
#include "messages.h"
#include "peer.h"
#include "resources.h"
#include "verification.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <maskpilot/domain/download_context.h>
#include <maskpilot/infrastructure/ed2k_engine.h>
#include <set>
#include <stdexcept>
#include <thread>

namespace maskpilot {
namespace {
namespace wire = ed2k;
using Clock = std::chrono::steady_clock;
struct Source {
    Ed2kEndpoint endpoint;
    Clock::time_point retry{};
};
struct ActivePeer {
    std::size_t source = 0;
    std::string endpointKey;
    std::unique_ptr<wire::Peer> peer;
};
struct Index {
    std::size_t source = 0;
    std::unique_ptr<wire::Connection> connection;
    bool loginSent = false, ready = false;
    Clock::time_point born = Clock::now(), ask{};
};
void addSource(std::vector<Source>& sources, Ed2kEndpoint endpoint, bool explicitInput) {
    std::transform(endpoint.host.begin(), endpoint.host.end(), endpoint.host.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    const auto existing = std::find_if(sources.begin(), sources.end(), [&](const Source& source) {
        return source.endpoint.host == endpoint.host && source.endpoint.port == endpoint.port;
    });
    if (existing != sources.end()) {
        // An encryption requirement can never be weakened by a second source advertisement.
        existing->endpoint.cryptOptions |= endpoint.cryptOptions;
        if (existing->endpoint.clientHash.empty())
            existing->endpoint.clientHash = endpoint.clientHash;
        else if (!endpoint.clientHash.empty() && existing->endpoint.clientHash != endpoint.clientHash)
            throw std::invalid_argument("ED2K source identity advertisements disagree.");
        return;
    }
    if (sources.size() == maxEd2kSources) {
        if (explicitInput)
            throw std::invalid_argument("Combined ED2K sources exceed the supported bound.");
        return;
    }
    sources.push_back({std::move(endpoint), {}});
}
class Ed2kTransfer final : public ITransferTransport {
    Ed2kOptions options_;
    std::shared_ptr<Ed2kResources> resources_;
    std::shared_ptr<void> acquirePart() {
        auto count = resources_->parts.load();
        do {
            if (count >= 8)
                return {};
        } while (!resources_->parts.compare_exchange_weak(count, count + 1));
        return {resources_.get(), [owner = resources_](void*) { owner->parts.fetch_sub(1); }};
    }
    void execute(const Task& task, const std::filesystem::path& work, TransferControl& control,
                 TransferEvent& event, const std::function<void(TransferEvent)>& callback) {
        if (const auto error = validateEd2kOptions(options_); !error.empty())
            throw std::invalid_argument(error);
        if (!isOpaqueId(task.id) || task.ownerId != "standalone" || !task.projectId.empty() ||
            !task.originGrantId.empty() || task.spec.originAuthenticationRequired ||
            hasOriginMaterial(task.spec) || task.spec.maxStoredBytes || task.spec.sameOriginRedirectOnly)
            throw std::invalid_argument(
                "ED2K requires a standalone task without origin credentials or HTTP grants.");
        for (const auto& path : {task.spec.destination, work}) {
            const auto text = path.u8string();
            if (!path.is_absolute() || text.starts_with(u8"//") || text.starts_with(u8"\\\\"))
                throw std::invalid_argument("ED2K requires absolute local destination and recovery paths.");
        }
        auto link = parseEd2kFileLink(task.spec.url);
        event.total = static_cast<std::int64_t>(link.size);
        event.phase = TaskState::Probing;
        callback(event);
        wire::Checkpoint checkpoint(task, work, link, control);
        struct Registration {
            std::shared_ptr<Ed2kResources> resources;
            std::string task;
            Registration(std::shared_ptr<Ed2kResources> value, std::string id)
                : resources(std::move(value)), task(std::move(id)) {
                resources->tasks.fetch_add(1);
            }
            ~Registration() {
                resources->forget(task);
                resources->tasks.fetch_sub(1);
            }
        } registration(resources_, task.id);
        const auto contextKey = wire::sessionKey(task, work);
        struct StageCleanup {
            wire::Checkpoint& checkpoint;
            ~StageCleanup() { checkpoint.cleanupStage(); }
        } stageCleanup{checkpoint};
        event.resumable = true;
        event.downloaded = static_cast<std::int64_t>(checkpoint.verifiedBytes());
        std::vector<Source> sources, servers;
        for (const auto& source : options_.sources)
            addSource(sources, source, true);
        for (const auto& source : link.sources)
            addSource(sources, source, true);
        for (const auto& server : options_.servers)
            addSource(servers, server, true);
        std::vector<ActivePeer> peers;
        std::vector<Index> indexes;
        const auto connections = static_cast<std::size_t>(std::clamp(task.spec.connections, 1, 8));
        auto lastEvent = Clock::now() - std::chrono::seconds(1), lastRate = Clock::now();
        auto tokenAt = Clock::now();
        std::uint64_t traffic = 0, lastTraffic = checkpoint.verifiedBytes();
        double tokens = 0;
        std::int64_t rate = 0;
        std::string status;
        while (checkpoint.verifiedBytes() < link.size) {
            if (control.pause || control.cancel)
                throw std::runtime_error("ED2K transfer stopped.");
            bool receivedThisTurn = false, verifiedThisTurn = false;
            const auto now = Clock::now();
            const auto limit = std::max<std::int64_t>(0, control.speedLimit.load());
            tokens = limit ? std::min<double>(static_cast<double>(limit),
                                              tokens + std::chrono::duration<double>(now - tokenAt).count() *
                                                           static_cast<double>(limit))
                           : 0;
            tokenAt = now;
            const bool usableSource = std::any_of(sources.begin(), sources.end(), [](const Source& source) {
                return !(source.endpoint.cryptOptions & 4);
            });
            if (!usableSource && servers.empty())
                throw std::runtime_error(
                    sources.empty()
                        ? "Configure an ED2K index server or direct peer source for this file link."
                        : "Available ED2K sources require unsupported protocol encryption.");
            for (std::size_t i = 0; i < servers.size() && indexes.size() < 2; ++i) {
                if (servers[i].retry > now ||
                    std::any_of(indexes.begin(), indexes.end(),
                                [&](const Index& index) { return index.source == i; }))
                    continue;
                try {
                    const auto key = contextKey + ":index:" + servers[i].endpoint.host + ':' +
                                     std::to_string(servers[i].endpoint.port);
                    auto lease = resources_->tryPeer(key, task.id, true);
                    if (!lease)
                        continue;
                    Index index;
                    index.source = i;
                    index.connection =
                        std::make_unique<wire::Connection>(servers[i].endpoint, task.spec, std::move(lease));
                    indexes.push_back(std::move(index));
                } catch (const std::exception& error) {
                    status = error.what();
                    servers[i].retry = now + std::chrono::seconds(60);
                }
            }
            for (auto it = indexes.begin(); it != indexes.end();) {
                try {
                    bool answered = false;
                    it->connection->tick();
                    if (it->connection->connected() && !it->loginSent) {
                        it->connection->send(wire::indexLogin(checkpoint.identity()));
                        it->loginSent = true;
                    }
                    for (unsigned count = 0; count < 32; ++count) {
                        auto incoming = it->connection->next();
                        if (!incoming)
                            break;
                        auto frame = wire::unpack(std::move(*incoming));
                        if (frame.opcode == 0x05 || frame.opcode == 0x18)
                            throw std::runtime_error("ED2K index rejected this connection.");
                        if (frame.opcode == 0x40) {
                            wire::Reader reader(frame.payload);
                            if (!reader.u32() || reader.remaining() > 32)
                                throw std::invalid_argument("Invalid ED2K index identity response.");
                            it->ready = true;
                        } else if (frame.opcode == 0x42 || frame.opcode == 0x44) {
                            for (auto source : wire::parseSources(frame, wire::parseHash(link.hash)))
                                addSource(sources, std::move(source), false);
                            answered = true;
                        }
                    }
                    if (answered) {
                        servers[it->source].retry = now + std::chrono::minutes(15);
                        it = indexes.erase(it);
                        continue;
                    }
                    if (!it->ready && now - it->born > std::chrono::seconds(30))
                        throw std::runtime_error("ED2K index login timed out.");
                    if (it->ready && it->ask == Clock::time_point{}) {
                        it->connection->send(wire::sourceRequest(link));
                        it->ask = now;
                    }
                    if (it->ask != Clock::time_point{} && now - it->ask > std::chrono::seconds(15))
                        throw std::runtime_error("ED2K index did not return file sources.");
                    ++it;
                } catch (const std::exception& error) {
                    status = error.what();
                    servers[it->source].retry = now + std::chrono::seconds(60);
                    it = indexes.erase(it);
                }
            }
            for (std::size_t i = 0; i < sources.size() && peers.size() < connections; ++i) {
                if (sources[i].retry > now || (sources[i].endpoint.cryptOptions & 4) ||
                    std::any_of(peers.begin(), peers.end(),
                                [&](const ActivePeer& peer) { return peer.source == i; }))
                    continue;
                try {
                    const auto key = contextKey + ':' + sources[i].endpoint.host + ':' +
                                     std::to_string(sources[i].endpoint.port);
                    auto lease = resources_->tryPeer(key, task.id);
                    if (!lease)
                        continue;
                    peers.push_back({i, key,
                                     std::make_unique<wire::Peer>(sources[i].endpoint, task.spec, link,
                                                                  checkpoint.identity(), std::move(lease))});
                } catch (const std::exception& error) {
                    status = error.what();
                    sources[i].retry = now + std::chrono::seconds(60);
                }
            }
            for (auto it = peers.begin(); it != peers.end();) {
                bool failed = (sources[it->source].endpoint.cryptOptions & 4) != 0;
                bool yield = false;
                try {
                    if (failed)
                        throw std::runtime_error("ED2K source now requires protocol encryption.");
                    const bool data = it->peer->assigned().has_value();
                    const auto allowance =
                        limit && data ? static_cast<std::size_t>(std::min(16384.0, std::floor(tokens)))
                                      : 16384;
                    const auto read = it->peer->tick(control, allowance != 0, traffic, allowance);
                    receivedThisTurn = receivedThisTurn || read != 0;
                    if (limit && data)
                        tokens = std::max(0.0, tokens - static_cast<double>(read));
                } catch (const std::exception& error) {
                    if (control.pause || control.cancel)
                        throw;
                    status = error.what();
                    failed = true;
                }
                if (!failed) {
                    if (auto hashes = it->peer->takeHashes())
                        checkpoint.setHashes(std::move(*hashes));
                    if (auto part = it->peer->takePart()) {
                        try {
                            checkpoint.commitPart(part->index, part->bytes);
                            verifiedThisTurn = true;
                            yield = resources_->peerWaiting(it->endpointKey);
                        } catch (const std::invalid_argument& error) {
                            status = error.what();
                            failed = true;
                        }
                    }
                }
                if (failed) {
                    sources[it->source].retry = now + std::chrono::minutes(15);
                    it = peers.erase(it);
                } else if (yield) {
                    // Close before handing the endpoint to the next waiting file.
                    it = peers.erase(it);
                } else
                    ++it;
            }
            std::set<std::size_t> assigned;
            for (const auto& peer : peers)
                if (const auto part = peer.peer->assigned())
                    assigned.insert(*part);
            if (!checkpoint.hashes().empty())
                for (auto& peer : peers) {
                    const auto share =
                        std::max<std::size_t>(1, 8 / std::max<std::size_t>(1, resources_->tasks.load()));
                    if (assigned.size() >= share)
                        break;
                    if (!peer.peer->ready())
                        continue;
                    for (std::size_t i = 0; i < checkpoint.partCount(); ++i)
                        if (!checkpoint.verified(i) && !assigned.contains(i) && peer.peer->hasPart(i)) {
                            auto permit = acquirePart();
                            if (!permit)
                                break;
                            peer.peer->assign(i, checkpoint.partSize(i), std::move(permit));
                            assigned.insert(i);
                            break;
                        }
                }
            // A verified part is also a control boundary for fast transfers.
            if (verifiedThisTurn || now - lastEvent >= std::chrono::milliseconds(200)) {
                std::uint64_t received = checkpoint.verifiedBytes();
                for (const auto& peer : peers)
                    received += peer.peer->received();
                received = std::min(received, link.size);
                if (now - lastRate >= std::chrono::seconds(1)) {
                    rate = static_cast<std::int64_t>(
                        static_cast<double>(received - std::min(lastTraffic, received)) /
                        std::chrono::duration<double>(now - lastRate).count());
                    lastRate = now;
                    lastTraffic = received;
                }
                event.downloaded = static_cast<std::int64_t>(received);
                event.peers = static_cast<int>(peers.size());
                event.speed = rate;
                event.phase = !assigned.empty()
                                  ? TaskState::Downloading
                                  : (std::any_of(peers.begin(), peers.end(),
                                                 [](const auto& peer) { return peer.peer->queued(); })
                                         ? TaskState::RemoteQueued
                                         : TaskState::SourceWaiting);
                event.error = peers.empty() ? status : peers.front().peer->status();
                callback(event);
                lastEvent = now;
            }
            // Keep draining ready sockets in bounded turns. Sleeping after every
            // read imposed a timer-resolution-dependent cap even without a rate limit.
            if (receivedThisTurn)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        peers.clear();
        indexes.clear();
        event.error.clear();
        event.peers = 0;
        event.speed = 0;
        event.downloaded = static_cast<std::int64_t>(link.size);
        event.phase = TaskState::Verifying;
        callback(event);
        const auto stage = checkpoint.assembleStage();
        wire::verifyStage(task, link, stage, control);
        if (control.scanBeforePublish) {
            const auto scan = control.scanBeforePublish(stage);
            event.scanState = scan.state;
            if (scan.outcome != TransferOutcome::Complete) {
                event.outcome = scan.outcome;
                event.error = scan.error;
                return;
            }
        }
        event.phase = TaskState::Finalizing;
        callback(event);
        if (control.pause || control.cancel)
            throw std::runtime_error("ED2K publication stopped.");
        transfer_files::publishFile(stage, task.spec.destination);
        event.outcome = TransferOutcome::Complete;
    }

  public:
    Ed2kTransfer(Ed2kOptions options, std::shared_ptr<Ed2kResources> resources)
        : options_(std::move(options)), resources_(resources ? std::move(resources) : makeEd2kResources()) {}
    void discard(const Task& task, const std::filesystem::path& work) override {
        wire::Checkpoint::discard(task, work);
    }
    void run(const Task& task, const std::filesystem::path& work, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        TransferEvent event;
        try {
            execute(task, work, control, event, callback);
        } catch (const std::exception& error) {
            event.outcome = control.cancel  ? TransferOutcome::Cancelled
                            : control.pause ? TransferOutcome::Paused
                                            : TransferOutcome::Attention;
            event.error = control.cancel || control.pause ? std::string{} : error.what();
        }
        event.speed = 0;
        callback(event);
    }
};
} // namespace
std::shared_ptr<Ed2kResources> makeEd2kResources() { return std::make_shared<Ed2kResources>(); }
std::unique_ptr<ITransferTransport> makeEd2kTransfer(Ed2kOptions options,
                                                     std::shared_ptr<Ed2kResources> resources) {
    return std::make_unique<Ed2kTransfer>(std::move(options), std::move(resources));
}
} // namespace maskpilot
