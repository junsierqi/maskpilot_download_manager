#include "../filesystem/transfer_files.h"
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
    bool complete = false, conflict = false;
    size_t bytes = 0;
    void line(std::string line) {
        bytes += line.size();
        if (bytes > 1024 * 1024)
            throw std::runtime_error("Response headers exceed the limit.");
        if (line.starts_with("HTTP/")) {
            fields.clear();
            complete = false;
            conflict = false;
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
        fields[name] = std::move(value);
    }
    std::string get(const std::string& name) const {
        auto it = fields.find(name);
        return it == fields.end() ? std::string() : it->second;
    }
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
struct CurlGlobal {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("Cannot initialize networking.");
    }
    ~CurlGlobal() { curl_global_cleanup(); }
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
    if (!spec.proxy.empty())
        curl_easy_setopt(easy.handle, CURLOPT_PROXY, spec.proxy.c_str());
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
    return urlPart(value, CURLUPART_SCHEME) + "://" + urlPart(value, CURLUPART_HOST) + ":" +
           urlPart(value, CURLUPART_PORT);
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
        easy.handle, CURLOPT_WRITEFUNCTION, +[](char*, size_t, size_t, void*) -> size_t { return 0; });
    auto code = curl_easy_perform(easy.handle);
    Probe result;
    result.code = code;
    result.status = headers.status;
    result.etag = headers.get("etag");
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
Probe probe(const DownloadSpec& initial, TransferControl& control) {
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
            auto candidate = spec;
            candidate.url = next;
            if (!validateSpec(candidate).empty())
                throw std::runtime_error("Server returned an invalid redirect.");
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
    Easy easy;
    Headers headers;
    FILE* file = nullptr;
    std::filesystem::path part, offsetPath;
    std::int64_t begin = 0, end = -1, written = 0, total = -1, partOffset = 0;
    bool ranged = false, validated = false, finished = false;
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
        if (validated)
            return error.empty();
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
        } else if (headers.status != 200) {
            error = "HTTP response was not successful (" + std::to_string(headers.status) + ").";
            return false;
        }
        if (!headers.get("content-encoding").empty() && headers.get("content-encoding") != "identity") {
            error = "Unexpected encoded response.";
            return false;
        }
        return true;
    }
    static size_t write(char* data, size_t size, size_t count, void* context) {
        auto& sink = *static_cast<Sink*>(context);
        auto length = size * count;
        try {
            if (sink.headers.status >= 300 && sink.headers.status < 400)
                return length;
            if (!sink.validate())
                return 0;
            if (sink.end >= 0 && static_cast<std::uint64_t>(length) >
                                     static_cast<std::uint64_t>(sink.end - sink.begin - sink.written)) {
                sink.error = "Response exceeded the requested file bounds.";
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
        static CurlGlobal global;
        if (isFtpUrl(task.spec.url)) {
            makeFtpTransfer()->run(task, work, control, callback);
            return;
        }
        TransferEvent event;
        event.phase = TaskState::Probing;
        auto emit = [&](TransferOutcome result, std::string error = {}, bool retry = false) {
            event.outcome = result;
            event.error = std::move(error);
            event.retryable = retry;
            callback(event);
        };
        callback(event);
        auto stopped = [&]() {
            if (control.cancel) {
                emit(TransferOutcome::Cancelled);
                return true;
            }
            if (control.pause) {
                emit(TransferOutcome::Paused);
                return true;
            }
            return false;
        };
        Probe remote;
        try {
            remote = probe(task.spec, control);
        } catch (const std::exception& e) {
            if (!stopped())
                emit(TransferOutcome::Attention, e.what());
            return;
        }
        if (stopped())
            return;
        event.total = remote.total;
        event.retryAfter = remote.retryAfter;
        if (remote.status != 200 && remote.status != 206 && !(remote.status == 416 && remote.total == 0)) {
            emit(remote.status == 401 || remote.status == 403 ? TransferOutcome::Attention
                                                              : TransferOutcome::Failed,
                 "Server rejected download (HTTP " + std::to_string(remote.status) + ").",
                 retryStatus(remote.status) || retryCode(remote.code));
            return;
        }
        const bool resumable = remote.ranges && strongEtag(remote.etag) && remote.total >= 0;
        event.resumable = resumable;
        std::filesystem::create_directories(work);
        const auto metadata = work / "identity";
        int count = resumable && remote.total >= 1024 * 1024 ? task.spec.connections : 1;
        bool reuse = false;
        if (resumable && std::filesystem::exists(metadata)) {
            std::ifstream input(metadata);
            std::string tag, resource;
            std::int64_t length;
            int storedCount;
            if (input >> std::quoted(tag) >> length >> storedCount >> std::quoted(resource);
                input && tag == remote.etag && resource == remote.effectiveUrl && length == remote.total &&
                storedCount >= 1 && storedCount <= 8) {
                count = storedCount;
                reuse = true;
            }
        }
        auto ranges = remote.total >= 0 ? splitSegments(remote.total, count) : std::vector<Segment>{{0, -1}};
        std::ostringstream identity;
        identity << std::quoted(remote.etag) << ' ' << remote.total << ' ' << ranges.size() << ' '
                 << std::quoted(remote.effectiveUrl);
        if (!reuse) {
            // Delete only our fixed-name part files, never arbitrary user paths.
            for (int i = 0; i < 8; ++i) {
                std::error_code ec;
                std::filesystem::remove(work / ("part-" + std::to_string(i)), ec);
                std::filesystem::remove(work / ("offset-" + std::to_string(i)), ec);
            }
            atomicText(metadata, identity.str());
        }
        std::vector<std::unique_ptr<Sink>> sinks;
        auto* multi = curl_multi_init();
        if (!multi)
            throw std::runtime_error("Cannot initialize transfer scheduler.");
        struct MultiGuard {
            CURLM* multi;
            std::vector<std::unique_ptr<Sink>>& sinks;
            ~MultiGuard() {
                for (auto& sink : sinks)
                    curl_multi_remove_handle(multi, sink->easy.handle);
                curl_multi_cleanup(multi);
            }
        } guard{multi, sinks};
        for (size_t i = 0; i < ranges.size(); ++i) {
            auto sink = std::make_unique<Sink>();
            sink->part = work / ("part-" + std::to_string(i));
            sink->offsetPath = work / ("offset-" + std::to_string(i));
            const auto range = ranges[i];
            std::int64_t offset = 0;
            if (reuse && std::filesystem::exists(sink->part) && std::filesystem::exists(sink->offsetPath)) {
                std::ifstream input(sink->offsetPath);
                input >> offset;
                if (!input || offset < 0 || offset > range.end - range.begin ||
                    static_cast<std::uint64_t>(offset) > std::filesystem::file_size(sink->part))
                    offset = 0;
            }
            if (std::filesystem::exists(sink->part))
                std::filesystem::resize_file(sink->part, static_cast<std::uintmax_t>(offset));
            sink->begin = range.begin + offset;
            sink->end = range.end;
            sink->total = remote.total;
            sink->ranged = resumable;
            sink->expectedEtag = remote.etag;
            sink->file = openFile(sink->part, L"ab");
            if (!sink->file)
                throw std::runtime_error("Cannot open temporary file.");
            // The sidecar stores total bytes in this part, including previously synchronized data.
            sink->written = 0;
            sink->partOffset = offset;
            configure(sink->easy, remote.effectiveSpec, sink->headers, control);
            if (resumable && sink->begin < range.end) {
                auto rangeText = std::to_string(sink->begin) + "-" + std::to_string(range.end - 1);
                curl_easy_setopt(sink->easy.handle, CURLOPT_RANGE, rangeText.c_str());
                sink->easy.header("If-Range: " + remote.etag);
            }
            curl_easy_setopt(sink->easy.handle, CURLOPT_HTTPHEADER, sink->easy.list);
            curl_easy_setopt(sink->easy.handle, CURLOPT_WRITEFUNCTION, Sink::write);
            curl_easy_setopt(sink->easy.handle, CURLOPT_WRITEDATA, sink.get());
            curl_easy_setopt(sink->easy.handle, CURLOPT_PRIVATE, sink.get());
            if (range.end >= 0 && sink->begin == range.end) {
                sink->finished = true;
            } else if (curl_multi_add_handle(multi, sink->easy.handle) != CURLM_OK)
                throw std::runtime_error("Cannot queue transfer connection.");
            sinks.push_back(std::move(sink));
        }
        const auto started = Clock::now();
        auto lastEvent = started;
        std::int64_t previousBytes = 0;
        auto previousTime = started;
        std::string failure;
        bool integrityFailure = false, retryable = false;
        int running = 0;
        do {
            const auto limit = control.speedLimit.load();
            for (auto& sink : sinks)
                curl_easy_setopt(
                    sink->easy.handle, CURLOPT_MAX_RECV_SPEED_LARGE,
                    static_cast<curl_off_t>(
                        limit > 0 ? std::max<std::int64_t>(1, limit / static_cast<std::int64_t>(sinks.size()))
                                  : 0));
            auto code = curl_multi_perform(multi, &running);
            if (code != CURLM_OK) {
                failure = "Network scheduler failed.";
                retryable = true;
                break;
            }
            int remaining = 0;
            while (auto* message = curl_multi_info_read(multi, &remaining)) {
                if (message->msg != CURLMSG_DONE)
                    continue;
                Sink* sink = nullptr;
                curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &sink);
                sink->finished = true;
                if (control.pause || control.cancel)
                    continue;
                const bool valid = sink->validate();
                if (!sink->error.empty()) {
                    failure = sink->error;
                    integrityFailure =
                        (sink->ranged && (sink->headers.status == 200 || sink->headers.status == 206 ||
                                          sink->headers.status == 416)) ||
                        sink->headers.status == 401 || sink->headers.status == 403;
                    retryable = retryStatus(sink->headers.status);
                    event.retryAfter = static_cast<int>(
                        std::clamp<std::int64_t>(number(sink->headers.get("retry-after")), 0, 3600));
                } else if (message->data.result != CURLE_OK) {
                    failure = "Connection interrupted (" + std::to_string(message->data.result) + ").";
                    retryable = retryCode(message->data.result);
                } else if (!valid || (sink->end >= 0 && sink->written != sink->end - sink->begin)) {
                    failure = "Response ended before the requested bytes were received.";
                    retryable = true;
                }
            }
            event.downloaded = 0;
            for (size_t i = 0; i < sinks.size(); ++i)
                event.downloaded += sinks[i]->begin - ranges[i].begin + sinks[i]->written;
            const auto now = Clock::now();
            if (now - lastEvent >= std::chrono::milliseconds(200) || running == 0) {
                const auto elapsed = std::chrono::duration<double>(now - previousTime).count();
                if (elapsed >= 0.5) {
                    event.speed = static_cast<std::int64_t>((event.downloaded - previousBytes) / elapsed);
                    previousBytes = event.downloaded;
                    previousTime = now;
                }
                event.phase = TaskState::Downloading;
                callback(event);
                lastEvent = now;
            }
            if (control.pause || control.cancel || !failure.empty())
                break;
            int descriptors = 0;
            if (running > 0)
                curl_multi_poll(multi, nullptr, 0, 100, &descriptors);
        } while (running > 0);
        for (size_t i = 0; i < sinks.size(); ++i) {
            auto& sink = *sinks[i];
            if (sink.file) {
                syncFile(sink.file);
                atomicText(sink.offsetPath, std::to_string(sink.begin - ranges[i].begin + sink.written));
                fclose(sink.file);
                sink.file = nullptr;
            }
        }
        if (stopped())
            return;
        if (!failure.empty()) {
            emit(integrityFailure ? TransferOutcome::Attention : TransferOutcome::Failed, failure, retryable);
            return;
        }
        if (remote.total >= 0 && event.downloaded != remote.total) {
            emit(TransferOutcome::Failed, "Downloaded byte count does not match the resource.");
            return;
        }
        if (remote.total < 0)
            event.total = event.downloaded;
        event.phase = TaskState::Verifying;
        event.speed = 0;
        callback(event);
        std::filesystem::create_directories(task.spec.destination.parent_path());
        auto stage = task.spec.destination;
        stage += L".maskpilot-" + std::filesystem::path(task.id).wstring() + L".tmp";
        FILE* output = openFile(stage, L"wb");
        if (!output) {
            emit(TransferOutcome::Attention, "Cannot create destination file. Check disk space and access.");
            return;
        }
        try {
            std::array<char, 128 * 1024> buffer{};
            for (auto& sink : sinks) {
                std::ifstream input(sink->part, std::ios::binary);
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
            if (std::filesystem::file_size(stage) != static_cast<std::uint64_t>(event.total))
                throw std::runtime_error("Assembled file length is invalid.");
            if (!task.spec.expectedSha256.empty()) {
                auto expected = task.spec.expectedSha256;
                std::transform(expected.begin(), expected.end(), expected.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (sha256File(stage, &control) != expected) {
                    emit(TransferOutcome::Attention,
                         "SHA-256 verification failed. The file was not published.");
                    return;
                }
            }
            event.phase = TaskState::Finalizing;
            callback(event);
            if (std::filesystem::exists(task.spec.destination)) {
                if (sha256File(stage, &control) == sha256File(task.spec.destination, &control)) {
                    if (stopped())
                        return;
                    std::filesystem::remove(stage);
                    emit(TransferOutcome::Complete);
                    return;
                }
                emit(TransferOutcome::Attention, "Destination already exists. Existing file was preserved.");
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
            emit(TransferOutcome::Complete);
        } catch (const std::exception& e) {
            if (output)
                fclose(output);
            if (!stopped())
                emit(TransferOutcome::Attention, e.what());
        }
    }
};
} // namespace
std::unique_ptr<ITransferTransport> makeCurlTransfer() { return std::make_unique<CurlTransfer>(); }
std::string sha256File(const std::filesystem::path& path, const TransferControl* control) {
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
    DWORD length = 0, size = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length),
                          &size, 0) < 0)
        throw std::runtime_error("Cannot initialize checksum.");
    std::vector<unsigned char> object(length);
    std::array<unsigned char, 32> digest{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot initialize checksum.");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read file for checksum.");
    std::array<char, 128 * 1024> buffer{};
    while (input) {
        if (control && (control->pause || control->cancel))
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
    (void)path;
    throw std::runtime_error("Checksum provider is not implemented for this platform.");
#endif
}
} // namespace maskpilot
