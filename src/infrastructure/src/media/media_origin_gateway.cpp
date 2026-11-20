#include "media_origin_gateway.h"
#include "../network/proxy_options.h"
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <curl/curl.h>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace maskpilot::media_process {
namespace {
constexpr qsizetype headLimit = 64 * 1024, bufferLimit = 256 * 1024;
using Clock = std::chrono::steady_clock;
int port(const QUrl& url) { return url.port(url.scheme() == "https" ? 443 : 80); }
bool sameOrigin(const QUrl& a, const QUrl& b) {
    return a.isValid() && !a.isRelative() && a.userInfo().isEmpty() && a.scheme() == b.scheme() &&
           a.host().compare(b.host(), Qt::CaseInsensitive) == 0 && port(a) == port(b) &&
           a.fragment().isEmpty();
}
bool clean(const QByteArray& text) {
    return std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
class Listener final : public QTcpServer {
    std::function<void(qintptr)> accepted_;
    void incomingConnection(qintptr fd) override { accepted_(fd); }

  public:
    explicit Listener(std::function<void(qintptr)> accepted) : accepted_(std::move(accepted)) {}
};
void refuse(QTcpSocket& client, bool authentication = false) {
    client.write(authentication ? "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic "
                                  "realm=\"media-origin\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                                : "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    client.waitForBytesWritten(100);
}
} // namespace
struct MediaOriginGateway::Impl {
    DownloadSpec specification;
    QUrl origin;
    QByteArray token, authentication;
    quint16 localPort = 0;
    std::atomic_bool stopping = false;
    std::jthread listener;
    struct Worker {
        std::shared_ptr<std::atomic_bool> done;
        std::jthread thread;
    };
    explicit Impl(const DownloadSpec& spec)
        : specification(spec), origin(QUrl::fromEncoded(QByteArray::fromStdString(spec.url))),
          token(QUuid::createUuid().toString(QUuid::Id128).toLatin1()),
          authentication("Basic " + (QByteArray("scope:") + token).toBase64()) {
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
                throw std::runtime_error("Cannot initialize media origin routing.");
        });
        std::promise<quint16> ready;
        auto result = ready.get_future();
        listener = std::jthread([this, ready = std::move(ready)]() mutable {
            std::vector<Worker> workers;
            Listener server([&](qintptr descriptor) {
                std::erase_if(workers, [](const Worker& worker) { return worker.done->load(); });
                if (workers.size() >= 16) {
                    QTcpSocket client;
                    if (client.setSocketDescriptor(descriptor))
                        refuse(client);
                    return;
                }
                auto done = std::make_shared<std::atomic_bool>(false);
                workers.push_back({done, std::jthread([this, done, descriptor] {
                                       try {
                                           serve(descriptor);
                                       } catch (...) { /* Never expose source credentials. */
                                       }
                                       done->store(true);
                                   })});
            });
            if (!server.listen(QHostAddress::LocalHost, 0)) {
                ready.set_exception(
                    std::make_exception_ptr(std::runtime_error("Cannot open owned media origin routing.")));
                return;
            }
            ready.set_value(server.serverPort());
            while (!stopping)
                server.waitForNewConnection(50);
            server.close();
            // Worker joins occur while the specification and stop flag are still alive.
        });
        localPort = result.get();
    }
    ~Impl() {
        stopping = true;
        if (listener.joinable())
            listener.join();
    }
    void serve(qintptr descriptor) {
        QTcpSocket client;
        if (!client.setSocketDescriptor(descriptor))
            return;
        client.setReadBufferSize(bufferLimit);
        QByteArray input;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        qsizetype end = -1;
        while (!stopping && (end = input.indexOf("\r\n\r\n")) < 0) {
            if (Clock::now() >= deadline || input.size() > headLimit ||
                client.state() != QAbstractSocket::ConnectedState)
                return;
            client.waitForReadyRead(50);
            input += client.read(std::max<qsizetype>(0, headLimit + 4 - input.size()));
        }
        if (stopping || end < 0 || end > headLimit)
            return;
        const auto lines = QString::fromLatin1(input.left(end)).split("\r\n");
        const auto request = lines.front().toLatin1().split(' ');
        if (request.size() != 3 || !clean(request[0]) || !clean(request[1]) ||
            (request[2] != "HTTP/1.0" && request[2] != "HTTP/1.1")) {
            refuse(client);
            return;
        }
        const bool tunnel = request[0] == "CONNECT";
        const auto destination = QUrl::fromEncoded(
            tunnel ? QByteArray("https://") + request[1] + "/" : request[1], QUrl::StrictMode);
        if (!sameOrigin(destination, origin) ||
            (tunnel && (destination.path() != "/" || destination.hasQuery())) ||
            (!tunnel && (origin.scheme() != "http" || (request[0] != "GET" && request[0] != "HEAD")))) {
            refuse(client);
            return;
        }
        int authenticationFields = 0, hostFields = 0, lengthFields = 0;
        QByteArray forwarded;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const auto line = lines[i].toLatin1();
            const auto colon = line.indexOf(':');
            const auto name = line.left(colon).toLower();
            const auto value = line.mid(colon + 1).trimmed();
            if (colon <= 0 || !clean(line) || name.contains(' ') || name.contains('\t')) {
                refuse(client);
                return;
            }
            if (name == "proxy-authorization") {
                if (++authenticationFields != 1 || value != authentication) {
                    refuse(client, true);
                    return;
                }
                continue;
            }
            if (name == "host") {
                const auto host =
                    QUrl::fromEncoded(origin.scheme().toLatin1() + "://" + value, QUrl::StrictMode);
                if (++hostFields != 1 || !sameOrigin(host, origin) || !host.path().isEmpty() ||
                    host.hasQuery()) {
                    refuse(client);
                    return;
                }
            }
            if (name == "transfer-encoding" || name == "upgrade" ||
                (name == "content-length" && (++lengthFields != 1 || value != "0"))) {
                refuse(client);
                return;
            }
            if (name == "connection" || name == "proxy-connection" || name == "keep-alive" || name == "te" ||
                name == "trailer")
                continue;
            forwarded += line + "\r\n";
        }
        if (authenticationFields != 1) {
            refuse(client, true);
            return;
        }
        if (!tunnel && (hostFields != 1 || input.size() != end + 4)) {
            refuse(client);
            return;
        }
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> connection(curl_easy_init(), curl_easy_cleanup);
        if (!connection)
            return;
        auto* handle = connection.get();
        auto raw = origin;
        raw.setScheme("http");
        raw.setPort(port(origin));
        raw.setPath("/");
        raw.setQuery(QString());
        raw.setFragment(QString());
        const auto endpoint = raw.toEncoded();
        const auto set = [&](CURLoption option, auto value) {
            if (curl_easy_setopt(handle, option, value) != CURLE_OK)
                throw std::runtime_error("Cannot configure media origin route.");
        };
        set(CURLOPT_URL, endpoint.constData());
        network::configureProxy(handle, specification);
        set(CURLOPT_HTTPPROXYTUNNEL, 1L);
        set(CURLOPT_CONNECT_ONLY, 1L);
        set(CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        set(CURLOPT_TIMEOUT_MS, 10000L);
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_NOPROGRESS, 0L);
        set(
            CURLOPT_XFERINFOFUNCTION,
            +[](void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return static_cast<Impl*>(opaque)->stopping ? 1 : 0;
            });
        set(CURLOPT_XFERINFODATA, this);
        if (curl_easy_perform(handle) != CURLE_OK || stopping) {
            refuse(client);
            return;
        }
        QByteArray pending;
        if (tunnel) {
            client.write("HTTP/1.1 200 Connection Established\r\n\r\n");
            pending = input.mid(end + 4);
        } else {
            auto target =
                destination.adjusted(QUrl::RemoveScheme | QUrl::RemoveAuthority | QUrl::RemoveFragment)
                    .toEncoded();
            if (target.isEmpty() || target.startsWith('?'))
                target.prepend('/');
            pending = request[0] + " " + target + " HTTP/1.1\r\n" + forwarded + "Connection: close\r\n\r\n";
        }
        auto active = Clock::now();
        std::array<char, 32 * 1024> bytes{};
        while (!stopping && client.state() == QAbstractSocket::ConnectedState) {
            const auto previousActivity = active;
            if (Clock::now() - active > std::chrono::seconds(30))
                break;
            if (tunnel && pending.size() < bufferLimit) {
                const auto next =
                    client.read(std::min<qsizetype>(bytes.size(), bufferLimit - pending.size()));
                pending += next;
                if (!next.isEmpty())
                    active = Clock::now();
            } else if (!tunnel && client.bytesAvailable() > 0) {
                break; // Never forward a second plain HTTP request or a late request body.
            }
            if (!pending.isEmpty()) {
                std::size_t count = 0;
                const auto status = curl_easy_send(handle, pending.constData(),
                                                   static_cast<std::size_t>(pending.size()), &count);
                if (status != CURLE_OK && status != CURLE_AGAIN)
                    break;
                pending.remove(0, static_cast<qsizetype>(count));
                if (count)
                    active = Clock::now();
            }
            if (client.bytesToWrite() < bufferLimit) {
                std::size_t count = 0;
                const auto available = static_cast<std::size_t>(bufferLimit - client.bytesToWrite());
                const auto status =
                    curl_easy_recv(handle, bytes.data(), std::min(bytes.size(), available), &count);
                if (status == CURLE_OK && !count) {
                    auto draining = Clock::now();
                    auto remaining = client.bytesToWrite();
                    while (!stopping && client.state() == QAbstractSocket::ConnectedState &&
                           client.bytesToWrite()) {
                        client.waitForBytesWritten(50);
                        if (client.bytesToWrite() < remaining) {
                            remaining = client.bytesToWrite();
                            draining = Clock::now();
                        }
                        if (Clock::now() - draining > std::chrono::seconds(30))
                            break;
                    }
                    break;
                }
                if (status != CURLE_OK && status != CURLE_AGAIN)
                    break;
                if (count) {
                    if (client.write(bytes.data(), static_cast<qint64>(count)) < 0)
                        break;
                    active = Clock::now();
                }
            }
            if (client.bytesToWrite())
                client.waitForBytesWritten(10);
            client.waitForReadyRead(active == previousActivity ? 10 : 0);
        }
    }
};
MediaOriginGateway::MediaOriginGateway(const DownloadSpec& spec) : impl_(std::make_unique<Impl>(spec)) {}
MediaOriginGateway::~MediaOriginGateway() = default;
DownloadSpec MediaOriginGateway::routedRequest() const {
    auto spec = impl_->specification;
    spec.proxy = "http://127.0.0.1:" + std::to_string(impl_->localPort);
    spec.proxyUsername = "scope";
    spec.proxyPassword = impl_->token.toStdString();
    spec.proxyAuthenticationRequired = true;
    return spec;
}
} // namespace maskpilot::media_process
