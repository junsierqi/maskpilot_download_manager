#include "peer.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace maskpilot::ed2k {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t blockBytes = 180 * 1024;
struct Block {
    std::uint64_t begin = 0, end = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    Bytes compressed;
    std::uint32_t compressedSize = 0;
    void cover(std::uint64_t a, std::uint64_t b) {
        ranges.emplace_back(a, b);
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
        for (const auto& range : ranges) {
            if (!merged.empty() && merged.back().second >= range.first)
                merged.back().second = std::max(merged.back().second, range.second);
            else
                merged.push_back(range);
        }
        if (merged.size() > 256)
            throw std::invalid_argument("ED2K block fragmentation exceeds its bound.");
        ranges = std::move(merged);
    }
    bool complete() const {
        return ranges.size() == 1 && ranges[0].first == begin && ranges[0].second == end;
    }
};
} // namespace
struct Peer::Impl {
    Ed2kEndpoint endpoint;
    Connection connection;
    Ed2kFileLink link;
    Hash identity, file;
    bool sentHello = false, helloDone = false, started = false, accepted = false, large = false;
    bool statusSeen = false;
    std::uint32_t queueRank = 0;
    std::vector<bool> available;
    std::optional<std::size_t> part;
    std::shared_ptr<void> memoryLease;
    Bytes data;
    std::size_t nextOffset = 0, completeBytes = 0;
    std::vector<Block> blocks;
    std::optional<CompletedPart> completed;
    std::optional<std::vector<Hash>> hashes;
    Clock::time_point activity = Clock::now(), reask = Clock::now(), born = Clock::now();
    void releasePart() {
        if (completed)
            return;
        part.reset();
        Bytes{}.swap(data);
        memoryLease.reset();
        blocks.clear();
        nextOffset = completeBytes = 0;
    }
    Impl(Ed2kEndpoint source, const DownloadSpec& route, Ed2kFileLink value, Hash client,
         std::shared_ptr<void> lease)
        : endpoint(std::move(source)), connection(endpoint, route, std::move(lease)), link(std::move(value)),
          identity(client), file(parseHash(link.hash)) {}
    void sendHash(std::uint8_t opcode) { connection.send({classicProtocol, opcode, hashBytes(file)}); }
    void requestBlocks() {
        if (!part || !accepted || completed)
            return;
        std::array<std::uint64_t, 3> begins{}, ends{};
        std::size_t count = 0;
        while (blocks.size() < 3 && nextOffset < data.size()) {
            const auto length = std::min<std::size_t>(blockBytes, data.size() - nextOffset);
            const auto begin = *part * ed2kPartBytes + nextOffset;
            begins[count] = begin;
            ends[count] = begin + length;
            blocks.push_back(Block{begin, begin + length});
            nextOffset += length;
            ++count;
        }
        if (!count)
            return;
        auto payload = hashBytes(file);
        if (link.size > ed2kLegacyMaxFileBytes) {
            if (!large)
                throw std::runtime_error("ED2K source does not support this large file.");
            for (auto value : begins)
                append64(payload, value);
            for (auto value : ends)
                append64(payload, value);
            connection.send({extendedProtocol, 0xa1, std::move(payload)});
        } else {
            for (auto value : begins)
                append32(payload, static_cast<std::uint32_t>(value));
            for (auto value : ends)
                append32(payload, static_cast<std::uint32_t>(value));
            connection.send({classicProtocol, 0x47, std::move(payload)});
        }
    }
    void receiveData(Frame frame, TransferControl& control, std::uint64_t& traffic) {
        const bool compressed = frame.opcode == 0x40 || frame.opcode == 0xa3;
        const bool wide = frame.opcode == 0xa2 || frame.opcode == 0xa3;
        const auto header = std::size_t(16 + (wide ? 8 : 4) + (compressed ? 4 : (wide ? 8 : 4)));
        const auto charged = frame.protocol == packedProtocol
                                 ? frame.payload.size()
                                 : (frame.payload.size() > header ? frame.payload.size() - header : 0);
        const auto admitted = control.payloadBudget ? control.payloadBudget->accept(charged) : charged;
        traffic += admitted;
        if (admitted != charged) {
            control.pause = true;
            throw std::runtime_error("ED2K native payload allowance was exhausted.");
        }
        frame = unpack(std::move(frame));
        Reader reader(frame.payload);
        requireHash(reader, file);
        const auto begin = wide ? reader.u64() : reader.u32();
        const auto endOrSize = compressed ? reader.u32() : (wide ? reader.u64() : reader.u32());
        if (!part || !accepted || begin < *part * ed2kPartBytes)
            throw std::invalid_argument("Unrequested ED2K file data.");
        auto block = std::find_if(blocks.begin(), blocks.end(), [&](const Block& value) {
            return compressed ? value.begin == begin : value.begin <= begin && begin < value.end;
        });
        if (block == blocks.end())
            throw std::invalid_argument("ED2K data is outside its requested block.");
        const auto bytes = reader.take(reader.remaining());
        std::uint64_t end = endOrSize;
        Bytes inflated;
        std::span<const std::uint8_t> received = bytes;
        if (compressed) {
            if (!endOrSize || endOrSize > block->end - block->begin + 1024 ||
                (block->compressedSize && block->compressedSize != endOrSize) ||
                bytes.size() > endOrSize - block->compressed.size())
                throw std::invalid_argument("Invalid ED2K compressed block bounds.");
            block->compressedSize = static_cast<std::uint32_t>(endOrSize);
            block->compressed.insert(block->compressed.end(), bytes.begin(), bytes.end());
            if (block->compressed.size() < endOrSize)
                return;
            inflated = inflateBounded(block->compressed, static_cast<std::size_t>(block->end - block->begin));
            if (inflated.size() != block->end - block->begin)
                throw std::invalid_argument("ED2K compressed block length mismatch.");
            received = inflated;
            end = block->end;
        }
        if (end <= begin || end > block->end || end - begin != received.size())
            throw std::invalid_argument("ED2K data length does not match its requested range.");
        const auto offset = begin - *part * ed2kPartBytes;
        if (offset > data.size() || received.size() > data.size() - offset)
            throw std::invalid_argument("ED2K data exceeds the assigned part.");
        std::copy(received.begin(), received.end(), data.begin() + static_cast<std::ptrdiff_t>(offset));
        block->cover(begin, end);
        if (block->complete()) {
            completeBytes += static_cast<std::size_t>(block->end - block->begin);
            blocks.erase(block);
        }
        if (completeBytes == data.size()) {
            completed = CompletedPart{*part, std::move(memoryLease), std::move(data)};
            blocks.clear();
        } else
            requestBlocks();
    }
    void handle(Frame frame, TransferControl& control, std::uint64_t& traffic) {
        if (frame.opcode == 0x46 || frame.opcode == 0x40 || frame.opcode == 0xa2 || frame.opcode == 0xa3) {
            receiveData(std::move(frame), control, traffic);
            return;
        }
        frame = unpack(std::move(frame));
        Reader reader(frame.payload);
        if (frame.protocol == classicProtocol && (frame.opcode == 0x4c || frame.opcode == 0x01)) {
            const auto info = parseHello(frame);
            if (info.requiresEncryption || (endpoint.cryptOptions & 4))
                throw std::runtime_error("ED2K source requires unsupported protocol encryption.");
            if (!endpoint.clientHash.empty() && info.identity != parseHash(endpoint.clientHash))
                throw std::invalid_argument("ED2K source identity differs from the link.");
            large = info.largeFiles;
            if (link.size > ed2kLegacyMaxFileBytes && !large)
                throw std::runtime_error("ED2K source does not support this large file.");
            if (frame.opcode == 0x01)
                connection.send(hello(identity, 0, true));
            if (!helloDone) {
                helloDone = true;
                sendHash(0x58);
            }
            return;
        }
        if (frame.protocol == extendedProtocol && (frame.opcode == 0x01 || frame.opcode == 0x02)) {
            reader.take(2);
            readTags(reader);
            reader.requireEnd();
            if (frame.opcode == 0x01) {
                Bytes response{1, 1};
                append32(response, 2);
                numberTag(response, 0x20, 1);
                numberTag(response, 0x24, 0);
                connection.send({extendedProtocol, 0x02, std::move(response)});
            }
            return;
        }
        if (!helloDone)
            throw std::invalid_argument("ED2K peer sent file metadata before its handshake.");
        switch (frame.opcode) {
        case 0x59: {
            requireHash(reader, file);
            const auto length = reader.u16();
            if (length > 4096)
                throw std::invalid_argument("ED2K peer filename is too long.");
            reader.take(length);
            reader.requireEnd();
            // Peer filenames are never used for local paths.
            if (!started) {
                started = true;
                sendHash(0x4f);
                if (link.size >= ed2kPartBytes)
                    sendHash(0x51);
            }
            break;
        }
        case 0x50:
            available = parsePartStatus(frame, link);
            statusSeen = true;
            sendHash(0x54);
            reask = Clock::now();
            break;
        case 0x52:
            hashes = parseHashset(frame, link);
            break;
        case 0x55:
            reader.requireEnd();
            accepted = true;
            queueRank = 0;
            requestBlocks();
            break;
        case 0x48:
            requireHash(reader, file);
            reader.requireEnd();
            throw std::runtime_error("ED2K source no longer has the requested file.");
        case 0x57:
            reader.requireEnd();
            accepted = false;
            releasePart();
            break;
        case 0x5c:
            queueRank = reader.u32();
            reader.requireEnd();
            accepted = false;
            releasePart();
            break;
        case 0x60:
            if (frame.protocol != extendedProtocol)
                break;
            queueRank = reader.u16();
            reader.take(10);
            reader.requireEnd();
            accepted = false;
            releasePart();
            break;
        default:
            // Unsolicited bounded optional metadata is ignored; no uploads, messages or callbacks.
            break;
        }
    }
};
Peer::Peer(Ed2kEndpoint endpoint, const DownloadSpec& route, Ed2kFileLink link, const Hash& identity,
           std::shared_ptr<void> endpointLease) {
    if (!endpointLease)
        throw std::logic_error("ED2K peer requires an endpoint lease.");
    if (endpoint.cryptOptions & 4)
        throw std::runtime_error("ED2K source requires unsupported protocol encryption.");
    // The checkpoint already validated optional link proofs. Do not copy a large
    // proof/source list into each connection; peer proofs are checked again on commit.
    std::vector<std::string>{}.swap(link.partHashes);
    std::vector<std::string>{}.swap(link.webSeeds);
    std::vector<Ed2kEndpoint>{}.swap(link.sources);
    impl_ = std::make_unique<Impl>(std::move(endpoint), route, std::move(link), identity,
                                   std::move(endpointLease));
}
Peer::~Peer() = default;
std::size_t Peer::tick(TransferControl& control, bool receive, std::uint64_t& payloadBytes,
                       std::size_t readLimit) {
    auto& value = *impl_;
    const auto read = value.connection.tick(receive, readLimit);
    if (value.connection.connected() && !value.sentHello) {
        value.connection.send(hello(value.identity, 0));
        value.sentHello = true;
    }
    for (unsigned i = 0; i < 64; ++i) {
        auto frame = value.connection.next();
        if (!frame)
            break;
        value.activity = Clock::now();
        value.handle(std::move(*frame), control, payloadBytes);
    }
    const auto now = Clock::now();
    if (!receive)
        value.activity = now;
    if ((!value.statusSeen && now - value.born > std::chrono::seconds(45)) ||
        (value.part && now - value.activity > std::chrono::seconds(90)))
        throw std::runtime_error("ED2K source stopped responding.");
    if (value.statusSeen && !value.accepted && now - value.reask > std::chrono::minutes(29)) {
        value.sendHash(0x54);
        value.reask = now;
    }
    return read;
}
bool Peer::ready() const { return impl_->accepted && impl_->statusSeen && !impl_->part; }
bool Peer::queued() const { return impl_->statusSeen && !impl_->accepted; }
std::uint32_t Peer::rank() const { return impl_->queueRank; }
std::string Peer::status() const {
    if (!impl_->connection.connected())
        return "Connecting to ED2K source.";
    if (!impl_->helloDone)
        return "Waiting for ED2K handshake.";
    if (!impl_->started)
        return "Waiting for ED2K file metadata.";
    if (!impl_->statusSeen)
        return "Waiting for ED2K file availability.";
    if (!impl_->accepted)
        return "Waiting in remote ED2K queue.";
    if (!impl_->part)
        return "Waiting for ED2K part proof or memory allowance.";
    return "Downloading ED2K part.";
}
bool Peer::hasPart(std::size_t index) const {
    return index < impl_->available.size() && impl_->available[index];
}
std::optional<std::size_t> Peer::assigned() const { return impl_->part; }
std::uint64_t Peer::received() const { return impl_->completeBytes; }
void Peer::assign(std::size_t index, std::size_t size, std::shared_ptr<void> memoryLease) {
    if (!ready() || !hasPart(index) || !size || size > ed2kPartBytes || !memoryLease)
        throw std::logic_error("Invalid ED2K peer part assignment.");
    impl_->part = index;
    impl_->data.resize(size);
    impl_->nextOffset = 0;
    impl_->completeBytes = 0;
    impl_->memoryLease = std::move(memoryLease);
    impl_->requestBlocks();
}
std::optional<CompletedPart> Peer::takePart() {
    auto result = std::move(impl_->completed);
    impl_->completed.reset();
    if (result) {
        impl_->part.reset();
        impl_->completeBytes = 0;
    }
    return result;
}
std::optional<std::vector<Hash>> Peer::takeHashes() {
    auto result = std::move(impl_->hashes);
    impl_->hashes.reset();
    return result;
}
} // namespace maskpilot::ed2k
