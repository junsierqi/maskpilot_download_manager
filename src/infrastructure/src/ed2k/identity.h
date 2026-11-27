#pragma once
#include "integrity.h"
#include <maskpilot/domain/task.h>

namespace maskpilot::ed2k {
std::string publicRouteKey(const Task& task);
std::string sessionKey(const Task& task, const std::filesystem::path& work);
Hash loadClientIdentity(const Task& task, const std::filesystem::path& work);
} // namespace maskpilot::ed2k
