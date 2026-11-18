#pragma once
#include <maskpilot/application/ports.h>
#include <maskpilot/application/traffic.h>

namespace maskpilot {
// Opens one immutable run. Caller finishes only after every native callback has stopped.
std::shared_ptr<INativePayloadBudget> makeNativePayloadBudget(std::shared_ptr<IQueueTrafficRepository>,
                                                              const std::string& taskId,
                                                              const std::string& queueId,
                                                              std::function<std::int64_t()> utcNow);
} // namespace maskpilot
