#include <fstream>
#include <gtest/gtest.h>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <thread>

namespace {
struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 std::filesystem::path("maskpilot-test-" + maskpilot::newTaskId());
    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace
TEST(Storage, PersistsTaskButNeverAuthenticationSecrets) {
    TempDirectory temp;
    auto repo = maskpilot::makeSqliteRepository(temp.path / "tasks.db");
    maskpilot::Task t;
    t.id = maskpilot::newTaskId();
    t.spec.url = "https://example.test/file";
    t.spec.destination = temp.path / "中文.bin";
    t.spec.cookie = "private-cookie";
    t.spec.authorization = "secret-token";
    t.createdAt = maskpilot::unixTime();
    repo->save(t);
    auto loaded = repo->load();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded[0].spec.destination, t.spec.destination);
    EXPECT_TRUE(loaded[0].spec.cookie.empty());
    EXPECT_TRUE(loaded[0].spec.authorization.empty());
    repo->erase(t.id);
    EXPECT_TRUE(repo->load().empty());
}
TEST(Checksums, UsesKnownSha256Vector) {
    TempDirectory temp;
    auto path = temp.path / "abc";
    {
        std::ofstream f(path);
        f << "abc";
    }
    EXPECT_EQ(maskpilot::sha256File(path),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
TEST(Storage, PreservesEmbeddedNullsForValidation) {
    TempDirectory temp;
    auto repo = maskpilot::makeSqliteRepository(temp.path / "tasks.db");
    maskpilot::Task task;
    task.id = maskpilot::newTaskId();
    task.spec.url = std::string("https://example.test/file\0suffix", 32);
    auto destination = (temp.path / "file.bin").u8string();
    destination += std::u8string(u8"\0suffix", 7);
    task.spec.destination = std::filesystem::path(destination);
    repo->save(task);
    const auto loaded = repo->load();
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded[0].spec.url, task.spec.url);
    EXPECT_EQ(loaded[0].spec.destination, task.spec.destination);
    EXPECT_FALSE(maskpilot::validateSpec(loaded[0].spec).empty());
    auto validUrl = loaded[0].spec;
    validUrl.url = "https://example.test/file";
    EXPECT_FALSE(maskpilot::validateSpec(validUrl).empty());
}
TEST(Storage, RejectsNoncanonicalTaskIdentities) {
    TempDirectory temp;
    auto repo = maskpilot::makeSqliteRepository(temp.path / "tasks.db");
    maskpilot::Task task;
    task.id = "ABCDEF0123456789ABCDEF0123456789";
    task.spec.url = "https://example.test/file";
    task.spec.destination = temp.path / "file.bin";
    repo->save(task);
    EXPECT_THROW(repo->load(), std::runtime_error);
}
TEST(Service, RejectsCaseVariantCacheOwners) {
    TempDirectory temp;
    maskpilot::DownloadService service(maskpilot::makeSqliteRepository(temp.path / "tasks.db"),
                                       maskpilot::makeCurlTransfer, temp.path / "parts");
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/file";
    spec.destination = temp.path / "file.bin";
    spec.startPaused = true;
    const std::string id = "abcdef0123456789abcdef0123456789";
    ASSERT_EQ(service.add(spec, id).get(), id);
    EXPECT_THROW(service.add(spec, "ABCDEF0123456789ABCDEF0123456789").get(), std::invalid_argument);
    EXPECT_EQ(service.snapshot().size(), 1);
}
TEST(Checksums, HonorsCancellation) {
    TempDirectory temp;
    auto path = temp.path / "abc";
    {
        std::ofstream f(path);
        f << "abc";
    }
    maskpilot::TransferControl control;
    control.cancel = true;
    EXPECT_THROW(maskpilot::sha256File(path, &control), std::runtime_error);
}
TEST(Service, DurableAcknowledgementAndIdempotentRequestIdentity) {
    TempDirectory temp;
    maskpilot::DownloadService service(maskpilot::makeSqliteRepository(temp.path / "tasks.db"),
                                       maskpilot::makeCurlTransfer, temp.path / "parts");
    maskpilot::DownloadSpec spec;
    spec.url = "https://example.test/file";
    spec.destination = temp.path / "file";
    spec.startPaused = true;
    const auto request = maskpilot::newTaskId();
    EXPECT_EQ(service.add(spec, request).get(), request);
    EXPECT_EQ(service.add(spec, request).get(), request);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(service.snapshot().size(), 1);
}
