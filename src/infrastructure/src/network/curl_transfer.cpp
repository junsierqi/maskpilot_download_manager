#include "../filesystem/transfer_files.h"
#include "curl_runtime.h"
#include "http_layout.h"
#include "proxy_options.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <curl/curl.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <maskpilot/infrastructure/adapters.h>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <bcrypt.h>
#include <io.h>
#include <windows.h>
#else
#include <QCryptographicHash>
#include <unistd.h>
#endif

namespace maskpilot {
namespace {
using Clock = std::chrono::steady_clock;
std::string pathText(const std::filesystem::path& p) {
    auto s = p.u8string();
    return {s.begin(), s.end()};
}
using transfer_files::atomicText;
using transfer_files::openFile;
using transfer_files::replaceOwnedFile;
using transfer_files::syncFile;
std::string trim(std::string value) {
    while (!value.empty() &&
           (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
    auto start = value.find_first_not_of(" \t");
    return start == std::string::npos ? std::string() : value.substr(start);
}
std::int64_t number(const std::string& text) {
    std::int64_t result = -1;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    return error == std::errc() && end == text.data() + text.size() && result >= 0 ? result : -1;
}
struct Range {
    std::int64_t begin = -1, end = -1, total = -1;
};
Range parseRange(const std::string& text) {
    if (!text.starts_with("bytes "))
        return {};
    auto dash = text.find('-', 6), slash = text.find('/', 6);
    if (dash == std::string::npos || slash == std::string::npos || dash > slash)
        return {};
    Range r{number(text.substr(6, dash - 6)), number(text.substr(dash + 1, slash - dash - 1)),
            number(text.substr(slash + 1))};
    if (r.begin < 0 || r.end < r.begin || r.total <= r.end)
        return {};
    return r;
}
struct Headers {
    long status = 0;
    std::map<std::string, std::string> fields;
    bool complete = false, conflict = false, contentTypeInvalid = false;
    size_t bytes = 0;
    void line(std::string line) {
        bytes += line.size();
        if (bytes > 1024 * 1024)
            throw std::runtime_error("Response headers exceed the limit.");
        if (line.starts_with("HTTP/")) {
            fields.clear();
            complete = false;
            conflict = false;
            contentTypeInvalid = false;
            auto pos = line.find(' ');
            status = pos == std::string::npos ? 0 : std::strtol(line.c_str() + pos + 1, nullptr, 10);
            return;
        }
        if (line == "\r\n" || line == "\n") {
            complete = true;
            return;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos)
            return;
        auto name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto value = trim(line.substr(colon + 1));
        if ((name == "content-range" || name == "content-length" || name == "etag") &&
            fields.contains(name) && fields[name] != value)
            conflict = true;
        if (name == "content-type" &&
            (!validHttpResponseContentType(value) || (fields.contains(name) && fields[name] != value)))
            contentTypeInvalid = true;
        fields[name] = std::move(value);
    }
    std::string get(const std::string& name) const {
        auto it = fields.find(name);
        return it == fields.end() ? std::string() : it->second;
    }
};
// One worker owns the multi handle and this scalar; every validated response contributes.
// Unsafe or inconsistent metadata affects classification, never ordinary byte completion.
struct ResponseContentType {
    std::string probe, actual;
    bool invalid = false, sawActual = false;
    void seed(long status, const std::string& value, bool unsafe) {
        if (status == 200 || status == 206) {
            probe = value;
            invalid = unsafe;
        }
    }
    void observe(const Headers& headers) {
        const auto value = headers.get("content-type");
        if (headers.contentTypeInvalid || value.empty() || (!probe.empty() && value != probe) ||
            (sawActual && value != actual))
            invalid = true;
        if (!sawActual)
            actual = value;
        sawActual = true;
    }
    std::string value() const { return invalid ? std::string{} : (sawActual ? actual : probe); }
};
size_t headerCallback(char* data, size_t size, size_t count, void* context) {
    try {
        static_cast<Headers*>(context)->line(std::string(data, size * count));
        return size * count;
    } catch (...) {
        return 0;
    }
}
struct Easy {
    CURL* handle = curl_easy_init();
    curl_slist* list = nullptr;
    Easy() {
        if (!handle)
            throw std::runtime_error("Cannot initialize network connection.");
    }
    ~Easy() {
        if (list)
            curl_slist_free_all(list);
        if (handle)
            curl_easy_cleanup(handle);
    }
    Easy(const Easy&) = delete;
    Easy& operator=(const Easy&) = delete;
    void header(const std::string& text) {
        auto* next = curl_slist_append(list, text.c_str());
        if (!next)
            throw std::bad_alloc();
        list = next;
    }
};
void configure(Easy& easy, const DownloadSpec& spec, Headers& headers, TransferControl& control) {
    curl_easy_setopt(easy.handle, CURLOPT_URL, spec.url.c_str());
    // Redirects are resolved explicitly during probing so origin-bound headers cannot leak.
    curl_easy_setopt(easy.handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy.handle, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(easy.handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy.handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy.handle, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(easy.handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy.handle, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(easy.handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy.handle, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(easy.handle, CURLOPT_USERAGENT, spec.userAgent.c_str());
    curl_easy_setopt(easy.handle, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(easy.handle, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    curl_easy_setopt(easy.handle, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(easy.handle, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(easy.handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        easy.handle, CURLOPT_XFERINFOFUNCTION,
        +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            auto& c = *static_cast<TransferControl*>(context);
            return c.pause || c.cancel ? 1 : 0;
        });
    curl_easy_setopt(easy.handle, CURLOPT_XFERINFODATA, &control);
    network::configureProxy(easy.handle, spec);
    if (spec.httpRequest.method == HttpMethod::Post) {
        // Complete proxy authentication before the state-changing source request is sent.
        // HTTP proxies must permit CONNECT to the source port; never fall back to direct access.
        curl_easy_setopt(easy.handle, CURLOPT_HTTPPROXYTUNNEL, 1L);
        curl_easy_setopt(easy.handle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
        curl_easy_setopt(easy.handle, CURLOPT_HEADEROPT, static_cast<long>(CURLHEADER_SEPARATE));
        // Keep this buffer alive through completion. No implicit HTTP2 stream replay or reused connection.
        curl_easy_setopt(easy.handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(easy.handle, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(easy.handle, CURLOPT_FORBID_REUSE, 1L);
        curl_easy_setopt(easy.handle, CURLOPT_POSTFIELDS, spec.httpRequest.body.c_str());
        curl_easy_setopt(easy.handle, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(spec.httpRequest.body.size()));
        easy.header("Content-Type: " + spec.httpRequest.contentType);
        easy.header("Expect:");
    }
    if (!spec.referrer.empty())
        curl_easy_setopt(easy.handle, CURLOPT_REFERER, spec.referrer.c_str());
    if (!spec.cookie.empty())
        easy.header("Cookie: " + spec.cookie);
    if (!spec.authorization.empty())
        easy.header("Authorization: " + spec.authorization);
    for (const auto& [name, value] : spec.headers)
        easy.header(name + ": " + value);
}
std::string urlPart(const std::string& value, CURLUPart part) {
    auto* url = curl_url();
    if (!url)
        throw std::bad_alloc();
    char* output = nullptr;
    std::string result;
    if (curl_url_set(url, CURLUPART_URL, value.c_str(), 0) == CURLUE_OK &&
        curl_url_get(url, part, &output, CURLU_DEFAULT_PORT) == CURLUE_OK) {
        result = output;
        curl_free(output);
    }
    curl_url_cleanup(url);
    return result;
}
std::string origin(const std::string& value) {
    auto scheme = urlPart(value, CURLUPART_SCHEME), host = urlPart(value, CURLUPART_HOST);
    const auto lower = [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    };
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), lower);
    std::transform(host.begin(), host.end(), host.begin(), lower);
    const auto port = number(urlPart(value, CURLUPART_PORT));
    if ((scheme != "http" && scheme != "https") || host.empty() || port < 1 || port > 65535)
        throw std::runtime_error("Invalid HTTP origin.");
    return scheme + "://" + host + ":" + std::to_string(port);
}
std::string resolveUrl(const std::string& base, const std::string& location) {
    auto* url = curl_url();
    if (!url)
        throw std::bad_alloc();
    char* output = nullptr;
    std::string result;
    if (curl_url_set(url, CURLUPART_URL, base.c_str(), 0) == CURLUE_OK &&
        curl_url_set(url, CURLUPART_URL, location.c_str(), 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_URL, &output, 0) == CURLUE_OK) {
        result = output;
        curl_free(output);
    }
    curl_url_cleanup(url);
    return result;
}
struct Probe {
    std::int64_t total = -1;
    bool ranges = false;
    std::string etag, effectiveUrl;
    std::string contentType;
    bool contentTypeInvalid = false;
    DownloadSpec effectiveSpec;
    long status = 0;
    int retryAfter = 0;
    CURLcode code = CURLE_OK;
};
Probe probeOnce(const DownloadSpec& spec, TransferControl& control, Headers& headers) {
    Easy easy;
    configure(easy, spec, headers, control);
    curl_easy_setopt(easy.handle, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(easy.handle, CURLOPT_HTTPHEADER, easy.list);
    curl_easy_setopt(
        easy.handle, CURLOPT_WRITEFUNCTION, +[](char*, size_t size, size_t count, void* opaque) -> size_t {
            auto& control = *static_cast<TransferControl*>(opaque);
            if (count && size > std::numeric_limits<size_t>::max() / count)
                return 0;
            const auto bytes = size * count;
            if (!control.payloadBudget)
                return 0;
            const auto accepted = control.payloadBudget->accept(bytes);
            if (accepted < bytes)
                control.pause = true;
            // Consume the one-byte range probe; ignored-range bodies remain bounded to one callback.
            return accepted == bytes && bytes == 1 ? bytes : 0;
        });
    curl_easy_setopt(easy.handle, CURLOPT_WRITEDATA, &control);
    auto code = curl_easy_perform(easy.handle);
    Probe result;
    result.code = code;
    result.status = headers.status;
    result.etag = headers.get("etag");
    result.contentType = headers.get("content-type");
    result.contentTypeInvalid = headers.contentTypeInvalid;
    result.retryAfter =
        static_cast<int>(std::clamp<std::int64_t>(number(headers.get("retry-after")), 0, 3600));
    char* effective = nullptr;
    curl_easy_getinfo(easy.handle, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective)
        result.effectiveUrl = effective;
    if (headers.conflict || (!headers.complete && code == CURLE_OK))
        throw std::runtime_error("Invalid server response headers.");
    if (headers.status == 206) {
        auto r = parseRange(headers.get("content-range"));
        if (r.begin != 0 || r.end != 0 || r.total < 1)
            throw std::runtime_error("Server returned an invalid probe range.");
        result.total = r.total;
        result.ranges = true;
    } else if (headers.status == 200)
        result.total = number(headers.get("content-length"));
    else if (headers.status == 416 && headers.get("content-range") == "bytes */0") {
        result.total = 0;
        result.ranges = true;
    }
    if (!headers.get("content-encoding").empty() && headers.get("content-encoding") != "identity")
        throw std::runtime_error("Server ignored the requested byte encoding.");
    return result;
}
Probe probe(const DownloadSpec& initial, TransferControl& control, bool exactOriginGrant) {
    auto spec = initial;
    for (int redirect = 0; redirect <= 5; ++redirect) {
        Headers headers;
        auto result = probeOnce(spec, control, headers);
        if (control.pause || control.cancel) {
            result.effectiveSpec = spec;
            return result;
        }
        if (result.status == 301 || result.status == 302 || result.status == 303 || result.status == 307 ||
            result.status == 308) {
            auto next = resolveUrl(spec.url, headers.get("location"));
            if (exactOriginGrant && next != initial.url)
                throw std::runtime_error("Redirect requires fresh source authorization for the new URL.");
            auto candidate = spec;
            candidate.url = next;
            if (!validateSpec(candidate).empty())
                throw std::runtime_error("Server returned an invalid redirect.");
            if (initial.sameOriginRedirectOnly && origin(initial.url) != origin(next))
                throw std::runtime_error("Redirect to a different origin was blocked by this task's policy.");
            if (spec.url.starts_with("https://") && next.starts_with("http://"))
                throw std::runtime_error("Insecure HTTPS downgrade redirect was blocked.");
            if (origin(spec.url) != origin(next)) {
                candidate.cookie.clear();
                candidate.authorization.clear();
                candidate.headers.clear();
                candidate.referrer.clear();
            }
            spec = std::move(candidate);
            continue;
        }
        result.effectiveSpec = std::move(spec);
        return result;
    }
    throw std::runtime_error("Too many download redirects.");
}
bool strongEtag(const std::string& etag) {
    return etag.size() >= 2 && etag.front() == '"' && etag.back() == '"' &&
           etag.find_first_of("\r\n") == std::string::npos;
}
bool retryStatus(long status) {
    return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}
bool retryCode(CURLcode code) {
    return code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT || code == CURLE_RECV_ERROR ||
           code == CURLE_SEND_ERROR || code == CURLE_OPERATION_TIMEDOUT || code == CURLE_PARTIAL_FILE ||
           code == CURLE_HTTP2 || code == CURLE_HTTP2_STREAM;
}
struct Sink {
    int extentId = 0;
    bool attached = false;
    TransferControl* control = nullptr;
    ResponseContentType* contentType = nullptr;
    Easy easy;
    Headers headers;
    FILE* file = nullptr;
    std::filesystem::path part, offsetPath;
    std::int64_t begin = 0, end = -1, written = 0, total = -1, partOffset = 0, maxStoredBytes = 0;
    bool storedLimitExceeded = false;
    bool post = false, ranged = false, validated = false, finished = false;
    std::string expectedEtag, error;
    Clock::time_point checkpointAt = Clock::now();
    ~Sink() {
        if (file)
            fclose(file);
    }
    void checkpoint() {
        if (!file)
            return;
        syncFile(file);
        atomicText(offsetPath, std::to_string(partOffset + written));
        checkpointAt = Clock::now();
    }
    bool validate() {
        if (validated) {
            if (error.empty() && contentType)
                contentType->observe(headers);
            return error.empty();
        }
        if (!headers.complete)
            return false;
        validated = true;
        if (headers.conflict) {
            error = "Conflicting response headers.";
            return false;
        }
        if (headers.status >= 400) {
            error = "Server rejected download (HTTP " + std::to_string(headers.status) + ").";
            return false;
        }
        if (ranged) {
            auto r = parseRange(headers.get("content-range"));
            if (headers.status != 206 || r.begin != begin || r.end != end - 1 || r.total != total) {
                error = "Server changed or ignored the requested range. Resume was stopped safely.";
                return false;
            }
            if (headers.get("etag") != expectedEtag) {
                error = "Remote file identity changed. Create a new task to restart safely.";
                return false;
            }
        } else if (post ? (headers.status < 200 || headers.status >= 300) : headers.status != 200) {
            error = "HTTP response was not successful (" + std::to_string(headers.status) + ").";
            return false;
        }
        if (!headers.get("content-encoding").empty() && headers.get("content-encoding") != "identity") {
            error = "Unexpected encoded response.";
            return false;
        }
        if (contentType)
            contentType->observe(headers);
        return true;
    }
    static size_t write(char* data, size_t size, size_t count, void* context) {
        auto& sink = *static_cast<Sink*>(context);
        if (count && size > std::numeric_limits<size_t>::max() / count)
            return 0;
        auto length = size * count;
        try {
            if (sink.control && sink.control->payloadBudget) {
                const auto accepted = sink.control->payloadBudget->accept(length);
                if (accepted < length)
                    sink.control->pause = true;
                length = accepted;
                if (!length)
                    return 0;
            }
            if (sink.headers.status >= 300 && sink.headers.status < 400)
                return sink.post ? 0 : length;
            if (!sink.validate())
                return 0;
            if (sink.end >= 0 && static_cast<std::uint64_t>(length) >
                                     static_cast<std::uint64_t>(sink.end - sink.begin - sink.written)) {
                sink.error = "Response exceeded the requested file bounds.";
                return 0;
            }
            if (sink.maxStoredBytes > 0 &&
                (sink.partOffset > sink.maxStoredBytes ||
                 sink.written > sink.maxStoredBytes - sink.partOffset ||
                 static_cast<std::uint64_t>(length) >
                     static_cast<std::uint64_t>(sink.maxStoredBytes - sink.partOffset - sink.written))) {
                sink.storedLimitExceeded = true;
                sink.error = "Response exceeds this task's stored-byte limit.";
                return 0;
            }
            auto saved = fwrite(data, 1, length, sink.file);
            sink.written += static_cast<std::int64_t>(saved);
            if (saved != length) {
                sink.error = "Unable to write file. Check disk space and permissions.";
                return 0;
            }
            if (Clock::now() - sink.checkpointAt > std::chrono::seconds(1))
                sink.checkpoint();
            return length;
        } catch (const std::exception&) {
            sink.error = "Unable to commit downloaded data. Check disk space.";
            return 0;
        }
    }
};
class CurlTransfer final : public ITransferTransport {
  public:
    void run(const Task& task, const std::filesystem::path& work, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        network::initializeCurl();
        if (isSftpUrl(task.spec.url)) {
            makeSftpTransfer()->run(task, work, control, callback);
            return;
        }
        if (isFtpUrl(task.spec.url)) {
            makeFtpTransfer()->run(task, work, control, callback);
            return;
        }
        const bool post = task.spec.httpRequest.method == HttpMethod::Post;
        TransferEvent event;
        ResponseContentType contentType;
        auto deliver = [&] {
            event.responseContentType = contentType.value();
            callback(event);
        };
        event.phase = TaskState::Probing;
        auto report = [&](TransferOutcome result, std::string error = {}, bool retry = false) {
            event.outcome = result;
            event.error = std::move(error);
            event.retryable = retry && !post;
            deliver();
        };
        deliver();
        auto stopped = [&]() {
            if (control.cancel) {
                report(TransferOutcome::Cancelled);
                return true;
            }
            if (control.pause) {
                report(TransferOutcome::Paused);
                return true;
            }
            return false;
        };
        Probe remote;
        try {
            auto spec = task.spec;
            spec.originAuthenticationRequired =
                task.originAuthenticationRequired || requiresOriginAuthentication(spec);
            if (const auto error = validateOriginAuthentication(spec, true); !error.empty())
                throw std::invalid_argument(error);
            if (const auto error = validateHttpRequest(spec.httpRequest, true); !error.empty())
                throw std::invalid_argument(error);
            if (const auto error = validateSpec(spec); !error.empty())
                throw std::invalid_argument(error);
            if (post) {
                if (task.ownerId != "standalone" || !task.projectId.empty())
                    throw std::invalid_argument("POST requires a standalone task.");
                if (std::filesystem::exists(spec.destination))
                    throw std::invalid_argument("Destination already exists. POST was not submitted.");
                std::filesystem::create_directories(spec.destination.parent_path());
                auto stage = spec.destination;
                stage += L".maskpilot-" + std::filesystem::path(task.id).wstring() + L".tmp";
                // Check the actual publication directory before a server operation can be created.
                // Append mode preserves any prior owned stage; the new response replaces it at assembly.
                auto* preflight = openFile(stage, L"ab");
                if (!preflight)
                    throw std::invalid_argument(
                        "Destination staging file is unavailable. POST was not submitted.");
                fclose(preflight);
                remote.status = 200;
                remote.effectiveSpec = std::move(spec);
                remote.effectiveUrl = remote.effectiveSpec.url;
            } else
                remote = probe(spec, control, requiresOriginAuthentication(spec));
        } catch (const std::exception& e) {
            if (!stopped())
                report(TransferOutcome::Attention, e.what());
            return;
        }
        if (stopped())
            return;
        event.total = remote.total;
        event.retryAfter = remote.retryAfter;
        if (remote.status == 0 && remote.code != CURLE_OK) {
            report(TransferOutcome::Failed,
                   "Network request failed: " + std::string(curl_easy_strerror(remote.code)) + ".",
                   retryCode(remote.code));
            return;
        }
        if (remote.status != 200 && remote.status != 206 && !(remote.status == 416 && remote.total == 0)) {
            report(remote.status == 401 || remote.status == 403 ? TransferOutcome::Attention
                                                              : TransferOutcome::Failed,
                 "Server rejected download (HTTP " + std::to_string(remote.status) + ").",
                 retryStatus(remote.status) || retryCode(remote.code));
            return;
        }
        auto observation = remote.effectiveSpec;
        observation.url = remote.effectiveUrl;
        if (remote.effectiveUrl.empty() || remote.effectiveUrl.size() > 32768 ||
            !validateSpec(observation).empty()) {
            report(TransferOutcome::Attention, "Server response URL is invalid.");
            return;
        }
        if (!post)
            event.responseUrl = remote.effectiveUrl;
        contentType.seed(remote.status, remote.contentType, remote.contentTypeInvalid);
        if (task.spec.maxStoredBytes > 0 && remote.total > task.spec.maxStoredBytes) {
            report(TransferOutcome::Attention, "Resource exceeds this task's stored-byte limit.");
            return;
        }
        bool resumable = remote.ranges && strongEtag(remote.etag) && remote.total >= 0;
        if (resumable) {
            try {
                // Quoting can expand unusual server tags. Preserve downloading
                // even when this identity cannot fit bounded recovery metadata.
                const int count = remote.total >= 1024 * 1024 ? task.spec.connections : 1;
                (void)http::encodeLayout(
                    http::initialLayout(remote.etag, remote.effectiveUrl, remote.total, count));
            } catch (const std::invalid_argument&) {
                resumable = false;
            }
        }
        event.resumable = resumable;
        const auto metadata = work / "identity";
        std::vector<http::Extent> ranges;
        std::map<int, std::int64_t> offsets;
        std::string failure;
        long postStatus = 0;
        std::string postLocation;
        bool postRedirectReady = false;
        bool integrityFailure = false, retryable = false;
        try {
            std::filesystem::create_directories(work);
            auto removeExtentArtifacts = [&](int id) {
                for (const auto* prefix : {"part-", "offset-"}) {
                    auto path = work / (std::string(prefix) + std::to_string(id));
                    for (int temporary = 0; temporary < 2; ++temporary) {
                        std::error_code error;
                        std::filesystem::remove(path, error);
                        if (error)
                            throw std::runtime_error("Cannot reset owned HTTP recovery files. Close files "
                                                     "using this task and retry.");
                        path += L".new";
                    }
                }
            };
            http::Layout layout;
            bool reuse = false;
            if (resumable) {
                if (const auto saved = http::readLayout(metadata);
                    saved && saved->etag == remote.etag && saved->effectiveUrl == remote.effectiveUrl &&
                    saved->total == remote.total) {
                    layout = *saved;
                    reuse = true;
                    if (layout.legacy) {
                        layout.legacy = false;
                        layout.generation = 1;
                        http::saveLayout(metadata, layout);
                    }
                }
            }
            if (!reuse) {
                // Never install a new identity while any old numeric checkpoint can
                // be mistaken for one of its stable IDs. Unrelated files are untouched.
                for (std::size_t id = 0; id < maxRecoveryExtents; ++id)
                    removeExtentArtifacts(static_cast<int>(id));
                for (const auto* name : {"identity", "identity.new"}) {
                    std::error_code error;
                    std::filesystem::remove(work / name, error);
                    if (error)
                        throw std::runtime_error(
                            "Cannot reset HTTP recovery identity. Existing files were preserved.");
                }
                if (resumable) {
                    const int count = remote.total >= 1024 * 1024 ? task.spec.connections : 1;
                    layout = http::initialLayout(remote.etag, remote.effectiveUrl, remote.total, count);
                    http::saveLayout(metadata, layout);
                }
            }
            ranges = resumable ? layout.extents : std::vector<http::Extent>{{0, 0, remote.total}};
            for (const auto& extent : ranges) {
                const auto part = work / ("part-" + std::to_string(extent.id));
                const auto sidecar = work / ("offset-" + std::to_string(extent.id));
                std::int64_t prefix = 0;
                if (reuse && std::filesystem::exists(part) && std::filesystem::exists(sidecar)) {
                    std::error_code error;
                    const auto sidecarSize = std::filesystem::file_size(sidecar, error);
                    if (!error && sidecarSize > 0 && sidecarSize <= 64) {
                        std::ifstream input(sidecar);
                        input >> prefix;
                        if (!input || prefix < 0 || prefix > extent.end - extent.begin)
                            prefix = 0;
                        else {
                            input >> std::ws;
                            if (!input.eof() ||
                                static_cast<std::uint64_t>(prefix) > std::filesystem::file_size(part))
                                prefix = 0;
                        }
                    }
                }
                if (std::filesystem::exists(part))
                    std::filesystem::resize_file(part, static_cast<std::uintmax_t>(prefix));
                offsets.emplace(extent.id, prefix);
                if (extent.end == extent.begin && !std::filesystem::exists(part)) {
                    auto* empty = openFile(part, L"wb");
                    if (!empty)
                        throw std::runtime_error("Cannot create the empty downloaded part.");
                    try {
                        syncFile(empty);
                    } catch (...) {
                        fclose(empty);
                        throw;
                    }
                    fclose(empty);
                }
            }
            std::map<int, std::unique_ptr<Sink>> active;
            auto* multi = curl_multi_init();
            if (!multi)
                throw std::runtime_error("Cannot initialize transfer scheduler.");
            struct MultiGuard {
                CURLM*& multi;
                std::map<int, std::unique_ptr<Sink>>& active;
                std::map<int, std::int64_t>& offsets;
                void close(bool report) {
                    std::exception_ptr failed;
                    if (multi) {
                        for (auto& [id, sink] : active)
                            if (sink->attached) {
                                if (curl_multi_remove_handle(multi, sink->easy.handle) != CURLM_OK && !failed)
                                    failed = std::make_exception_ptr(
                                        std::runtime_error("Cannot release transfer connection."));
                                sink->attached = false;
                            }
                        curl_multi_cleanup(multi);
                        multi = nullptr;
                    }
                    for (auto& [id, sink] : active) {
                        try {
                            sink->checkpoint();
                            offsets.at(id) = sink->partOffset + sink->written;
                        } catch (...) {
                            if (!failed)
                                failed = std::current_exception();
                        }
                        if (sink->file) {
                            fclose(sink->file);
                            sink->file = nullptr;
                        }
                    }
                    active.clear();
                    if (report && failed)
                        std::rethrow_exception(failed);
                }
                ~MultiGuard() { close(false); }
            } guard{multi, active, offsets};
            bool streamFinished = false, splittingEnabled = true;
            auto handleLimit = [&] {
                const int configured = resumable ? std::clamp(task.spec.connections, 1, 8) : 1;
                const auto assigned = control.speedLimit.load();
                return assigned > 0 ? static_cast<int>(std::min<std::int64_t>(configured, assigned))
                                    : configured;
            };
            auto start = [&](const http::Extent& extent) {
                auto sink = std::make_unique<Sink>();
                sink->extentId = extent.id;
                sink->control = &control;
                sink->contentType = &contentType;
                sink->part = work / ("part-" + std::to_string(extent.id));
                sink->offsetPath = work / ("offset-" + std::to_string(extent.id));
                sink->partOffset = offsets.at(extent.id);
                sink->begin = extent.begin + sink->partOffset;
                sink->end = extent.end;
                sink->total = remote.total;
                sink->maxStoredBytes = task.spec.maxStoredBytes;
                sink->ranged = resumable;
                sink->post = post;
                sink->expectedEtag = remote.etag;
                sink->file = openFile(sink->part, L"ab");
                if (!sink->file)
                    throw std::runtime_error("Cannot open temporary file.");
                configure(sink->easy, remote.effectiveSpec, sink->headers, control);
                if (resumable) {
                    const auto range = std::to_string(sink->begin) + "-" + std::to_string(extent.end - 1);
                    curl_easy_setopt(sink->easy.handle, CURLOPT_RANGE, range.c_str());
                    sink->easy.header("If-Range: " + remote.etag);
                }
                curl_easy_setopt(sink->easy.handle, CURLOPT_HTTPHEADER, sink->easy.list);
                curl_easy_setopt(sink->easy.handle, CURLOPT_WRITEFUNCTION, Sink::write);
                curl_easy_setopt(sink->easy.handle, CURLOPT_WRITEDATA, sink.get());
                curl_easy_setopt(sink->easy.handle, CURLOPT_PRIVATE, sink.get());
                auto& owned = active.emplace(extent.id, std::move(sink)).first->second;
                if (curl_multi_add_handle(multi, owned->easy.handle) != CURLM_OK)
                    throw std::runtime_error("Cannot queue transfer connection.");
                owned->attached = true;
            };
            auto fill = [&] {
                for (const auto& extent : ranges) {
                    if (active.size() >= static_cast<std::size_t>(handleLimit()))
                        break;
                    const bool unfinished =
                        extent.end < 0 ? !streamFinished : offsets.at(extent.id) < extent.end - extent.begin;
                    if (unfinished && !active.contains(extent.id))
                        start(extent);
                }
            };
            auto detach = [&](int id) {
                auto& sink = *active.at(id);
                if (curl_multi_remove_handle(multi, sink.easy.handle) != CURLM_OK)
                    throw std::runtime_error("Cannot detach transfer connection for recovery.");
                sink.attached = false;
                sink.checkpoint();
                offsets.at(id) = sink.partOffset + sink.written;
                fclose(sink.file);
                sink.file = nullptr;
                active.erase(id);
            };
            auto downloaded = [&] {
                std::int64_t bytes = 0;
                for (const auto& [id, prefix] : offsets)
                    bytes += prefix;
                for (const auto& [id, sink] : active)
                    bytes += sink->written;
                return bytes;
            };
            auto lastEvent = Clock::now(), previousTime = lastEvent;
            std::int64_t previousBytes = downloaded();
            for (;;) {
                if (control.pause || control.cancel || !failure.empty())
                    break;
                // A positive byte/s allocation must not become one byte/s on every connection.
                // Detach only between multi calls, preserving each extent's durable prefix.
                while (active.size() > static_cast<std::size_t>(handleLimit()))
                    detach(active.rbegin()->first);
                fill();
                if (active.empty())
                    break;
                const auto limit = control.speedLimit.load();
                for (auto& [id, sink] : active)
                    curl_easy_setopt(sink->easy.handle, CURLOPT_MAX_RECV_SPEED_LARGE,
                                     static_cast<curl_off_t>(
                                         limit > 0 ? std::max<std::int64_t>(
                                                         1, limit / static_cast<std::int64_t>(active.size()))
                                                   : 0));
                int running = 0;
                if (curl_multi_perform(multi, &running) != CURLM_OK) {
                    failure = "Network scheduler failed.";
                    retryable = true;
                    break;
                }
                int remaining = 0;
                while (auto* message = curl_multi_info_read(multi, &remaining)) {
                    if (message->msg != CURLMSG_DONE)
                        continue;
                    // CURLMsg may be invalidated by remove_handle; copy everything first.
                    const auto result = message->data.result;
                    auto* easy = message->easy_handle;
                    Sink* sink = nullptr;
                    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &sink);
                    if (!sink)
                        throw std::runtime_error("Transfer completion had no owned extent.");
                    const int id = sink->extentId;
                    if (post) {
                        postStatus = sink->headers.status;
                        postLocation = sink->headers.get("location");
                        postRedirectReady = sink->headers.complete && !sink->headers.conflict;
                        if (sink->headers.complete)
                            event.responseUrl = remote.effectiveUrl;
                    }
                    if (!control.pause && !control.cancel && failure.empty()) {
                        const bool valid = sink->validate();
                        if (!sink->error.empty()) {
                            failure = sink->error;
                            integrityFailure = sink->storedLimitExceeded ||
                                               (sink->ranged &&
                                                (sink->headers.status == 200 || sink->headers.status == 206 ||
                                                 sink->headers.status == 416)) ||
                                               sink->headers.status == 401 || sink->headers.status == 403;
                            retryable = retryStatus(sink->headers.status);
                            event.retryAfter = static_cast<int>(
                                std::clamp<std::int64_t>(number(sink->headers.get("retry-after")), 0, 3600));
                        } else if (result != CURLE_OK) {
                            failure = "Connection interrupted (" + std::to_string(result) + ").";
                            retryable = retryCode(result);
                        } else if (!valid || (sink->end >= 0 && sink->written != sink->end - sink->begin)) {
                            failure = "Response ended before the requested bytes were received.";
                            retryable = true;
                        }
                    }
                    detach(id);
                    if (!resumable)
                        streamFinished = true;
                }
                event.downloaded = downloaded();
                const auto now = Clock::now();
                if (now - lastEvent >= std::chrono::milliseconds(200) || active.empty()) {
                    const auto elapsed = std::chrono::duration<double>(now - previousTime).count();
                    if (elapsed >= 0.5) {
                        event.speed = static_cast<std::int64_t>((event.downloaded - previousBytes) / elapsed);
                        previousBytes = event.downloaded;
                        previousTime = now;
                    }
                    event.phase = TaskState::Downloading;
                    deliver();
                    lastEvent = now;
                }
                if (control.pause || control.cancel || !failure.empty())
                    break;
                fill();
                while (resumable && splittingEnabled &&
                       active.size() < static_cast<std::size_t>(handleLimit()) &&
                       layout.extents.size() < maxRecoveryExtents) {
                    int victim = -1;
                    std::int64_t largest = 2 * http::minSplitBytes - 1;
                    for (const auto& extent : ranges) {
                        const auto found = active.find(extent.id);
                        if (found == active.end())
                            continue;
                        const auto tail =
                            extent.end - extent.begin - found->second->partOffset - found->second->written;
                        if (tail > largest) {
                            largest = tail;
                            victim = extent.id;
                        }
                    }
                    if (victim < 0)
                        break;
                    detach(victim); // durable prefix and closed FILE precede layout ownership change
                    if (control.pause || control.cancel)
                        break;
                    const auto next = http::splitLayout(layout, victim, offsets.at(victim));
                    if (!next) {
                        splittingEnabled = false;
                        break;
                    }
                    // A legacy or externally interrupted cache can contain orphan
                    // numeric files. The old manifest does not own this fresh ID;
                    // clear its entire namespace before publishing the new owner.
                    for (const auto& extent : next->extents)
                        if (!offsets.contains(extent.id))
                            removeExtentArtifacts(extent.id);
                    try {
                        http::saveLayout(metadata, *next);
                    } catch (const std::exception&) {
                        throw std::runtime_error(
                            "Cannot commit adaptive recovery layout. Existing checkpoints were preserved.");
                    }
                    layout = *next;
                    ranges = layout.extents;
                    for (const auto& extent : ranges)
                        offsets.try_emplace(extent.id, 0);
                    // New child files are opened only after the complete layout commits.
                    fill();
                }
                int descriptors = 0;
                if (!active.empty())
                    curl_multi_poll(multi, nullptr, 0, 100, &descriptors);
            }
            guard.close(true);
            event.downloaded = 0;
            for (const auto& [id, prefix] : offsets)
                event.downloaded += prefix;
        } catch (const std::exception& exception) {
            failure = exception.what();
            integrityFailure = true; // Recovery/commit failure preserves owned parts for retry.
            retryable = false;
        }
        if (stopped())
            return;
        if (post && postStatus >= 300 && postStatus < 400) {
            try {
                if (!postRedirectReady || (postStatus != 301 && postStatus != 302 && postStatus != 303))
                    throw std::invalid_argument("POST redirect would repeat submission and was refused.");
                const auto target = resolveUrl(remote.effectiveUrl, postLocation);
                if (postLocation.empty() || target.empty() || origin(target) != origin(remote.effectiveUrl))
                    throw std::invalid_argument("POST redirect leaves the authorized source origin.");
                auto next = task;
                next.spec.url = target;
                next.spec.httpRequest = {};
                next.spec.connections = 1;
                next.spec.sameOriginRedirectOnly = true;
                // A redirected result is retrieved as GET. The original POST is never repeated.
                run(next, work, control, [&](TransferEvent result) {
                    result.retryable = false;
                    result.resumable = false;
                    callback(std::move(result));
                });
            } catch (const std::exception&) {
                report(TransferOutcome::Attention, "POST redirect was refused. Submission may have reached the "
                                                 "server; review before retrying.");
            }
            return;
        }
        if (!failure.empty()) {
            if (post)
                failure += " POST may have reached the server; confirm a new submission before retrying.";
            report(integrityFailure ? TransferOutcome::Attention : TransferOutcome::Failed, failure, retryable);
            return;
        }
        if (task.spec.maxStoredBytes > 0 && event.downloaded > task.spec.maxStoredBytes) {
            report(TransferOutcome::Attention, "Downloaded resource exceeds this task's stored-byte limit.");
            return;
        }
        if (remote.total >= 0 && event.downloaded != remote.total) {
            report(TransferOutcome::Failed, "Downloaded byte count does not match the resource.");
            return;
        }
        if (remote.total < 0)
            event.total = event.downloaded;
        event.phase = TaskState::Verifying;
        event.speed = 0;
        deliver();
        std::filesystem::create_directories(task.spec.destination.parent_path());
        auto stage = task.spec.destination;
        stage += L".maskpilot-" + std::filesystem::path(task.id).wstring() + L".tmp";
        FILE* output = openFile(stage, L"wb");
        if (!output) {
            report(TransferOutcome::Attention, "Cannot create destination file. Check disk space and access.");
            return;
        }
        try {
            std::array<char, 128 * 1024> buffer{};
            for (const auto& extent : ranges) {
                std::ifstream input(work / ("part-" + std::to_string(extent.id)), std::ios::binary);
                if (!input)
                    throw std::runtime_error("A downloaded part is missing.");
                while (input) {
                    if (control.pause || control.cancel) {
                        fclose(output);
                        output = nullptr;
                        std::error_code ec;
                        std::filesystem::remove(stage, ec);
                        stopped();
                        return;
                    }
                    input.read(buffer.data(), buffer.size());
                    auto length = input.gcount();
                    if (length > 0 && fwrite(buffer.data(), 1, static_cast<size_t>(length), output) !=
                                          static_cast<size_t>(length))
                        throw std::runtime_error("Cannot assemble file. Check disk space.");
                }
                if (!input.eof())
                    throw std::runtime_error("Cannot read downloaded part.");
            }
            syncFile(output);
            fclose(output);
            output = nullptr;
            const auto finalSize = std::filesystem::file_size(stage);
            if (task.spec.maxStoredBytes > 0 &&
                finalSize > static_cast<std::uint64_t>(task.spec.maxStoredBytes))
                throw std::runtime_error("Assembled resource exceeds this task's stored-byte limit.");
            if (finalSize != static_cast<std::uint64_t>(event.total))
                throw std::runtime_error("Assembled file length is invalid.");
            if (!task.spec.expectedSha256.empty()) {
                auto expected = task.spec.expectedSha256;
                std::transform(expected.begin(), expected.end(), expected.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (sha256File(stage, &control) != expected) {
                    report(TransferOutcome::Attention,
                         "SHA-256 verification failed. The file was not published.");
                    return;
                }
            }
            if (control.scanBeforePublish) {
                const auto scan = control.scanBeforePublish(stage);
                if (scan.outcome != TransferOutcome::Complete) {
                    report(scan.outcome, scan.error);
                    return;
                }
            }
            event.phase = TaskState::Finalizing;
            deliver();
            if (std::filesystem::exists(task.spec.destination)) {
                if (sha256File(stage, &control) == sha256File(task.spec.destination, &control)) {
                    if (stopped())
                        return;
                    std::filesystem::remove(stage);
                    report(TransferOutcome::Complete);
                    return;
                }
                report(TransferOutcome::Attention, "Destination already exists. Existing file was preserved.");
                return;
            }
            if (stopped())
                return;
#ifdef _WIN32
            if (!MoveFileExW(stage.c_str(), task.spec.destination.c_str(), MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error(
                    "Cannot publish the final file. Existing files are never overwritten.");
#else
            // Hard link provides no-replace publication on the same volume.
            std::filesystem::create_hard_link(stage, task.spec.destination);
            std::filesystem::remove(stage);
#endif
            report(TransferOutcome::Complete);
        } catch (const std::exception& e) {
            if (output)
                fclose(output);
            if (!stopped())
                report(TransferOutcome::Attention, e.what());
        }
    }
};
} // namespace
std::unique_ptr<ITransferTransport> makeCurlTransfer() { return std::make_unique<CurlTransfer>(); }
std::string sha256File(const std::filesystem::path& path, const TransferControl* control,
                       const std::function<bool()>& shouldStop) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize checksum.");
    struct Cleanup {
        BCRYPT_ALG_HANDLE& a;
        BCRYPT_HASH_HANDLE& h;
        ~Cleanup() {
            if (h)
                BCryptDestroyHash(h);
            if (a)
                BCryptCloseAlgorithmProvider(a, 0);
        }
    } cleanup{algorithm, hash};
    std::array<unsigned char, 32> digest{};
    // CNG owns object storage until BCryptDestroyHash, including all exceptional exits.
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot initialize checksum.");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read file for checksum.");
    std::array<char, 128 * 1024> buffer{};
    while (input) {
        if ((control && (control->pause || control->cancel)) || (shouldStop && shouldStop()))
            throw std::runtime_error("Checksum cancelled.");
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count > 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0)
            throw std::runtime_error("Checksum calculation failed.");
    }
    if (!input.eof() || BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("Checksum calculation failed.");
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto c : digest)
        out << std::setw(2) << static_cast<int>(c);
    return out.str();
#else
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read file for checksum.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 128 * 1024> buffer{};
    while (input) {
        if ((control && (control->pause || control->cancel)) || (shouldStop && shouldStop()))
            throw std::runtime_error("Checksum cancelled.");
        input.read(buffer.data(), buffer.size());
        if (const auto count = input.gcount(); count > 0)
            hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(count)));
    }
    if (!input.eof())
        throw std::runtime_error("Checksum calculation failed.");
    return hash.result().toHex().toStdString();
#endif
}
} // namespace maskpilot
