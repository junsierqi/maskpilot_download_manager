#include "connection.h"
#include "../network/curl_runtime.h"
#include "../network/proxy_options.h"
#include <algorithm>
#include <array>
#include <curl/curl.h>
#include <deque>
#include <stdexcept>

namespace maskpilot::ed2k {
struct Connection::Impl {
    CURL* easy = nullptr;
    CURLM* multi = nullptr;
    bool added = false, ready = false;
    FrameDecoder decoder;
    std::deque<Bytes> writes;
    std::size_t writeOffset = 0, pendingWrite = 0;
    ~Impl() {
        if (added)
            curl_multi_remove_handle(multi, easy);
        if (easy)
            curl_easy_cleanup(easy);
        if (multi)
            curl_multi_cleanup(multi);
    }
};
Connection::Connection(const Ed2kEndpoint& endpoint, const DownloadSpec& route,
                       std::shared_ptr<void> endpointLease)
    : endpointLease_(std::move(endpointLease)), impl_(std::make_unique<Impl>()) {
    if (!validateEd2kOptions({{}, {endpoint}}).empty())
        throw std::invalid_argument("Invalid ED2K connection endpoint.");
    network::initializeCurl();
    auto& state = *impl_;
    state.easy = curl_easy_init();
    state.multi = curl_multi_init();
    if (!state.easy || !state.multi)
        throw std::runtime_error("Cannot create ED2K connection.");
    const auto set = [&](CURLoption option, auto value) {
        if (curl_easy_setopt(state.easy, option, value) != CURLE_OK)
            throw std::runtime_error("Cannot configure ED2K connection.");
    };
    const auto url = "http://" + endpoint.host + ':' + std::to_string(endpoint.port) + '/';
    set(CURLOPT_URL, url.c_str());
    set(CURLOPT_CONNECT_ONLY, 1L);
    set(CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_TCP_NODELAY, 1L);
    set(CURLOPT_TCP_KEEPALIVE, 1L);
    // A plain HTTP proxy must establish a tunnel for this binary protocol.
    set(CURLOPT_HTTPPROXYTUNNEL, 1L);
    network::configureProxy(state.easy, route);
    if (curl_multi_add_handle(state.multi, state.easy) != CURLM_OK)
        throw std::runtime_error("Cannot start ED2K connection.");
    state.added = true;
}
Connection::~Connection() = default;
bool Connection::connected() const { return impl_->ready; }
std::size_t Connection::tick(bool receive, std::size_t readLimit) {
    auto& state = *impl_;
    if (!state.ready) {
        int running = 0;
        if (curl_multi_perform(state.multi, &running) != CURLM_OK)
            throw std::runtime_error("ED2K connection failed.");
        int remaining = 0;
        while (const auto* message = curl_multi_info_read(state.multi, &remaining)) {
            if (message->msg != CURLMSG_DONE)
                continue;
            if (message->data.result != CURLE_OK)
                throw std::runtime_error(
                    "ED2K connection failed; check the endpoint and selected proxy route.");
            // Keep the easy handle attached: connect-only send/recv require it.
            state.ready = true;
        }
    }
    if (!state.ready)
        return 0;
    std::size_t sentThisTick = 0;
    while (!state.writes.empty() && sentThisTick < 256 * 1024) {
        const auto& bytes = state.writes.front();
        std::size_t sent = 0;
        const auto result =
            curl_easy_send(state.easy, bytes.data() + state.writeOffset,
                           std::min(bytes.size() - state.writeOffset, 256 * 1024 - sentThisTick), &sent);
        if (result == CURLE_AGAIN)
            break;
        if (result != CURLE_OK || !sent)
            throw std::runtime_error("Cannot send ED2K request.");
        state.writeOffset += sent;
        state.pendingWrite -= sent;
        sentThisTick += sent;
        if (state.writeOffset == bytes.size()) {
            state.writes.pop_front();
            state.writeOffset = 0;
        }
    }
    if (!receive || !readLimit)
        return 0;
    std::array<std::uint8_t, 16 * 1024> bytes;
    const auto capacity =
        std::min({bytes.size(), readLimit, maxFrameBytes + 5 - state.decoder.pendingBytes()});
    if (!capacity)
        return 0;
    std::size_t count = 0;
    const auto result = curl_easy_recv(state.easy, bytes.data(), capacity, &count);
    if (result == CURLE_AGAIN)
        return 0;
    if (result != CURLE_OK || !count)
        throw std::runtime_error("ED2K connection closed.");
    state.decoder.feed(std::span(bytes).first(count));
    return count;
}
void Connection::send(const Frame& frame) {
    auto bytes = encodeFrame(frame);
    if (bytes.size() > 128 * 1024 - impl_->pendingWrite)
        throw std::runtime_error("ED2K pending requests exceed their bound.");
    impl_->pendingWrite += bytes.size();
    impl_->writes.push_back(std::move(bytes));
}
std::optional<Frame> Connection::next() { return impl_->decoder.next(); }
} // namespace maskpilot::ed2k
