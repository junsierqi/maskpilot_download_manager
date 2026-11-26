#include "../filesystem/transfer_files.h"
#include "curl_runtime.h"
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
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif

namespace maskpilot {
namespace {
using Clock = std::chrono::steady_clock;
namespace files = transfer_files;

#ifdef _WIN32
constexpr bool nativeFtpsCompatibility = true;
#else
constexpr bool nativeFtpsCompatibility = false;
#endif

bool containsAscii(std::string_view text, std::string_view value) {
    return std::search(text.begin(), text.end(), value.begin(), value.end(),
                       [](unsigned char left, unsigned char right) {
                           return (left >= 'A' && left <= 'Z' ? left + ('a' - 'A') : left) == right;
                       }) != text.end();
}
bool requiresSessionReuse(std::string_view text) {
    if (containsAscii(text, "no session reuse required") ||
        containsAscii(text, "no session resumption required") ||
        containsAscii(text, "no session reuse is required") ||
        containsAscii(text, "no session resumption is required"))
        return false;
    return containsAscii(text, "session resumption required") ||
           containsAscii(text, "session reuse required") ||
           containsAscii(text, "session reuse is required") ||
           containsAscii(text, "session of data connection not resumed") ||
           containsAscii(text, "tls session not resumed");
}
struct FtpReply {
    int multiline = 0;
    bool multilineRequiresReuse = false, requiresReuse = false, resumeAccepted = false;

    void receive(std::string_view line) {
        if (line.size() > 65536) {
            multiline = 0;
            multilineRequiresReuse = requiresReuse = false;
            return;
        }
        const auto digit = [](char value) { return value >= '0' && value <= '9'; };
        const bool reply = line.size() >= 4 && digit(line[0]) && digit(line[1]) && digit(line[2]) &&
                           (line[3] == ' ' || line[3] == '-');
        const int code = reply ? (line[0] - '0') * 100 + (line[1] - '0') * 10 + line[2] - '0' : 0;
        if (multiline) {
            if (multiline == 425 || multiline == 522)
                multilineRequiresReuse = multilineRequiresReuse || requiresSessionReuse(line);
            if (code == multiline && line[3] == ' ') {
                if (code == 350)
                    resumeAccepted = true;
                requiresReuse = (code == 425 || code == 522) && multilineRequiresReuse;
                multiline = 0;
                multilineRequiresReuse = false;
            }
            return;
        }
        if (!reply)
            return;
        if (code == 350 && line[3] == ' ')
            resumeAccepted = true;
        requiresReuse = false;
        if (line[3] == '-') {
            multiline = code;
            multilineRequiresReuse = (code == 425 || code == 522) && requiresSessionReuse(line);
        } else
            requiresReuse = (code == 425 || code == 522) && requiresSessionReuse(line);
    }
};

struct Easy {
    CURL* value = curl_easy_init();
    Easy() {
        if (!value)
            throw std::runtime_error("Cannot create remote file connection.");
    }
    ~Easy() { curl_easy_cleanup(value); }
    Easy(const Easy&) = delete;
    Easy& operator=(const Easy&) = delete;
};
struct ResumeCommand {
    curl_slist* value = nullptr;
    ~ResumeCommand() { curl_slist_free_all(value); }
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
                throw std::runtime_error("Cannot close remote file cache file.");
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

template <typename T> void option(CURL* easy, CURLoption key, T value) {
    if (curl_easy_setopt(easy, key, value) != CURLE_OK)
        throw std::runtime_error("Cannot configure remote file connection.");
}
std::string pathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
void checkPrivateKey(const std::filesystem::path& path) {
#ifdef _WIN32
    for (auto current = path; !current.empty(); current = current.parent_path()) {
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::invalid_argument("Use a regular local private key without reparse points.");
        if (current == current.root_path())
            break;
    }
#endif
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) > 1024 * 1024 || error)
        throw std::invalid_argument("SFTP private key is unavailable or exceeds 1 MiB.");
}
void configure(CURL* easy, const DownloadSpec& spec, TransferControl& control) {
    option(easy, CURLOPT_URL, spec.url.c_str());
    const bool sftp = isSftpUrl(spec.url);
    option(easy, CURLOPT_PROTOCOLS_STR, sftp ? "sftp" : "ftp,ftps");
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
    if (sftp) {
        option(easy, CURLOPT_FRESH_CONNECT, 1L);
        option(easy, CURLOPT_FORBID_REUSE, 1L);
        const auto fingerprint = canonicalSftpHostKey(spec.sftp.hostKeySha256);
        option(easy, CURLOPT_SSH_HOST_PUBLIC_KEY_SHA256, fingerprint.c_str());
        option(easy, CURLOPT_USERNAME, spec.sftp.username.c_str());
        if (spec.sftp.authentication == SftpAuthentication::Password) {
            option(easy, CURLOPT_SSH_AUTH_TYPES, static_cast<long>(CURLSSH_AUTH_PASSWORD));
            option(easy, CURLOPT_PASSWORD, spec.sftp.password.c_str());
        } else {
            option(easy, CURLOPT_SSH_AUTH_TYPES, static_cast<long>(CURLSSH_AUTH_PUBLICKEY));
            checkPrivateKey(spec.sftp.privateKeyFile);
            const auto key = pathText(spec.sftp.privateKeyFile);
            option(easy, CURLOPT_SSH_PRIVATE_KEYFILE, key.c_str());
            option(easy, CURLOPT_SSH_PUBLIC_KEYFILE, "");
            option(easy, CURLOPT_KEYPASSWD, spec.sftp.keyPassphrase.c_str());
        }
    } else {
        if (spec.ftpExplicitTls || spec.url.starts_with("ftps://"))
            option(easy, CURLOPT_USE_SSL, CURLUSESSL_ALL);
        if (spec.ftpActiveMode)
            option(easy, CURLOPT_FTPPORT, "-");
        if (!spec.ftpUsername.empty())
            option(easy, CURLOPT_USERNAME, spec.ftpUsername.c_str());
        if (!spec.ftpPassword.empty())
            option(easy, CURLOPT_PASSWORD, spec.ftpPassword.c_str());
    }
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
                throw std::runtime_error("Cannot create Remote file transfer scheduler.");
            if (curl_multi_add_handle(value, easy) != CURLM_OK) {
                curl_multi_cleanup(value);
                value = nullptr;
                throw std::runtime_error("Cannot schedule Remote file transfer.");
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
            throw std::runtime_error("Remote file transfer scheduling failed.");
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
            throw std::runtime_error("Remote file transfer ended without a result.");
        }
        int descriptors = 0;
        if (curl_multi_poll(multi.value, nullptr, 0, 100, &descriptors) != CURLM_OK)
            throw std::runtime_error("Cannot wait for Remote file connection.");
    }
}
Result failure(CURLcode code, bool sftp) {
    if (sftp && (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSH))
        return {TransferOutcome::Attention, "SFTP server key or SSH authentication failed. Check the trusted "
                                            "SHA256 fingerprint and login method."};
    if (code == CURLE_LOGIN_DENIED)
        return {TransferOutcome::Attention, "Remote file login was rejected. Check the task credentials."};
    if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE ||
        code == CURLE_SSL_CONNECT_ERROR || code == CURLE_USE_SSL_FAILED)
        return {TransferOutcome::Attention,
                "FTPS certificate or encrypted connection failed. Encryption was not downgraded."};
    if (code == CURLE_REMOTE_FILE_NOT_FOUND || code == CURLE_FTP_COULDNT_RETR_FILE)
        return {TransferOutcome::Attention, "Remote file source is unavailable or access was denied."};
    const bool retry = code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
                       code == CURLE_OPERATION_TIMEDOUT || code == CURLE_RECV_ERROR ||
                       code == CURLE_SEND_ERROR || code == CURLE_PARTIAL_FILE;
    return {TransferOutcome::Failed,
            "Remote file transfer failed (network code " + std::to_string(code) + ").", retry};
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
        tag != (isSftpUrl(task.spec.url) ? "sftp-v1" : "ftp-v1") || url != task.spec.url ||
        hash != task.spec.expectedSha256 || size != remote.size || modified != remote.modified ||
        tls != task.spec.ftpExplicitTls)
        return 0;
    if (isSftpUrl(task.spec.url)) {
        std::string pin;
        if (!(meta >> std::quoted(pin)) || pin != canonicalSftpHostKey(task.spec.sftp.hostKeySha256))
            return 0;
    }
    std::ifstream saved(work / "offset-0");
    if (!(saved >> offset) || offset < 0 || offset > remote.size)
        return 0;
    const auto bytes = std::filesystem::file_size(work / "part-0", ec);
    if (ec || bytes < static_cast<std::uint64_t>(offset))
        return 0;
    return offset;
}
Result transfer(TransferKind expectedKind, const Task& task, const std::filesystem::path& work,
                TransferControl& control, TransferEvent& event,
                const std::function<void(TransferEvent)>& callback) {
    network::initializeCurl();
    const bool sftp = isSftpUrl(task.spec.url);
    if (sourceKind(task.spec) != expectedKind || (!isFtpUrl(task.spec.url) && !sftp))
        return {TransferOutcome::Attention, "The remote file adapter does not match the source protocol."};
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
        return failure(remote.code, sftp);
    event.total = remote.size;
    event.resumable = !task.spec.expectedSha256.empty() && remote.size >= 0 && remote.modified >= 0;
    std::filesystem::create_directories(work);
    auto offset = recoverOffset(task, work, remote);
    std::ostringstream identity;
    identity << (sftp ? "sftp-v1 " : "ftp-v1 ") << std::quoted(task.spec.url) << ' ' << remote.size << ' '
             << remote.modified << ' ' << std::quoted(task.spec.expectedSha256) << ' '
             << task.spec.ftpExplicitTls;
    if (sftp)
        identity << ' ' << std::quoted(canonicalSftpHostKey(task.spec.sftp.hostKeySha256));
    files::atomicText(work / "identity", identity.str());
    const auto part = work / "part-0";
    const bool protectedFtp = nativeFtpsCompatibility && !sftp &&
                              (task.spec.ftpExplicitTls || task.spec.url.starts_with("ftps://"));
    bool restarted = false, tls12 = false, transferred = false;
    CURLcode refusedSessionCode = CURLE_OK;
    // REST restart and a protocol-version transition are independent one-shot operations.
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (offset > 0)
            std::filesystem::resize_file(part, offset);
        File output;
        output.value = files::openFile(part, offset > 0 ? L"ab" : L"wb");
        if (!output.value)
            throw std::runtime_error("Cannot write remote file cache file.");
        struct Sink {
            FILE* file;
            std::int64_t bytes, limit;
            bool failed = false;
            TransferControl* control;
            const FtpReply* reply;
            bool requiresResume;
        } sink{output.value, offset, remote.size, false, &control, nullptr, protectedFtp && offset > 0};
        FtpReply reply; // Outlive Easy: cleanup may still deliver a control reply.
        sink.reply = &reply;
        ResumeCommand resume; // Outlive Easy, which retains the command list.
        Easy easy;
        configure(easy.value, task.spec, control);
        if (protectedFtp) {
            // Own metadata/Sink/final checks enforce length; let curl read the final FTP reply.
            option(easy.value, CURLOPT_IGNORE_CONTENT_LENGTH, 1L);
            option(easy.value, CURLOPT_HEADERFUNCTION,
                   +[](char* data, size_t size, size_t count, void* context) -> size_t {
                       if (count && size > std::numeric_limits<size_t>::max() / count)
                           return 0;
                       const auto bytes = size * count;
                       static_cast<FtpReply*>(context)->receive(std::string_view(data, bytes));
                       return bytes;
                   });
            option(easy.value, CURLOPT_HEADERDATA, &reply);
            if (tls12)
                option(easy.value, CURLOPT_SSLVERSION,
                       static_cast<long>(CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2));
        }
        option(
            easy.value, CURLOPT_WRITEFUNCTION,
            +[](char* data, size_t size, size_t count, void* opaque) -> size_t {
                auto& out = *static_cast<Sink*>(opaque);
                if (out.requiresResume && !out.reply->resumeAccepted) {
                    out.failed = true;
                    return 0;
                }
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
        if (offset > 0) {
            if (protectedFtp) {
                // Ignoring FTP SIZE bypasses curl's normal resume state. Issue REST
                // explicitly and require its 350 reply before accepting any payload.
                const auto command = "REST " + std::to_string(offset);
                resume.value = curl_slist_append(nullptr, command.c_str());
                if (!resume.value)
                    throw std::runtime_error("Cannot configure remote file resume.");
                option(easy.value, CURLOPT_PREQUOTE, resume.value);
            } else
                option(easy.value, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(offset));
        }
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
                    "Remote file output exceeded the expected length or could not be written."};
        long response = 0;
        if (protectedFtp && !tls12 && code != CURLE_OK && sink.bytes == offset && reply.requiresReuse &&
            curl_easy_getinfo(easy.value, CURLINFO_RESPONSE_CODE, &response) == CURLE_OK &&
            (response == 425 || response == 522)) {
            tls12 = true;
            refusedSessionCode = code;
            continue;
        }
        if (offset > 0 && !restarted &&
            (code == CURLE_RANGE_ERROR || code == CURLE_BAD_DOWNLOAD_RESUME ||
             code == CURLE_FTP_COULDNT_USE_REST ||
             (protectedFtp && code == CURLE_QUOTE_ERROR))) {
            // Some servers advertise a file but reject REST. Discard uncertain data and retry once.
            restarted = true;
            offset = 0;
            continue;
        }
        if (code != CURLE_OK) {
            if (tls12 && refusedSessionCode != CURLE_OK &&
                (code == CURLE_SSL_CONNECT_ERROR || code == CURLE_USE_SSL_FAILED))
                return failure(refusedSessionCode, sftp);
            return failure(code, sftp);
        }
        if (remote.size >= 0 && sink.bytes != remote.size)
            return {TransferOutcome::Failed,
                    "Remote file source ended before the expected length was received.", true};
        const auto after = probe(task.spec, control);
        if (isStopped(control))
            return stopped(control);
        if (after.code != CURLE_OK)
            return failure(after.code, sftp);
        if ((remote.size >= 0 && after.size != remote.size) ||
            (remote.modified >= 0 && after.modified != remote.modified))
            return {TransferOutcome::Attention,
                    "Remote file source changed during transfer. Restart the task."};
        transferred = true;
        break;
    }
    if (!transferred)
        return {TransferOutcome::Failed, "Remote file protocol negotiation did not complete."};
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
            throw std::runtime_error("Cannot read remote file cache for publication.");
        File output;
        output.value = files::openFile(stage, L"wb");
        if (!output.value)
            throw std::runtime_error("Cannot assemble remote file output on the destination volume.");
        std::array<char, 256 * 1024> buffer{};
        std::int64_t copied = 0;
        while (input) {
            if (isStopped(control))
                return stopped(control);
            input.read(buffer.data(), buffer.size());
            const auto count = input.gcount();
            if (count && fwrite(buffer.data(), 1, static_cast<size_t>(count), output.value) !=
                             static_cast<size_t>(count))
                throw std::runtime_error("Cannot write remote file output on the destination volume.");
            copied += count;
        }
        if (!input.eof() || copied != event.downloaded)
            throw std::runtime_error("Remote file cache changed during assembly.");
        files::syncFile(output.value);
        output.close();
    }
    if (!task.spec.expectedSha256.empty()) {
        auto expected = task.spec.expectedSha256;
        std::transform(expected.begin(), expected.end(), expected.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (sha256File(stage, &control) != expected)
            return {TransferOutcome::Attention,
                    "Remote file SHA-256 verification failed. Output was not published."};
    }
    if (control.scanBeforePublish) {
        const auto scan = control.scanBeforePublish(stage);
        if (scan.outcome != TransferOutcome::Complete)
            return {scan.outcome, scan.error};
    }
    event.phase = TaskState::Finalizing;
    callback(event);
    if (isStopped(control))
        return stopped(control);
    files::publishFile(stage, task.spec.destination);
    return {TransferOutcome::Complete, {}};
}
class RemoteFileTransfer final : public ITransferTransport {
    TransferKind kind_;

  public:
    explicit RemoteFileTransfer(TransferKind kind) : kind_(kind) {}
    void run(const Task& task, const std::filesystem::path& work, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        TransferEvent event;
        event.phase = TaskState::Probing;
        callback(event);
        Result result{TransferOutcome::Attention, {}};
        try {
            result = transfer(kind_, task, work, control, event, callback);
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
std::unique_ptr<ITransferTransport> makeFtpTransfer() {
    return std::make_unique<RemoteFileTransfer>(TransferKind::Ftp);
}
std::unique_ptr<ITransferTransport> makeSftpTransfer() {
    return std::make_unique<RemoteFileTransfer>(TransferKind::Sftp);
}
} // namespace maskpilot
