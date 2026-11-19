#pragma once
#include <maskpilot/application/completion_scan.h>
namespace maskpilot {
int runSiteProjectCommand(int argc, char** argv, CompletionScanDependencies scanning = {});
}
