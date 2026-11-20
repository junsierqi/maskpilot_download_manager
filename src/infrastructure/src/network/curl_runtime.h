#pragma once

namespace maskpilot::network {
// One process-lifetime reference shared by native transports and authenticated media routing.
void initializeCurl();
} // namespace maskpilot::network
