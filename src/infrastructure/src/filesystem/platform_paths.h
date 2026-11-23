#pragma once
#include <filesystem>

namespace maskpilot::platform_paths {
// These two root-owned Darwin aliases are part of the platform layout. This
// does not authorize following links created inside a user-selected directory.
inline bool isSystemDirectoryAlias(const std::filesystem::path& path) {
#ifdef __APPLE__
    namespace fs = std::filesystem;
    if (path != fs::path("/var") && path != fs::path("/tmp"))
        return false;
    std::error_code error;
    const auto target = fs::read_symlink(path, error);
    const auto expected = fs::path("/private") / path.filename();
    if (error || (path.parent_path() / target).lexically_normal() != expected)
        return false;
    const auto parent = fs::symlink_status("/private", error);
    if (error || !fs::is_directory(parent))
        return false;
    const auto directory = fs::symlink_status(expected, error);
    return !error && fs::is_directory(directory);
#else
    (void)path;
    return false;
#endif
}
} // namespace maskpilot::platform_paths
