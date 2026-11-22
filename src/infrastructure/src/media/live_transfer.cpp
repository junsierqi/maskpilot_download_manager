#include "../filesystem/transfer_files.h"
#include "../network/curl_runtime.h"
#include "../network/proxy_options.h"
#include "live_assembly.h"
#include "live_playlist.h"
#include <QByteArray>
#include <QRegularExpression>
#include <QUrlQuery>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <curl/curl.h>
#include <limits>
#include <maskpilot/infrastructure/adapters.h>
#include <memory>
#include <openssl/crypto.h>
#include <stdexcept>
#include <thread>

namespace maskpilot {
namespace {
using Clock = std::chrono::steady_clock;
namespace files = transfer_files;
struct Interrupted {};
class Recording {
    const Task& task_;
    TransferControl& control_;
    const std::function<void(TransferEvent)>& callback_;
    Clock::time_point started_ = Clock::now();
    Clock::time_point notified_ = started_;
    FILE* file_ = nullptr;
    std::int64_t written_ = 0;
    bool media_ = false;

  public:
    Recording(const Task& task, TransferControl& control, const std::function<void(TransferEvent)>& callback)
        : task_(task), control_(control), callback_(callback) {}
    ~Recording() {
        if (file_)
            fclose(file_);
    }
    bool ended() const {
        return remaining() == 0 || control_.finish ||
               Clock::now() - started_ >= std::chrono::seconds(task_.spec.live.durationSeconds);
    }
    void check() const {
        if (control_.pause || control_.cancel ||
            (control_.payloadBudget &&
             (control_.payloadBudget->exhausted() || control_.payloadBudget->failed())))
            throw Interrupted{};
    }
    void open(const std::filesystem::path& path) {
        file_ = files::openFile(path, L"wb");
        if (!file_)
            throw std::runtime_error("Cannot open the owned live recording file.");
    }
    std::int64_t remaining() const { return task_.spec.live.maxBytes - written_; }
    bool append(const QByteArray& bytes, bool media = true) {
        check();
        if (bytes.size() > remaining())
            return false;
        if (fwrite(bytes.data(), 1, static_cast<size_t>(bytes.size()), file_) !=
            static_cast<size_t>(bytes.size()))
            throw std::runtime_error("Cannot write live recording. Check disk space.");
        written_ += bytes.size();
        media_ |= media;
        progress();
        return true;
    }
    void progress(bool force = false) {
        const auto now = Clock::now();
        if (!force && now - notified_ < std::chrono::milliseconds(200))
            return;
        notified_ = now;
        TransferEvent event;
        event.downloaded = written_;
        const auto ms = std::max<std::int64_t>(
            1, std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count());
        event.speed = static_cast<std::int64_t>(written_ * 1000.0 / ms);
        callback_(event);
    }
    void close() {
        if (!media_)
            throw std::runtime_error("No complete media was recorded; no output was published.");
        files::syncFile(file_);
        if (fclose(file_) != 0) {
            file_ = nullptr;
            throw std::runtime_error("Cannot close live recording.");
        }
        file_ = nullptr;
    }
    std::int64_t size() const { return written_; }
    void useProcessedSize(std::int64_t size) {
        if (size <= 0 || size > task_.spec.live.maxBytes)
            throw std::runtime_error("Processed live output exceeds its size limit.");
        written_ = size;
    }
};

class Http {
    const DownloadSpec& spec_;
    TransferControl& control_;
    Recording& recording_;
    QUrl origin_;

  public:
    Http(const DownloadSpec& spec, TransferControl& control, Recording& recording)
        : spec_(spec), control_(control), recording_(recording),
          origin_(QString::fromUtf8(spec.url), QUrl::StrictMode) {}
    // false means a deliberate recording boundary, never a transport success.
    bool get(QUrl& url, const std::function<bool(const QByteArray&)>& sink, bool media,
             std::int64_t bodyLimit, const std::optional<live::ByteRange>& range = {}) {
        for (int redirects = 0; redirects <= 5; ++redirects) {
            recording_.check();
            if (recording_.ended())
                return false;
            live::resolve(origin_, url, QString::fromUtf8(url.toEncoded()), spec_.mediaOrigins);
            const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(),
                                                                           curl_easy_cleanup);
            if (!easy)
                throw std::runtime_error("Cannot create live HTTP transfer.");
            struct State {
                Recording& recording;
                TransferControl& control;
                const std::function<bool(const QByteArray&)>& sink;
                std::int64_t limit, received = 0;
                bool media, boundary = false;
                long status = 0;
                std::size_t headerBytes = 0;
                QByteArray location;
                bool locationSeen = false;
                std::string error;
                QByteArray contentRange;
                bool contentRangeSeen = false;
                bool ranged = false;
            } state{recording_, control_, sink, bodyLimit, 0, media};
            state.ranged = range.has_value();
            const auto set = [&](CURLoption option, auto value) {
                if (curl_easy_setopt(easy.get(), option, value) != CURLE_OK)
                    throw std::runtime_error("Cannot configure live HTTP transfer.");
            };
            const auto encoded = url.toEncoded();
            set(CURLOPT_URL, encoded.constData());
            set(CURLOPT_PROTOCOLS_STR, "http,https");
            set(CURLOPT_FOLLOWLOCATION, 0L);
            set(CURLOPT_NOSIGNAL, 1L);
            set(CURLOPT_CONNECTTIMEOUT, 15L);
            set(CURLOPT_LOW_SPEED_LIMIT, 1L);
            set(CURLOPT_LOW_SPEED_TIME, 30L);
            set(CURLOPT_SSL_VERIFYPEER, 1L);
            set(CURLOPT_SSL_VERIFYHOST, 2L);
            set(CURLOPT_NETRC, CURL_NETRC_IGNORED);
            set(CURLOPT_USERAGENT, spec_.userAgent.c_str());
            set(CURLOPT_HTTP_CONTENT_DECODING, 0L);
            set(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
            set(CURLOPT_HEADEROPT, static_cast<long>(CURLHEADER_SEPARATE));
            network::configureProxy(easy.get(), spec_);
            curl_slist* headers = nullptr;
            const auto add = [&](const std::string& name, const std::string& value) {
                if (value.empty())
                    return;
                auto* next = curl_slist_append(headers, (name + ": " + value).c_str());
                if (!next) {
                    curl_slist_free_all(headers);
                    headers = nullptr;
                    throw std::runtime_error("Cannot allocate live request headers.");
                }
                headers = next;
            };
            add("Accept-Encoding", "identity");
            const auto* rule = static_cast<const MediaOriginAccess*>(nullptr);
            if (!live::sameOrigin(origin_, url)) {
                const auto target = mediaOrigin(url.toEncoded().toStdString());
                for (const auto& candidate : spec_.mediaOrigins)
                    if (mediaOrigin(candidate.origin, true) == target)
                        rule = &candidate;
                if (!rule)
                    throw std::runtime_error("Unapproved live source origin.");
            }
            add("Cookie", rule ? rule->cookie : spec_.cookie);
            add("Authorization", rule ? rule->authorization : spec_.authorization);
            if (!rule && !spec_.referrer.empty()) {
                auto referrer = QUrl(QString::fromUtf8(spec_.referrer));
                referrer.setPath("/");
                referrer.setQuery(QString{});
                referrer.setFragment(QString{});
                referrer.setUserInfo(QString{});
                add("Referer", referrer.toEncoded().toStdString());
            }
            for (const auto& [name, value] : rule ? rule->headers : spec_.headers)
                add(name, value);
            if (range)
                add("Range", "bytes=" + std::to_string(range->offset) + "-" +
                                 std::to_string(range->offset + range->length - 1));
            const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headerOwner(
                headers, curl_slist_free_all);
            set(CURLOPT_HTTPHEADER, headers);
            set(CURLOPT_HEADERDATA, &state);
            set(
                CURLOPT_HEADERFUNCTION, +[](char* data, size_t size, size_t count, void* opaque) -> size_t {
                    auto& s = *static_cast<State*>(opaque);
                    const auto n = size * count;
                    try {
                        s.headerBytes += n;
                        if (s.headerBytes > 64 * 1024)
                            throw std::runtime_error("Live response headers exceed the limit.");
                        const QByteArray line(data, static_cast<qsizetype>(n));
                        if (line.startsWith("HTTP/")) {
                            s.status = line.split(' ').value(1).toLong();
                            s.location.clear();
                            s.locationSeen = false;
                            s.contentRange.clear();
                            s.contentRangeSeen = false;
                        } else {
                            const auto colon = line.indexOf(':');
                            const auto key = line.left(colon).trimmed().toLower();
                            if (colon > 0 && key == "location") {
                                if (s.locationSeen)
                                    throw std::runtime_error("Ambiguous live redirect.");
                                s.location = line.mid(colon + 1).trimmed();
                                s.locationSeen = true;
                            }
                            if (colon > 0 && key == "content-encoding" &&
                                line.mid(colon + 1).trimmed().toLower() != "identity")
                                throw std::runtime_error("Encoded live responses are unsupported.");
                            if (colon > 0 && key == "content-range") {
                                if (s.contentRangeSeen)
                                    throw std::runtime_error("Ambiguous live content range.");
                                s.contentRange = line.mid(colon + 1).trimmed();
                                s.contentRangeSeen = true;
                            }
                        }
                        return n;
                    } catch (const std::exception& e) {
                        s.error = e.what();
                        return 0;
                    }
                });
            set(CURLOPT_WRITEDATA, &state);
            set(
                CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t count, void* opaque) -> size_t {
                    auto& s = *static_cast<State*>(opaque);
                    const auto n = size * count;
                    try {
                        s.recording.check();
                        if (s.recording.ended()) {
                            s.boundary = true;
                            return 0;
                        }
                        if (n > static_cast<std::uint64_t>(s.limit - s.received))
                            throw std::runtime_error("Live response exceeds its bounded size.");
                        s.received += static_cast<std::int64_t>(n);
                        if (s.status != (s.ranged ? 206 : 200))
                            return n;
                        if (s.media && s.control.payloadBudget && s.control.payloadBudget->accept(n) != n)
                            throw Interrupted{};
                        if (!s.sink(QByteArray(data, static_cast<qsizetype>(n)))) {
                            s.boundary = true;
                            return 0;
                        }
                        return n;
                    } catch (const Interrupted&) {
                        return 0;
                    } catch (const std::exception& e) {
                        s.error = e.what();
                        return 0;
                    }
                });
            set(CURLOPT_NOPROGRESS, 0L);
            set(CURLOPT_XFERINFODATA, &state);
            set(
                CURLOPT_XFERINFOFUNCTION,
                +[](void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                    auto& s = *static_cast<State*>(opaque);
                    s.boundary |= s.recording.ended();
                    return s.boundary || s.control.pause || s.control.cancel ||
                           (s.control.payloadBudget &&
                            (s.control.payloadBudget->exhausted() || s.control.payloadBudget->failed()));
                });
            struct Multi {
                CURLM* value = curl_multi_init();
                CURL* easy;
                explicit Multi(CURL* e) : easy(e) {
                    if (!value || curl_multi_add_handle(value, e) != CURLM_OK) {
                        if (value)
                            curl_multi_cleanup(value);
                        throw std::runtime_error("Cannot schedule live HTTP transfer.");
                    }
                }
                ~Multi() {
                    curl_multi_remove_handle(value, easy);
                    curl_multi_cleanup(value);
                }
            } multi(easy.get());
            int running = 1;
            while (running) {
                recording_.check();
                if (recording_.ended()) {
                    state.boundary = true;
                    break;
                }
                set(CURLOPT_MAX_RECV_SPEED_LARGE,
                    static_cast<curl_off_t>(std::max<std::int64_t>(0, control_.speedLimit.load())));
                if (curl_multi_perform(multi.value, &running) != CURLM_OK)
                    throw std::runtime_error("Live transfer scheduling failed.");
                recording_.progress();
                if (running)
                    curl_multi_poll(multi.value, nullptr, 0, 100, nullptr);
            }
            recording_.check();
            if (!state.error.empty())
                throw std::runtime_error(state.error);
            if (state.boundary)
                return false;
            int messages = 0;
            const auto* message = curl_multi_info_read(multi.value, &messages);
            if (!message || message->msg != CURLMSG_DONE || message->data.result != CURLE_OK)
                throw std::runtime_error(
                    "Live source connection failed or ended incompletely; no output published.");
            if (state.status == 301 || state.status == 302 || state.status == 303 || state.status == 307 ||
                state.status == 308) {
                if (redirects == 5 || !state.locationSeen)
                    throw std::runtime_error("Live redirect limit or missing destination.");
                url = live::resolve(origin_, url, QString::fromUtf8(state.location), spec_.mediaOrigins);
                continue;
            }
            if (state.status != (range ? 206 : 200))
                throw std::runtime_error("Live source refused the request. Check its address and login.");
            if (range) {
                const auto match = QRegularExpression("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$")
                                       .match(QString::fromLatin1(state.contentRange));
                bool firstOk = false, lastOk = false, totalOk = false;
                const auto first = match.captured(1).toLongLong(&firstOk);
                const auto last = match.captured(2).toLongLong(&lastOk);
                const auto total = match.captured(3).toLongLong(&totalOk);
                if (!match.hasMatch() || !firstOk || !lastOk || !totalOk || first != range->offset ||
                    last != range->offset + range->length - 1 || total <= last ||
                    state.received != range->length)
                    throw std::runtime_error(
                        "Live byte-range response does not match the requested complete range.");
            } else if (state.contentRangeSeen)
                throw std::runtime_error("Unexpected live partial response.");
            return true;
        }
        throw std::runtime_error("Live redirect limit exceeded.");
    }
};

void hls(const DownloadSpec& spec, TransferControl& control, Recording& record, Http& http,
         live::Assembly& assembly) {
    const QUrl origin(QString::fromUtf8(spec.url), QUrl::StrictMode);
    auto url = origin;
    QUrl audioUrl;
    std::int64_t next = -1, previousSequence = -1;
    std::int64_t lastDiscontinuity = -1;
    int depth = 0;
    auto lastSegment = Clock::now();
    const auto load = [&](QUrl& target, live::Playlist& result) {
        QUrlQuery query(target);
        for (const auto* key : {"_HLS_skip", "_HLS_msn", "_HLS_part"})
            query.removeAllQueryItems(key);
        target.setQuery(query);
        QByteArray bytes;
        if (!http.get(
                target,
                [&](const QByteArray& data) {
                    bytes += data;
                    return true;
                },
                false, 1024 * 1024))
            return false;
        result = live::parsePlaylist(bytes, origin, target, spec.mediaOrigins);
        return true;
    };
    const auto fetch = [&](const live::Source& source, std::int64_t sequence, QByteArray& output) {
        auto target = source.url;
        QByteArray bytes;
        if (!http.get(
                target,
                [&](const QByteArray& data) {
                    if (bytes.size() + data.size() > record.remaining() + 16)
                        return false;
                    bytes += data;
                    return true;
                },
                true, source.range ? source.range->length : 32 * 1024 * 1024, source.range))
            return false;
        if (!source.encryption.key.isEmpty()) {
            struct Key {
                QByteArray bytes;
                ~Key() {
                    if (!bytes.isEmpty())
                        OPENSSL_cleanse(bytes.data(), static_cast<std::size_t>(bytes.size()));
                }
            } key;
            auto keyUrl = source.encryption.key;
            if (!http.get(
                    keyUrl,
                    [&](const QByteArray& data) {
                        key.bytes += data;
                        return true;
                    },
                    false, 16))
                return false;
            output = live::decrypt(bytes, key.bytes, source.encryption.iv, sequence);
        } else
            output = std::move(bytes);
        if (output.isEmpty())
            throw std::runtime_error("Empty complete live media segment.");
        return true;
    };
    while (!record.ended()) {
        record.check();
        live::Playlist playlist;
        if (!load(url, playlist))
            return;
        if (!playlist.variant.isEmpty()) {
            if (++depth > 3 || next >= 0)
                throw std::runtime_error("Unsupported nested or changing live variant.");
            if (!playlist.audio.isEmpty()) {
                if (!audioUrl.isEmpty())
                    throw std::runtime_error("Unsupported nested external audio.");
                audioUrl = playlist.audio;
                live::requireMuxer(spec.mediaMuxer);
            }
            url = playlist.variant;
            continue;
        }
        live::Playlist audio;
        if (!audioUrl.isEmpty()) {
            if (!load(audioUrl, audio))
                return;
            if (!audio.variant.isEmpty())
                throw std::runtime_error("External live audio must be a media playlist.");
        }
        const auto end = playlist.sequence + static_cast<std::int64_t>(playlist.segments.size());
        if (next < 0)
            next = playlist.ended ? playlist.sequence : std::max(playlist.sequence, end - 3);
        if (playlist.sequence < previousSequence || next < playlist.sequence || next > end)
            throw std::runtime_error("Live HLS sequence skipped or reset. Recording was not published.");
        previousSequence = playlist.sequence;
        for (; next < end; ++next) {
            record.check();
            if (record.ended() || assembly.full())
                return;
            const auto& segment = playlist.media[static_cast<size_t>(next - playlist.sequence)];
            if (lastDiscontinuity > segment.discontinuity)
                throw std::runtime_error("Live discontinuity sequence reset.");
            const live::MediaSegment* pair[2]{&segment, nullptr};
            if (!audioUrl.isEmpty()) {
                const auto index = next - audio.sequence;
                if (index < 0 || index >= static_cast<std::int64_t>(audio.media.size()))
                    throw std::runtime_error("External live audio sequence is not aligned.");
                pair[1] = &audio.media[static_cast<size_t>(index)];
                if (!segment.programTimeMs || !pair[1]->programTimeMs ||
                    std::abs(*segment.programTimeMs - *pair[1]->programTimeMs) > 1 ||
                    std::abs(pair[1]->duration - segment.duration) > .001 ||
                    pair[1]->discontinuity != segment.discontinuity)
                    throw std::runtime_error("External live audio lacks a matching program clock "
                                             "(PROGRAM-DATE-TIME) or aligned timing.");
            }
            const bool processing = pair[1] || segment.initialization ||
                                    (lastDiscontinuity >= 0 && lastDiscontinuity != segment.discontinuity);
            if (processing)
                live::requireMuxer(spec.mediaMuxer);
            QByteArray content[2], maps[2];
            std::int64_t total = 0;
            for (int track = 0; track < 2 && pair[track]; ++track) {
                if (pair[track]->initialization && !fetch(*pair[track]->initialization, next, maps[track]))
                    return;
                if (!fetch(pair[track]->source, next, content[track]))
                    return;
                total += maps[track].size() + content[track].size();
            }
            if (total > record.remaining())
                return; // Keep complete earlier aligned pairs only.
            if (!segment.initialization) {
                if (content[0].size() % 188 != 0)
                    throw std::runtime_error("Live HLS segment is not a complete MPEG-TS stream.");
                for (qsizetype i = 0; i < content[0].size(); i += 188)
                    if (static_cast<unsigned char>(content[0][i]) != 0x47)
                        throw std::runtime_error("Live HLS segment has invalid MPEG-TS framing.");
            }
            for (int track = 0; track < 2 && pair[track]; ++track) {
                assembly.append(track, *pair[track], content[track], maps[track]);
                if ((!maps[track].isEmpty() && !record.append(maps[track], false)) ||
                    !record.append(content[track]))
                    return;
            }
            lastDiscontinuity = segment.discontinuity;
            lastSegment = Clock::now();
        }
        if (playlist.ended || record.remaining() == 0)
            return;
        if (Clock::now() - lastSegment > std::chrono::seconds(std::max(30, playlist.targetSeconds * 3)))
            throw std::runtime_error("Live playlist stopped advancing. No output was published.");
        const auto wake = Clock::now() + std::chrono::milliseconds(playlist.targetSeconds * 500);
        while (Clock::now() < wake && !record.ended()) {
            record.check();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    (void)control;
}
std::uint32_t be24(const QByteArray& data, qsizetype p) {
    return (static_cast<unsigned char>(data[p]) << 16) | (static_cast<unsigned char>(data[p + 1]) << 8) |
           static_cast<unsigned char>(data[p + 2]);
}
std::uint32_t be32(const QByteArray& data, qsizetype p) {
    return (static_cast<unsigned char>(data[p]) << 24) | be24(data, p + 1);
}
void flv(const DownloadSpec& spec, Recording& record, Http& http) {
    auto url = QUrl(QString::fromUtf8(spec.url), QUrl::StrictMode);
    QByteArray pending;
    bool header = false;
    const auto complete = http.get(
        url,
        [&](const QByteArray& data) {
            pending += data;
            for (;;) {
                if (!header) {
                    if (pending.size() < 9)
                        return true;
                    const auto offset = be32(pending, 5);
                    if (!pending.startsWith("FLV\x01") || (pending[4] & ~5) || offset < 9 || offset > 65536)
                        throw std::runtime_error("Invalid HTTP-FLV header.");
                    if (pending.size() < offset + 4)
                        return true;
                    if (be32(pending, offset) != 0 || !record.append(pending.left(offset + 4), false))
                        throw std::runtime_error("Invalid FLV header or recording size.");
                    pending.remove(0, offset + 4);
                    header = true;
                }
                if (pending.size() < 11)
                    return true;
                const auto type = static_cast<unsigned char>(pending[0]);
                const auto size = be24(pending, 1);
                if ((type != 8 && type != 9 && type != 18) || be24(pending, 8) != 0)
                    throw std::runtime_error("Unsupported or malformed FLV tag.");
                const auto length = static_cast<qsizetype>(size) + 15;
                if (pending.size() < length)
                    return true;
                if (be32(pending, length - 4) != size + 11)
                    throw std::runtime_error("Invalid FLV previous-tag size.");
                if (!record.append(pending.left(length), (type == 8 || type == 9) && size != 0))
                    return false;
                pending.remove(0, length);
            }
        },
        true, std::numeric_limits<std::int64_t>::max());
    if (complete && (!header || !pending.isEmpty()))
        throw std::runtime_error("Live FLV source ended with incomplete media. No output published.");
}
class LiveTransfer final : public ITransferTransport {
  public:
    void run(const Task& task, const std::filesystem::path& work, TransferControl& control,
             const std::function<void(TransferEvent)>& callback) override {
        TransferEvent event;
        event.phase = TaskState::Probing;
        auto finish = [&](TransferOutcome outcome, const std::string& error = {}) {
            event.outcome = outcome;
            event.error = error;
            event.speed = 0;
            callback(event);
        };
        try {
            if (task.ownerId != "standalone" || task.spec.kind != TransferKind::Live)
                throw std::invalid_argument("A standalone live recording task is required.");
            if (auto error = validateSpec(task.spec); !error.empty())
                throw std::invalid_argument(error);
            auto spec = task.spec;
            spec.originAuthenticationRequired |= task.originAuthenticationRequired;
            if (auto error = validateOriginAuthentication(spec, true); !error.empty())
                throw std::invalid_argument(error);
            if (auto error = validateProxy(spec, true); !error.empty())
                throw std::invalid_argument(error);
            if (std::filesystem::exists(spec.destination))
                throw std::runtime_error("Recording destination already exists; choose a new filename.");
            network::initializeCurl();
            std::filesystem::create_directories(work);
            auto raw = work / L"live-recording.tmp";
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(raw)))
                throw std::runtime_error("Unsafe owned recording path.");
            Recording recording(task, control, callback);
            recording.check();
            recording.open(raw);
            event.phase = TaskState::Downloading;
            callback(event);
            Http http(spec, control, recording);
            live::Assembly assembly(work);
            if (spec.live.format == LiveFormat::HlsTs)
                hls(spec, control, recording, http, assembly);
            else
                flv(spec, recording, http);
            recording.check();
            recording.close();
            if (assembly.needsMux()) {
                event.downloaded = event.total = recording.size();
                event.phase = TaskState::Finalizing;
                callback(event);
                raw = assembly.mux(spec, control);
                recording.check();
                recording.useProcessedSize(static_cast<std::int64_t>(std::filesystem::file_size(raw)));
            }
            event.downloaded = event.total = recording.size();
            event.phase = TaskState::Verifying;
            callback(event);
            std::filesystem::create_directories(spec.destination.parent_path());
            auto stage = spec.destination;
            stage += L".maskpilot-" + std::filesystem::path(task.id).wstring() + L".tmp";
            struct StageOwner {
                std::filesystem::path path;
                bool created = false;
                ~StageOwner() {
                    if (created) {
                        std::error_code ignored;
                        std::filesystem::remove(path, ignored);
                    }
                }
            } stageOwner{stage};
            // Exclusive stage creation preserves stale or externally created files.
            const std::unique_ptr<FILE, decltype(&fclose)> input(files::openFile(raw, L"rb"), fclose);
#ifdef _WIN32
            FILE* created = files::openFile(stage, L"wbx");
#else
            FILE* created = fopen(stage.c_str(), "wbx");
#endif
            std::unique_ptr<FILE, decltype(&fclose)> output(created, fclose);
            stageOwner.created = created != nullptr;
            if (!input || !output)
                throw std::runtime_error("Cannot stage live recording; existing files were preserved.");
            std::array<char, 128 * 1024> buffer{};
            std::int64_t copied = 0;
            while (const auto count = fread(buffer.data(), 1, buffer.size(), input.get())) {
                recording.check();
                if (count > static_cast<std::uint64_t>(recording.size() - copied) ||
                    fwrite(buffer.data(), 1, count, output.get()) != count)
                    throw std::runtime_error("Cannot copy the complete live recording.");
                copied += static_cast<std::int64_t>(count);
            }
            if (ferror(input.get()) || copied != recording.size())
                throw std::runtime_error("Live recording cache changed or could not be read.");
            recording.check();
            files::syncFile(output.get());
            if (fclose(output.release()) != 0)
                throw std::runtime_error("Cannot close the synchronized recording stage.");
            if (!spec.expectedSha256.empty()) {
                auto expected = spec.expectedSha256;
                std::transform(expected.begin(), expected.end(), expected.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (sha256File(stage, &control) != expected)
                    throw std::runtime_error("Recording SHA-256 verification failed. No output published.");
            }
            recording.check();
            if (control.scanBeforePublish) {
                const auto scan = control.scanBeforePublish(stage);
                if (scan.outcome != TransferOutcome::Complete) {
                    finish(scan.outcome, scan.error);
                    return;
                }
            }
            event.phase = TaskState::Finalizing;
            callback(event);
            recording.check();
            files::publishFile(stage, spec.destination);
            finish(TransferOutcome::Complete);
        } catch (const Interrupted&) {
            finish(control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused);
        } catch (const std::exception& e) {
            if (control.cancel || control.pause)
                finish(control.cancel ? TransferOutcome::Cancelled : TransferOutcome::Paused);
            else
                finish(TransferOutcome::Attention, e.what());
        }
    }
};
} // namespace
std::unique_ptr<ITransferTransport> makeLiveTransfer() { return std::make_unique<LiveTransfer>(); }
} // namespace maskpilot
