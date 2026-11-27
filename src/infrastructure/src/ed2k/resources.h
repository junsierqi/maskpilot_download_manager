#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace maskpilot {
// Resource and endpoint arbitration only. No sockets, identities or credentials are pooled here.
struct Ed2kResources : std::enable_shared_from_this<Ed2kResources> {
    std::atomic_size_t parts{0}, tasks{0};
    struct Endpoint {
        std::string holder;
        std::deque<std::string> waiting;
    };
    std::mutex mutex;
    std::map<std::string, Endpoint> endpoints;
    std::map<std::string, std::chrono::steady_clock::time_point> indexRequests;
    std::shared_ptr<void> tryPeer(const std::string& key, const std::string& task, bool index = false) {
        {
            std::lock_guard lock(mutex);
            auto& endpoint = endpoints[key];
            if (endpoint.holder == task)
                return {};
            if (std::find(endpoint.waiting.begin(), endpoint.waiting.end(), task) == endpoint.waiting.end())
                if (endpoint.waiting.size() < 128)
                    endpoint.waiting.push_back(task);
                else
                    throw std::runtime_error("ED2K endpoint wait queue exceeds its bound.");
            if (!endpoint.holder.empty() || endpoint.waiting.front() != task)
                return {};
            if (index) {
                const auto now = std::chrono::steady_clock::now();
                if (indexRequests.size() > 512)
                    std::erase_if(indexRequests, [&](const auto& entry) { return entry.second <= now; });
                auto& allowed = indexRequests[key];
                if (allowed > now)
                    return {};
                allowed = now + std::chrono::seconds(20);
            }
            endpoint.waiting.pop_front();
            endpoint.holder = task;
        }
        return {this, [owner = shared_from_this(), key, task](void*) {
                    std::lock_guard lock(owner->mutex);
                    const auto found = owner->endpoints.find(key);
                    if (found == owner->endpoints.end())
                        return;
                    if (found->second.holder == task)
                        found->second.holder.clear();
                    if (found->second.holder.empty() && found->second.waiting.empty())
                        owner->endpoints.erase(found);
                }};
    }
    bool peerWaiting(const std::string& key) {
        std::lock_guard lock(mutex);
        const auto found = endpoints.find(key);
        return found != endpoints.end() && !found->second.waiting.empty();
    }
    void forget(const std::string& task) {
        std::lock_guard lock(mutex);
        for (auto it = endpoints.begin(); it != endpoints.end();) {
            std::erase(it->second.waiting, task);
            if (it->second.holder.empty() && it->second.waiting.empty())
                it = endpoints.erase(it);
            else
                ++it;
        }
    }
};
} // namespace maskpilot
