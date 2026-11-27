#include "../../src/infrastructure/src/ed2k/checkpoint.h"
#include "../../src/infrastructure/src/ed2k/identity.h"
#include "../../src/infrastructure/src/ed2k/messages.h"
#include "../../src/infrastructure/src/ed2k/resources.h"
#include "../../src/infrastructure/src/ed2k/verification.h"
#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <sqlite3.h>
#include <thread>
#include <tuple>

namespace {
using namespace maskpilot;
namespace wire = maskpilot::ed2k;
std::filesystem::path path(const QTemporaryDir& directory) {
    const auto bytes = directory.path().toUtf8();
    return std::filesystem::canonical(
        std::filesystem::path(std::u8string(bytes.constData(), bytes.constData() + bytes.size())));
}
Task sample(const std::filesystem::path& folder) {
    Task task;
    task.id = newTaskId();
    task.spec.url = "ed2k://|file|a.bin|3|a448017aaf21d8525fc10ae87aa6729d|/";
    task.spec.destination = folder / "a.bin";
    return task;
}
TEST(Ed2kRecovery, PersistsVerifiedPartsAndRehashesOnRestart) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = path(temporary);
    auto task = sample(root);
    const auto link = parseEd2kFileLink(task.spec.url);
    TransferControl control;
    const wire::Bytes data{'a', 'b', 'c'};
    wire::Hash identity;
    {
        wire::Checkpoint checkpoint(task, root / "work", link, control);
        identity = checkpoint.identity();
        EXPECT_EQ(checkpoint.verifiedBytes(), 0);
        checkpoint.commitPart(0, data);
        EXPECT_EQ(checkpoint.verifiedBytes(), 3);
    }
    {
        wire::Checkpoint checkpoint(task, root / "work", link, control);
        EXPECT_EQ(checkpoint.identity(), identity);
        EXPECT_EQ(checkpoint.verifiedBytes(), 3);
        EXPECT_EQ(checkpoint.readPart(0), data);
    }
    {
        std::ofstream changed(root / "work/ed2k/payload.bin", std::ios::binary);
        changed << "xyz";
    }
    {
        wire::Checkpoint checkpoint(task, root / "work", link, control);
        EXPECT_EQ(checkpoint.verifiedBytes(), 0);
        EXPECT_EQ(std::filesystem::file_size(root / "work/ed2k/payload.bin"), 3);
        checkpoint.commitPart(0, data);
    }
    {
        std::ofstream unknown(root / "work/ed2k/keep.txt");
        unknown << "user data";
    }
    wire::Checkpoint::discard(task, root / "work");
    EXPECT_EQ(std::filesystem::file_size(root / "work/ed2k/keep.txt"), 9);
    EXPECT_TRUE(std::filesystem::exists(root / "work/ed2k/identity.json"));
}
TEST(Ed2kRecovery, RejectsForeignOrUnverifiedDataWithoutOverwrite) {
    QTemporaryDir temporary;
    const auto root = path(temporary);
    auto task = sample(root);
    auto link = parseEd2kFileLink(task.spec.url);
    TransferControl control;
    {
        wire::Checkpoint checkpoint(task, root / "work", link, control);
        EXPECT_THROW(checkpoint.commitPart(0, wire::Bytes{'b', 'a', 'd'}), std::invalid_argument);
        EXPECT_EQ(std::filesystem::file_size(root / "work/ed2k/payload.bin"), 0);
    }
    auto foreign = task;
    foreign.ownerId = "different-owner";
    EXPECT_THROW(wire::Checkpoint(foreign, root / "work", link, control), std::runtime_error);
    std::filesystem::create_directories(root / "foreign/ed2k");
    {
        std::ofstream keep(root / "foreign/ed2k/keep.txt");
        keep << "user data";
    }
    EXPECT_THROW(wire::Checkpoint(task, root / "foreign", link, control), std::runtime_error);
    EXPECT_EQ(std::filesystem::file_size(root / "foreign/ed2k/keep.txt"), 9);
}
TEST(Ed2kRecovery, CleanDiscardRemovesOnlyVerifiedOwnedParts) {
    QTemporaryDir temporary;
    const auto root = path(temporary);
    auto task = sample(root);
    TransferControl control;
    {
        wire::Checkpoint checkpoint(task, root / "work", parseEd2kFileLink(task.spec.url), control);
        checkpoint.commitPart(0, wire::Bytes{'a', 'b', 'c'});
    }
    wire::Checkpoint::discard(task, root / "work");
    EXPECT_FALSE(std::filesystem::exists(root / "work/ed2k"));
}
TEST(Ed2kRecovery, PreservesForeignStageAndReplacedRecoveryObject) {
    QTemporaryDir temporary;
    const auto root = path(temporary);
    auto task = sample(root);
    const auto link = parseEd2kFileLink(task.spec.url);
    TransferControl control;
    auto stage = task.spec.destination;
    stage += ".maskpilot-" + task.id + ".tmp";
    {
        wire::Checkpoint checkpoint(task, root / "work", link, control);
        checkpoint.commitPart(0, wire::Bytes{'a', 'b', 'c'});
        {
            std::ofstream userFile(stage);
            userFile << "user stage";
        }
        EXPECT_THROW(checkpoint.assembleStage(), std::runtime_error);
        checkpoint.cleanupStage();
        EXPECT_EQ(std::filesystem::file_size(stage), 10);
    }
    EXPECT_THROW(wire::Checkpoint::discard(task, root / "work"), std::runtime_error);
    EXPECT_EQ(std::filesystem::file_size(stage), 10);
    const auto payload = root / "work/ed2k/payload.bin";
    std::filesystem::rename(payload, root / "original-cache.bin");
    {
        std::ofstream replacement(payload);
        replacement << "abc";
    }
    EXPECT_THROW(wire::Checkpoint(task, root / "work", link, control), std::runtime_error);
    EXPECT_EQ(std::filesystem::file_size(payload), 3);
    EXPECT_EQ(std::filesystem::file_size(root / "original-cache.bin"), 3);
}
TEST(Ed2kRecovery, RejectsNonregularRecoveryObject) {
    QTemporaryDir temporary;
    const auto root = path(temporary);
    auto task = sample(root);
    const auto link = parseEd2kFileLink(task.spec.url);
    TransferControl control;
    {
        wire::Checkpoint checkpoint(task, root / "work", link, control);
    }
    std::filesystem::remove(root / "work/ed2k/payload.bin");
    std::filesystem::create_directory(root / "work/ed2k/payload.bin");
    EXPECT_THROW(wire::Checkpoint(task, root / "work", link, control), std::runtime_error);
    EXPECT_TRUE(std::filesystem::is_directory(root / "work/ed2k/payload.bin"));
}
TEST(Ed2kRecovery, VerifiesFinalMd4AuxiliaryAndCallerHash) {
    QTemporaryDir temporary;
    const auto root = path(temporary);
    auto task = sample(root);
    {
        std::ofstream output(task.spec.destination, std::ios::binary);
        output << "abc";
    }
    auto link = parseEd2kFileLink(task.spec.url);
    link.aichRoot = "VGMT4NSHA2AWVOR6EVYXQUGCNSONBWE5";
    task.spec.expectedSha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    TransferControl control;
    EXPECT_NO_THROW(wire::verifyStage(task, link, task.spec.destination, control));
    link.aichRoot[0] = 'A';
    EXPECT_THROW(wire::verifyStage(task, link, task.spec.destination, control), std::runtime_error);
    link.aichRoot.clear();
    task.spec.expectedSha256[0] = '0';
    EXPECT_THROW(wire::verifyStage(task, link, task.spec.destination, control), std::runtime_error);
    task.spec.expectedSha256.clear();
    {
        std::ofstream output(task.spec.destination, std::ios::binary);
        output << "bad";
    }
    EXPECT_THROW(wire::verifyStage(task, link, task.spec.destination, control), std::runtime_error);
}
TEST(Ed2kMessages, RejectsHashsetMismatchAndBoundsWireMetadata) {
    const auto link = parseEd2kFileLink("ed2k://|file|a|3|a448017aaf21d8525fc10ae87aa6729d|/");
    auto payload = wire::hashBytes(wire::parseHash(link.hash));
    wire::append16(payload, 0);
    EXPECT_EQ(wire::parseHashset({wire::classicProtocol, 0x52, payload}, link),
              std::vector{wire::parseHash(link.hash)});
    payload[0] ^= 1;
    EXPECT_THROW(wire::parseHashset({wire::classicProtocol, 0x52, payload}, link), std::invalid_argument);
    wire::Bytes tags;
    wire::append32(tags, 129);
    wire::Reader reader(tags);
    EXPECT_THROW(wire::readTags(reader), std::invalid_argument);
    EXPECT_THROW(wire::inflateBounded(wire::Bytes{1, 2, 3}, 1024), std::invalid_argument);
    const auto identity = wire::parseHash(link.hash);
    const auto info = wire::parseHello(wire::hello(identity, 0, true));
    EXPECT_EQ(info.identity, identity);
    EXPECT_TRUE(info.largeFiles);
    EXPECT_FALSE(info.requiresEncryption);
}
TEST(Ed2kIdentity, ReusesOwnerRouteIdentityAcrossTasksAndPreservesRouteIsolation) {
    QTemporaryDir temporary;
    const auto root = path(temporary);
    auto first = sample(root), second = sample(root);
    const auto link = parseEd2kFileLink(first.spec.url);
    TransferControl control;
    wire::Hash identity;
    {
        wire::Checkpoint checkpoint(first, root / "first", link, control);
        identity = checkpoint.identity();
    }
    {
        wire::Checkpoint checkpoint(second, root / "second", link, control);
        EXPECT_EQ(checkpoint.identity(), identity);
    }
    wire::Checkpoint::discard(first, root / "first");
    auto third = sample(root);
    {
        wire::Checkpoint checkpoint(third, root / "third", link, control);
        EXPECT_EQ(checkpoint.identity(), identity);
    }
    third.spec.proxy = "socks5://127.0.0.1:1080";
    third.spec.proxyUsername = "fixture-private-user";
    third.spec.proxyPassword = "fixture-private-password";
    {
        wire::Checkpoint checkpoint(third, root / "third", link, control);
        EXPECT_NE(checkpoint.identity(), identity);
    }
    for (const auto& entry : std::filesystem::directory_iterator(root / "ed2k-identities")) {
        std::ifstream file(entry.path());
        const std::string text((std::istreambuf_iterator<char>(file)), {});
        EXPECT_EQ(text.find("fixture-private"), text.npos);
    }
}
TEST(Ed2kIdentity, EndpointLeasesAreFifoAndCancellationRemovesWaiters) {
    auto resources = std::make_shared<Ed2kResources>();
    auto first = resources->tryPeer("owner-route-peer", "first");
    ASSERT_TRUE(first);
    EXPECT_FALSE(resources->tryPeer("owner-route-peer", "second"));
    EXPECT_FALSE(resources->tryPeer("owner-route-peer", "third"));
    EXPECT_TRUE(resources->peerWaiting("owner-route-peer"));
    first.reset();
    EXPECT_FALSE(resources->tryPeer("owner-route-peer", "third"));
    resources->forget("second");
    auto third = resources->tryPeer("owner-route-peer", "third");
    ASSERT_TRUE(third);
    auto isolated = resources->tryPeer("different-route-peer", "first");
    EXPECT_TRUE(isolated);
    third.reset();
    isolated.reset();
    EXPECT_TRUE(resources->endpoints.empty());
}
TEST(Ed2kMessages, UsesLargeFileEncodingAtTheLegacyPartBoundary) {
    auto link = parseEd2kFileLink("ed2k://|file|a|3|a448017aaf21d8525fc10ae87aa6729d|/");
    link.size = ed2kLegacyMaxFileBytes;
    EXPECT_EQ(wire::sourceRequest(link).payload.size(), 20);
    ++link.size;
    const auto request = wire::sourceRequest(link);
    wire::Reader reader(request.payload);
    wire::requireHash(reader, wire::parseHash(link.hash));
    EXPECT_EQ(reader.u32(), 0);
    EXPECT_EQ(reader.u64(), link.size);
    EXPECT_NO_THROW(reader.requireEnd());
}
TEST(Ed2kRecovery, MatchesIndependentAuxiliaryTreesAcrossPartBoundaries) {
    QTemporaryDir temporary;
    const auto root = path(temporary);
    auto task = sample(root);
    const std::string zeroes(128 * 1024, char(0));
    for (const auto& [size, hash, auxiliary] :
         std::vector<std::tuple<std::uint64_t, std::string, std::string>>{
             {9728001, "06329e9dba1373512c06386fe29e3c65", "HL3TFXORIUEPXUWFPY3JLR7SMKGTO4IH"},
             {19456001, "e57f824d28f69fe90864e17673668457", "A24DAJJZDJ7ZW7ER5MK7Y4U2SW3VC7CW"}}) {
        {
            std::ofstream output(task.spec.destination, std::ios::binary);
            auto remaining = size;
            while (remaining) {
                const auto count = std::min<std::uint64_t>(remaining, zeroes.size());
                output.write(zeroes.data(), static_cast<std::streamsize>(count));
                remaining -= count;
            }
        }
        auto link = parseEd2kFileLink(task.spec.url);
        link.size = size;
        link.hash = hash;
        link.aichRoot = auxiliary;
        TransferControl control;
        EXPECT_NO_THROW(wire::verifyStage(task, link, task.spec.destination, control));
    }
}
} // namespace

TEST(Ed2kProduct, SpecAndAppendOnlyKindsStates) {
    QTemporaryDir temporary;
    auto task = sample(path(temporary));
    EXPECT_EQ(sourceKind(task.spec), TransferKind::Ed2k);
    EXPECT_TRUE(validateSpec(task.spec).empty());
    EXPECT_EQ(static_cast<int>(TransferKind::Ed2k), 6);
    EXPECT_EQ(static_cast<int>(TaskState::PolicyWaiting), 17);
    EXPECT_TRUE(isActive(TaskState::SourceWaiting));
    EXPECT_TRUE(isActive(TaskState::RemoteQueued));
    task.spec.cookie = "secret";
    EXPECT_FALSE(validateSpec(task.spec).empty());
    task.spec.cookie.clear();
    task.spec.originAuthenticationRequired = true;
    EXPECT_FALSE(validateSpec(task.spec).empty());
    task.spec.originAuthenticationRequired = false;
    task.spec.url = "https://example.test/file";
    task.spec.ed2k.sources = {parseEd2kEndpoint("127.0.0.1:42")};
    EXPECT_FALSE(validateSpec(task.spec).empty());
}
TEST(Ed2kProduct, UpgradeSchema19AndRoundTripOptionsWithoutReorderingColumns) {
    QTemporaryDir temporary;
    const auto root = path(temporary), database = root / "tasks.db";
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
    const auto migrations =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "database/migrations";
    std::vector<std::filesystem::path> scripts;
    for (const auto& file : std::filesystem::directory_iterator(migrations))
        if (file.path().extension() == ".sql" && file.path().filename().string() < "0020")
            scripts.push_back(file.path());
    std::sort(scripts.begin(), scripts.end());
    ASSERT_EQ(scripts.size(), 19);
    for (const auto& script : scripts) {
        std::ifstream input(script);
        const std::string sql((std::istreambuf_iterator<char>(input)), {});
        ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    auto task = sample(root);
    task.spec.kind = TransferKind::Ed2k;
    task.spec.ed2k.servers = {parseEd2kEndpoint("index.example:4661", true)};
    task.spec.ed2k.sources = {parseEd2kEndpoint("127.0.0.1:42:128:11223344556677889900112233445566")};
    task.state = TaskState::RemoteQueued;
    {
        auto store = makeSqliteStore(database);
        store.tasks->save(task);
        task.ownerId = newTaskId();
        task.contextRevision = 1;
        EXPECT_THROW(store.tasks->save(task), std::invalid_argument);
    }
    auto store = makeSqliteStore(database);
    const auto loaded = store.tasks->load();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded.front().spec.ed2k, task.spec.ed2k);
    EXPECT_EQ(loaded.front().spec.kind, TransferKind::Ed2k);
    EXPECT_EQ(loaded.front().state, TaskState::RemoteQueued);
    bool backup = false;
    for (const auto& file : std::filesystem::directory_iterator(root))
        backup |= file.path().filename().string().find(".before-v20-") != std::string::npos;
    EXPECT_TRUE(backup);
}
TEST(Ed2kProduct, RemovalPreservesForeignStageAndPublishedFile) {
    QTemporaryDir temporary;
    const auto root = path(temporary);
    auto store = makeSqliteStore(root / "tasks.db");
    DownloadService service(std::move(store.tasks), makeUnifiedTransferFactory(), root / "parts");
    auto spec = sample(root).spec;
    spec.startPaused = true;
    const auto id = service.add(spec).get();
    auto stage = spec.destination;
    stage += ".maskpilot-" + id + ".tmp";
    {
        std::ofstream(stage) << "foreign";
        std::ofstream(spec.destination) << "existing";
    }
    service.remove(id);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!service.snapshot().empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(service.snapshot().empty());
    EXPECT_TRUE(std::filesystem::exists(stage));
    EXPECT_TRUE(std::filesystem::exists(spec.destination));
}

TEST(Ed2kProduct, RecoveryPausesActiveRemoteQueueAndRetainsSources) {
    QTemporaryDir temporary;
    const auto root = path(temporary);
    auto task = sample(root);
    task.spec.kind = TransferKind::Ed2k;
    task.spec.ed2k.servers = {parseEd2kEndpoint("index.example:4661", true)};
    task.state = TaskState::RemoteQueued;
    {
        auto store = makeSqliteStore(root / "tasks.db");
        store.tasks->save(task);
    }
    auto store = makeSqliteStore(root / "tasks.db");
    DownloadService service(std::move(store.tasks), makeUnifiedTransferFactory(), root / "parts");
    const auto tasks = service.snapshot();
    ASSERT_EQ(tasks.size(), 1);
    EXPECT_EQ(tasks.front().state, TaskState::Paused);
    EXPECT_EQ(tasks.front().runIntent, RunIntent::Paused);
    EXPECT_EQ(tasks.front().spec.ed2k, task.spec.ed2k);
}

TEST(Ed2kProduct, EditingSourcesPreservesImmutableFileIdentity) {
    QTemporaryDir temporary;
    const auto root = path(temporary);
    auto store = makeSqliteStore(root / "tasks.db");
    DownloadService service(std::move(store.tasks), makeUnifiedTransferFactory(), root / "parts");
    auto spec = sample(root).spec;
    spec.startPaused = true;
    const auto id = service.add(spec).get();
    auto changed = spec;
    changed.url = "ed2k://|file|other.bin|3|11223344556677889900112233445566|/";
    EXPECT_THROW(service.updateRequestWithCredentials(id, changed, false).get(), std::invalid_argument);
    ASSERT_EQ(service.snapshot().size(), 1);
    EXPECT_EQ(service.snapshot().front().spec.url, spec.url);
    spec.ed2k.sources = {parseEd2kEndpoint("127.0.0.1:1234")};
    EXPECT_NO_THROW(service.updateRequestWithCredentials(id, spec, false).get());
    EXPECT_EQ(service.snapshot().front().spec.ed2k, spec.ed2k);
}
