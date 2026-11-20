#pragma once
#include <cstdint>
#include <string>

namespace maskpilot {
enum class LiveFormat { HlsTs, Flv };
struct LiveRecordingOptions {
    LiveFormat format = LiveFormat::HlsTs;
    int durationSeconds = 3600;
    std::int64_t maxBytes = 1024LL * 1024 * 1024;
    bool operator==(const LiveRecordingOptions&) const = default;
};
std::string validateLiveRecording(const LiveRecordingOptions&);
} // namespace maskpilot
