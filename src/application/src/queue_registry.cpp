#include "queue_registry.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
namespace maskpilot {
namespace {
class UtcCivilTime final : public IQueueCivilTime {
  public:
    std::int64_t utcNow() const override {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
    std::optional<CivilMinute> at(const std::string& zone, std::int64_t utc) const override {
        if (zone != "UTC")
            return std::nullopt;
        auto day = utc / 86400, second = utc % 86400;
        if (second < 0) {
            second += 86400;
            --day;
        }
        return CivilMinute{static_cast<int>((day % 7 + 10) % 7), static_cast<int>(second / 60)};
    }
};
constexpr auto infinity = std::numeric_limits<std::int64_t>::max() / 4;
std::int64_t bounded(std::int64_t value) { return value == 0 ? infinity : std::min(value, infinity); }
std::vector<std::int64_t> share(std::int64_t budget, const std::vector<std::int64_t>& caps) {
    if (budget == infinity)
        return caps;
    std::vector<std::int64_t> out(caps.size());
    std::vector<std::size_t> pending;
    for (std::size_t i = 0; i < caps.size(); ++i)
        pending.push_back(i);
    while (!pending.empty()) {
        auto portion = budget / static_cast<std::int64_t>(pending.size());
        bool capped = false;
        for (auto it = pending.begin(); it != pending.end();)
            if (caps[*it] <= portion) {
                out[*it] = caps[*it];
                budget -= out[*it];
                it = pending.erase(it);
                capped = true;
            } else
                ++it;
        if (capped)
            continue;
        for (auto i : pending) {
            out[i] = portion + (budget % static_cast<std::int64_t>(pending.size()) > 0 ? 1 : 0);
            if (budget % static_cast<std::int64_t>(pending.size()) > 0)
                --budget;
        }
        break;
    }
    return out;
}
} // namespace
QueueRegistry::QueueRegistry(QueueDependencies dependencies)
    : repository_(std::move(dependencies.repository)),
      clock_(dependencies.clock ? std::move(dependencies.clock) : std::make_shared<UtcCivilTime>()) {
    if (!repository_) {
        queues_.emplace(defaultQueueId, DownloadQueue{});
        return;
    }
    for (auto queue : repository_->loadQueues()) {
        if (!validateDownloadQueue(queue).empty() || !queues_.emplace(queue.id, std::move(queue)).second)
            throw std::runtime_error("Saved queue configuration is invalid.");
    }
    if (!queues_.contains(defaultQueueId) || queues_.size() > maxDownloadQueues)
        throw std::runtime_error("Saved default queue is unavailable.");
    std::vector<std::string> names;
    for (auto& [id, q] : queues_) {
        if (std::find(names.begin(), names.end(), q.name) != names.end())
            throw std::runtime_error("Saved queue names collide.");
        names.push_back(q.name);
    }
}
const DownloadQueue& QueueRegistry::get(const std::string& id) const {
    auto it = queues_.find(id);
    if (it == queues_.end())
        throw std::invalid_argument("Download queue is unavailable.");
    return it->second;
}
std::vector<DownloadQueue> QueueRegistry::list() const {
    std::vector<DownloadQueue> result;
    for (auto& [id, q] : queues_)
        result.push_back(q);
    return result;
}
std::int64_t QueueRegistry::now() const { return clock_->utcNow(); }
bool QueueRegistry::available(const std::string& id, std::int64_t utc) const {
    const auto& q = get(id);
    if (!q.enabled)
        return false;
    if (!q.schedule.enabled)
        return true;
    auto minute = utc / 60;
    if (utc % 60 < 0)
        --minute;
    if (!cachedMinute_ || *cachedMinute_ != minute) {
        civilCache_.clear();
        cachedMinute_ = minute;
    }
    auto found = civilCache_.find(q.schedule.timeZone);
    if (found == civilCache_.end()) {
        std::optional<CivilMinute> value;
        try {
            value = clock_->at(q.schedule.timeZone, utc);
        } catch (const std::exception&) {
        }
        if (civilCache_.size() >= maxDownloadQueues)
            civilCache_.clear();
        found = civilCache_.emplace(q.schedule.timeZone, value).first;
    }
    if (!found->second)
        return false;
    return weeklyWindowContains(q.schedule, found->second->weekday, found->second->minute);
}
DownloadQueue QueueRegistry::prepare(QueueConfiguration config) const {
    if (!repository_)
        throw std::invalid_argument("Queue configuration requires persistent queue storage.");
    if (config.id.empty())
        config.id = newTaskId();
    auto found = queues_.find(config.id);
    if (found == queues_.end()) {
        if (config.expectedRevision != 0 || queues_.size() >= maxDownloadQueues)
            throw std::invalid_argument("Queue revision or count is invalid.");
    } else if (found->second.revision != config.expectedRevision)
        throw std::invalid_argument("Queue changed. Refresh and retry.");
    if (config.expectedRevision == std::numeric_limits<std::int64_t>::max())
        throw std::invalid_argument("Queue revision is exhausted.");
    DownloadQueue q{config.id,
                    config.name,
                    config.expectedRevision + 1,
                    config.enabled,
                    config.maxActive,
                    config.downloadLimit,
                    config.uploadLimit,
                    config.schedule,
                    config.dailyPayloadBudget,
                    config.payloadWindowHours};
    if (auto error = validateDownloadQueue(q); !error.empty())
        throw std::invalid_argument(error);
    if (q.schedule.enabled) {
        try {
            if (!clock_->at(q.schedule.timeZone, now()))
                throw std::invalid_argument("The selected queue time zone is unavailable.");
        } catch (const std::exception&) {
            throw std::invalid_argument("The selected queue time zone is unavailable.");
        }
    }
    for (auto& [id, old] : queues_)
        if (id != q.id && old.name == q.name)
            throw std::invalid_argument("Choose a unique queue name.");
    return q;
}
void QueueRegistry::commit(const DownloadQueue& q, std::int64_t expected) {
    repository_->saveQueue(q, expected);
    queues_.insert_or_assign(q.id, q);
    civilCache_.clear();
}
void QueueRegistry::remove(const std::string& id, std::int64_t expected) {
    if (!repository_ || id == defaultQueueId || get(id).revision != expected)
        throw std::invalid_argument("Queue cannot be removed or has changed.");
    repository_->eraseQueue(id, expected);
    queues_.erase(id);
}
void QueueRegistry::move(const std::vector<QueueMove>& moves, const std::string& target, std::int64_t now) {
    get(target);
    if (!repository_)
        throw std::invalid_argument("Moving tasks requires persistent queue storage.");
    repository_->moveTasks(moves, target, now);
}
std::int64_t mediaReservation(std::int64_t global, int activeSlots, const DownloadQueue& q,
                              std::int64_t cap) {
    auto value = bounded(cap);
    if (global > 0)
        value = std::min(value, global / std::max(1, activeSlots));
    if (q.downloadLimit > 0)
        value = std::min(value, q.downloadLimit / q.maxActive);
    if (value < 1)
        throw std::invalid_argument("Pause tasks before reducing bandwidth below active reservations.");
    return value == infinity ? 0 : value;
}
std::map<std::string, std::int64_t> allocateQueueRates(std::int64_t global,
                                                       const std::vector<DownloadQueue>& policies,
                                                       const std::vector<RateClaim>& claims, bool upload) {
    std::map<std::string, std::int64_t> result;
    std::int64_t remaining = bounded(global);
    std::map<std::string, std::int64_t> available;
    for (auto& q : policies)
        available[q.id] = bounded(upload ? q.uploadLimit : q.downloadLimit);
    std::map<std::string, std::vector<const RateClaim*>> groups;
    for (auto& c : claims) {
        if (!available.contains(c.queueId))
            throw std::invalid_argument("Download queue is unavailable.");
        if (c.fixed >= 0) {
            auto fixed = c.fixed;
            if ((fixed == 0 && (available[c.queueId] != infinity || remaining != infinity)) ||
                fixed > available[c.queueId] || fixed > remaining)
                throw std::invalid_argument("Pause media tasks before changing bandwidth limits.");
            if (available[c.queueId] != infinity)
                available[c.queueId] -= fixed;
            if (remaining != infinity)
                remaining -= fixed;
            result[c.id] = c.fixed;
        } else
            groups[c.queueId].push_back(&c);
    }
    std::vector<std::string> ids;
    std::vector<std::int64_t> demands;
    for (auto& [id, items] : groups) {
        std::int64_t demand = 0;
        for (auto* c : items)
            demand = std::min(infinity, demand + bounded(c->cap));
        ids.push_back(id);
        demands.push_back(std::min(available[id], demand));
    }
    auto groupShares = share(remaining, demands);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto& items = groups.at(ids[i]);
        std::vector<std::int64_t> caps;
        for (auto* c : items)
            caps.push_back(bounded(c->cap));
        auto rates = share(groupShares[i], caps);
        for (std::size_t j = 0; j < items.size(); ++j) {
            if (rates[j] < 1)
                throw std::invalid_argument("Pause tasks before exhausting a finite bandwidth allocation.");
            result[items[j]->id] = rates[j] == infinity ? 0 : rates[j];
        }
    }
    return result;
}
} // namespace maskpilot
