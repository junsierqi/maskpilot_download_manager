#pragma once
#include <functional>
#include <map>
#include <maskpilot/application/browser_recovery.h>
#include <maskpilot/domain/task.h>

namespace maskpilot {
// Coordinator-thread confined. Neither capabilities nor applied receipts are persisted.
class BrowserRecoveryRegistry {
  public:
    BrowserRecoverySession begin(const Task&, std::int64_t now);
    std::string taskId(const std::string& token, std::int64_t now);
    std::string appliedTaskId(const std::string& token, std::int64_t now);
    void apply(const std::string& token, const Task&, BrowserRecoveryMaterial, std::int64_t now,
               const std::function<void(Task&)>& commit);
    void cancel(const std::string& token);
    void invalidate(const std::string& taskId);

  private:
    struct Entry {
        std::string taskId;
        std::int64_t issuedAt = 0, expiresAt = 0;
        bool valid = true, applied = false;
        BrowserRecoveryMaterial material;
    };
    std::map<std::string, Entry> entries_;
    Entry& current(const std::string&, std::int64_t now);
    static void eligible(const Task&);
};
} // namespace maskpilot
