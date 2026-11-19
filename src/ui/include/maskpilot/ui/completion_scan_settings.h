#pragma once
#include <maskpilot/application/completion_scan.h>
class QSettings;
namespace maskpilot::ui {
CompletionScanOptions loadCompletionScanSettings(QSettings& settings);
void saveCompletionScanSettings(QSettings& settings, const CompletionScanOptions& options);
} // namespace maskpilot::ui
