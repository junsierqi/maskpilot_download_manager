#include <maskpilot/domain/live.h>

namespace maskpilot {
std::string validateLiveRecording(const LiveRecordingOptions& options) {
    if (options.format != LiveFormat::HlsTs && options.format != LiveFormat::Flv)
        return "Invalid live recording format.";
    if (options.durationSeconds < 1 || options.durationSeconds > 172800)
        return "Live recording duration must be between 1 second and 48 hours.";
    if (options.maxBytes < 1024 * 1024 || options.maxBytes > 1024LL * 1024 * 1024 * 1024)
        return "Live recording size must be between 1 MiB and 1 TiB.";
    return {};
}
} // namespace maskpilot
