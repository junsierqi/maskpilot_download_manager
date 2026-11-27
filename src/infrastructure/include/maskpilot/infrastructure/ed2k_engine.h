#pragma once
#include <maskpilot/application/ports.h>
#include <maskpilot/domain/ed2k.h>

namespace maskpilot {
struct Ed2kResources;
std::shared_ptr<Ed2kResources> makeEd2kResources();
// Standalone task transport; options never carry origin credentials or browser authority.
std::unique_ptr<ITransferTransport> makeEd2kTransfer(Ed2kOptions options = {},
                                                     std::shared_ptr<Ed2kResources> resources = {});
} // namespace maskpilot
