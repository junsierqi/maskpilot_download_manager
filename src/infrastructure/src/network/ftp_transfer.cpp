#include "../filesystem/transfer_files.h"
#include "proxy_options.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <curl/curl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <maskpilot/infrastructure/adapters.h>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace maskpilot {
namespace {
using Clock = std::chrono::steady_clock;
namespace files = transfer_files;

struct Easy {
    CURL* value = curl_easy_init();
    Easy() {
        if (!value)
            throw std::runtime_error("Cannot create FTP connection.");
    }
    ~Easy() { curl_easy_cleanup(value); }
    Easy(const Easy&) = delete;
    Easy& operator=(const Easy&) = delete;
};
struct File {
    FILE* value = nullptr;
    ~File() {
        if (value)
            fclose(value);
    }
    void close() {
        if (value) {
            auto* previous = value;
            value = nullptr;
            if (fclose(previous) != 0)
                throw std::runtime_error("Cannot close FTP cache file.");
        }
    }
};
struct Result {
    TransferOutcome outcome;
    std::string error;
    bool retryable = false;
};
Result stopped(const TransferControl& control) {
    return {control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused, {}};
}
bool isStopped(const TransferControl& control) { return control.cancel || control.pause; }

void initializeCurl() {
    // Keep a process-lifetime reference. HTTP may independently acquire/release its own reference.
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("Cannot initialize FTP networking.");
    });
}
template <typename T> void option(CURL* easy, CURLoption key, T value) {
    if (curl_easy_setopt(easy, key, value) != CURLE_OK)
        throw std::runtime_error("Cannot configure FTP connection.");
}
std::string pathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
void configure(CURL* easy, const DownloadSpec& spec, TransferControl& control) {
    option(easy, CURLOPT_URL, spec.url.c_str());
    option(easy, CURLOPT_PROTOCOLS_STR, "ftp,ftps");
    option(easy, CURLOPT_FOLLOWLOCATION, 0L);
    // Preserve native FTP semantics through HTTP proxies; never accept a proxy's HTML representation.
    option(easy, CURLOPT_HTTPPROXYTUNNEL, 1L);
    option(easy, CURLOPT_NOSIGNAL, 1L);
    option(easy, CURLOPT_CONNECTTIMEOUT, 15L);
    option(easy, CURLOPT_FTP_RESPONSE_TIMEOUT, 30L);
    option(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    option(easy, CURLOPT_LOW_SPEED_TIME, 60L);
    option(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    option(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    option(easy, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    option(easy, CURLOPT_FILETIME, 1L);
    if (spec.ftpExplicitTls || spec.url.starts_with("ftps://"))
        option(easy, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    if (spec.ftpActiveMode)
        option(easy, CURLOPT_FTPPORT, "-");
    if (!spec.ftpUsername.empty())
        option(easy, CURLOPT_USERNAME, spec.ftpUsername.c_str());
    if (!spec.ftpPassword.empty())
        option(easy, CURLOPT_PASSWORD, spec.ftpPassword.c_str());
    network::configureProxy(easy, spec);
    if (!spec.ftpCaBundle.empty()) {
        const auto ca = pathText(spec.ftpCaBundle);
        option(easy, CURLOPT_CAINFO, ca.c_str()); // libcurl copies this string.
    }
    option(easy, CURLOPT_NOPROGRESS, 0L);
    option(
        easy, CURLOPT_XFERINFOFUNCTION,
        +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return isStopped(*static_cast<TransferControl*>(context)) ? 1 : 0;
        });
    option(easy, CURLOPT_XFERINFODATA, &control);
}
CURLcode perform(CURL* easy, TransferControl& control, const std::function<void()>& progress = {}) {
    struct Multi {
        CURLM* value = curl_multi_init();
        CURL* easy;
        explicit Multi(CURL* handle) : easy(handle) {
            if (!value)
                throw std::runtime_error("Cannot create FTP transfer scheduler.");
            if (curl_multi_add_handle(value, easy) != CURLM_OK) {
                curl_multi_cleanup(value);
                value = nullptr;
                throw std::runtime_error("Cannot schedule FTP transfer.");
            }
        }
        ~Multi() {
            curl_multi_remove_handle(value, easy);
            curl_multi_cleanup(value);
        }
    } multi(easy);
    auto last = Clock::now();
    for (;;) {
        if (isStopped(control))
            return CURLE_ABORTED_BY_CALLBACK;
        option(easy, CURLOPT_MAX_RECV_SPEED_LARGE,
               static_cast<curl_off_t>(std::max<std::int64_t>(0, control.speedLimit.load())));
        int running = 0;
        if (curl_multi_perform(multi.value, &running) != CURLM_OK)
            throw std::runtime_error("FTP transfer scheduling failed.");
        const auto now = Clock::now();
        if (progress && (running == 0 || now - last >= std::chrono::milliseconds(200))) {
            progress();
            last = now;
        }
        if (!running) {
            int pending = 0;
            while (const auto* message = curl_multi_info_read(multi.value, &pending))
                if (message->msg == CURLMSG_DONE && message->easy_handle == easy)
                    return message->data.result;
            throw std::runtime_error("FTP transfer ended without a result.");
        }
        int descriptors = 0;
        if (curl_multi_poll(multi.value, nullptr, 0, 100, &descriptors) != CURLM_OK)
            throw std::runtime_error("Cannot wait for FTP connection.");
    }
}
Result failure(CURLcode code) {
    if (code == CURLE_LOGIN_DENIED)
        return {TransferOutcome::Attention, "FTP login was rejected. Check the task credentials."};
    if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE ||
        code == CURLE_SSL_CONNECT_ERROR || code == CURLE_USE_SSL_FAILED)
        return {TransferOutcome::Attention,
                "FTPS certificate or encrypted connection failed. Encryption was not downgraded."};
    if (code == CURLE_REMOTE_FILE_NOT_FOUND || code == CURLE_FTP_COULDNT_RETR_FILE)
        return {TransferOutcome::Attention, "FTP source is unavailable or access was denied."};
    const bool retry = code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
                       code == CURLE_OPERATION_TIMEDOUT || code == CURLE_RECV_ERROR ||
                       code == CURLE_SEND_ERROR || code == CURLE_PARTIAL_FILE;
    return {TransferOutcome::Failed, "FTP transfer failed (network code " + std::to_string(code) + ").",
            retry};
}
struct Metadata {
    curl_off_t size = -1, modified = -1;
    CURLcode code = CURLE_OK;
};
Metadata probe(const DownloadSpec& spec, TransferControl& control) {
    Easy easy;
    configure(easy.value, spec, control);
    option(easy.value, CURLOPT_NOBODY, 1L);
    option(
        easy.value, CURLOPT_WRITEFUNCTION,
        +[](char*, size_t size, size_t count, void*) -> size_t { return size * count; });
    Metadata result;
    result.code = perform(easy.value, control);
    curl_easy_getinfo(easy.value, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &result.size);
    curl_easy_getinfo(easy.value, CURLINFO_FILETIME_T, &result.modified);
    return result;
}
std::int64_t recoverOffset(const Task& task, const std::filesystem::path& work, const Metadata& remote) {
    // FTP timestamps and length are weak validators. Only a final expected digest authorizes reuse.
    if (task.spec.expectedSha256.empty() || remote.size < 0 || remote.modified < 0)
        return 0;
    const auto identity = work / "identity";
    std::error_code ec;
    if (std::filesystem::file_size(identity, ec) > 70000 || ec)
        return 0;
    std::ifstream meta(identity);
    std::string tag, url, hash;
    std::int64_t size = -1, modified = -1, offset = -1;
    bool tls = false;
    if (!(meta >> tag >> std::quoted(url) >> size >> modified >> std::quoted(hash) >> tls) ||
        tag != "ftp-v1" || url != task.spec.url || hash != task.spec.expectedSha256 || size != remote.size ||
        modified != remote.modified || tls != task.spec.ftpExplicitTls)
        return 0;
    std::ifstream saved(work / "offset-0");
    if (!(saved >> offset) || offset < 0 || offset > remote.size)
        return 0;
    const auto bytes = std::filesystem::file_size(work / "part-0", ec);
    if (ec || bytes < static_cast<std::uint64_t>(offset))
        return 0;
    return offset;
}
Result transfer(const Task& task, const std::filesystem::path& work, TransferControl& control,
                TransferEvent& event, const std::function<void(TransferEvent)>& callback) {
    initializeCurl();
    if (!isFtpUrl(task.spec.url))
        return {TransferOutcome::Attention, "This transfer requires an FTP or FTPS URL."};
    if (const auto error = validateSpec(task.spec); !error.empty())
        return {TransferOutcome::Attention, error};
    auto authenticatedSpec = task.spec;
    authenticatedSpec.originAuthenticationRequired =
        task.originAuthenticationRequired || requiresOriginAuthentication(task.spec);
    if (const auto error = validateOriginAuthentication(authenticatedSpec, true); !error.empty())
        return {TransferOutcome::Attention, error};
    if (isStopped(control))
        return stopped(control);
    const auto remote = probe(task.spec, control);
    if (isStopped(control))
        return stopped(control);
    if (remote.code != CURLE_OK)
        return failure(remote.code);
    event.total = remote.size;
    event.resumable = !task.spec.expectedSha256.empty() && remote.size >= 0 && remote.modified >= 0;
    std::filesystem::create_directories(work);
    auto offset = recoverOffset(task, work, remote);
    std::ostringstream identity;
    identity << "ftp-v1 " << std::quoted(task.spec.url) << ' ' << remote.size << ' ' << remote.modified << ' '
             << std::quoted(task.spec.expectedSha256) << ' ' << task.spec.ftpExplicitTls;
    files::atomicText(work / "identity", identity.str());
    const auto part = work / "part-0";
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (offset > 0)
            std::filesystem::resize_file(part, offset);
        File output;
        output.value = files::openFile(part, offset > 0 ? L"ab" : L"wb");
        if (!output.value)
            throw std::runtime_error("Cannot write FTP cache file.");
        struct Sink {
            FILE* file;
            std::int64_t bytes, limit;
            bool failed = false;
            TransferControl* control;
        } sink{output.value, offset, remote.size, false, &control};
        Easy easy;
        configure(easy.value, task.spec, control);
        option(
            easy.value, CURLOPT_WRITEFUNCTION,
            +[](char* data, size_t size, size_t count, void* opaque) -> size_t {
                auto& out = *static_cast<Sink*>(opaque);
                if (count && size > std::numeric_limits<size_t>::max() / count) {
                    out.failed = true;
                    return 0;
                }
                auto bytes = size * count;
                if (out.control->payloadBudget) {
                    auto accepted = out.control->payloadBudget->accept(bytes);
                    if (accepted < bytes)
                        out.control->pause = true;
                    bytes = accepted;
                    if (!bytes)
                        return 0;
                }
                if (bytes >
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - out.bytes) ||
                    (out.limit >= 0 && bytes > static_cast<std::uint64_t>(out.limit - out.bytes))) {
                    out.failed = true;
                    return 0;
                }
                if (fwrite(data, 1, bytes, out.file) != bytes) {
                    out.failed = true;
                    return 0;
                }
                out.bytes += static_cast<std::int64_t>(bytes);
                return bytes;
            });
        option(easy.value, CURLOPT_WRITEDATA, &sink);
        if (offset > 0)
            option(easy.value, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(offset));
        auto checkpointTime = Clock::now(), speedTime = checkpointTime;
        auto checkpointBytes = offset, speedBytes = offset;
        auto checkpoint = [&] {
            files::syncFile(output.value);
            files::atomicText(work / "offset-0", std::to_string(sink.bytes));
            checkpointTime = Clock::now();
            checkpointBytes = sink.bytes;
        };
        event.phase = TaskState::Downloading;
        event.downloaded = offset;
        callback(event);
        const auto code = perform(easy.value, control, [&] {
            const auto now = Clock::now();
            if (sink.bytes - checkpointBytes >= 1024 * 1024 ||
                now - checkpointTime >= std::chrono::seconds(1))
                checkpoint();
            const auto elapsed = std::chrono::duration<double>(now - speedTime).count();
            if (elapsed >= 0.5) {
                event.speed = static_cast<std::int64_t>((sink.bytes - speedBytes) / elapsed);
                speedBytes = sink.bytes;
                speedTime = now;
            }
            event.downloaded = sink.bytes;
            callback(event);
        });
        checkpoint();
        output.close();
        event.downloaded = sink.bytes;
        if (isStopped(control))
            return stopped(control);
        if (sink.failed)
            return {TransferOutcome::Attention,
                    "FTP output exceeded the expected length or could not be written."};
        if (offset > 0 && attempt == 0 &&
            (code == CURLE_RANGE_ERROR || code == CURLE_BAD_DOWNLOAD_RESUME ||
             code == CURLE_FTP_COULDNT_USE_REST)) {
            // Some servers advertise a file but reject REST. Discard uncertain data and retry once.
            offset = 0;
            continue;
        }
        if (code != CURLE_OK)
            return failure(code);
        if (remote.size >= 0 && sink.bytes != remote.size)
            return {TransferOutcome::Failed, "FTP source ended before the expected length was received.",
                    true};
        const auto after = probe(task.spec, control);
        if (isStopped(control))
            return stopped(control);
        if (after.code != CURLE_OK)
            return failure(after.code);
        if ((remote.size >= 0 && after.size != remote.size) ||
            (remote.modified >= 0 && after.modified != remote.modified))
            return {TransferOutcome::Attention, "FTP source changed during transfer. Restart the task."};
        break;
    }
    event.phase = TaskState::Verifying;
    event.total = event.downloaded;
    event.speed = 0;
    callback(event);
    if (isStopped(control))
        return stopped(control);
    std::filesystem::create_directories(task.spec.destination.parent_path());
    auto stage = task.spec.destination;
    stage += L".maskpilot-" + std::filesystem::path(task.id).wstring() + L".tmp";
    {
        std::ifstream input(part, std::ios::binary);
        if (!input)
            throw std::runtime_error("Cannot read FTP cache for publication.");
        File output;
        output.value = files::openFile(stage, L"wb");
        if (!output.value)
            throw std::runtime_error("Cannot assemble FTP output on the destination volume.");
        std::array<char, 256 * 1024> buffer{};
        std::int64_t copied = 0;
        while (input) {
            if (isStopped(control))
                return stopped(control);
            input.read(buffer.data(), buffer.size());
            const auto count = input.gcount();
            if (count && fwrite(buffer.data(), 1, static_cast<size_t>(count), output.value) !=
                             static_cast<size_t>(count))
                throw std::runtime_error("Cannot write FTP output on the destination volume.");
            copied += count;
        }
        if (!input.eof() || copied != event.downloaded)
            throw std::runtime_error("FTP cache changed during assembly.");
        files::syncFile(output.value);
        output.close();
    }
    if (!task.spec.expectedSha256.empty()) {
        auto expected = task.spec.expectedSha256;
        std::transform(expected.begin(), expected.end(), expected.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (sha256File(stage, &control) != expected)
            return {TransferOutcome::Attention, "FTP SHA-256 verification failed. Output was not published."};
    }
    event.phase = TaskState::Finalizing;
    callback(event);
    if (isStopped(control))
        return stopped(control);
    files::publishFile(stage, task.spec.destination);
    return {TransferOutcome::Complete, {}};
}
class FtpTransfer final : public ITransferTransport {
  public:
    void run(const Task& task, const std::filesystem::path& work, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        TransferEvent event;
        event.phase = TaskState::Probing;
        callback(event);
        Result result{TransferOutcome::Attention, {}};
        try {
            result = transfer(task, work, control, event, callback);
        } catch (const std::exception& error) {
            result = isStopped(control) ? stopped(control) : Result{TransferOutcome::Attention, error.what()};
        }
        // transfer() has released every connection and open file before terminal state is observable.
        event.outcome = result.outcome;
        event.error = std::move(result.error);
        event.retryable = result.retryable;
        event.speed = 0;
        callback(event);
    }
};
} // namespace
std::unique_ptr<ITransferTransport> makeFtpTransfer() { return std::make_unique<FtpTransfer>(); }
} // namespace maskpilot
