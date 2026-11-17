#include <algorithm>
#include <cctype>
#include <filesystem>
#include <maskpilot/domain/torrent.h>
#include <system_error>

namespace maskpilot {
std::string validateTorrentPath(const std::string& relativePath) {
    if (relativePath.empty() || relativePath.size() > 8192 || relativePath.front() == '/' ||
        relativePath.front() == '\\' || relativePath.find(':') != std::string::npos)
        return "Torrent contains an absolute, empty or unsupported file path.";
    auto text = relativePath;
    std::replace(text.begin(), text.end(), '\\', '/');
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find('/', begin);
        const auto component = text.substr(begin, end == std::string::npos ? end : end - begin);
        if (component.empty() || component == "." || component == ".." || component.back() == '.' ||
            component.back() == ' ' || component.find_first_of("<>\"|?*") != std::string::npos ||
            std::any_of(component.begin(), component.end(),
                        [](unsigned char c) { return c < 32 || c == 127; }))
            return "Torrent contains an unsafe file path component.";
        std::string lower = component;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.starts_with(".maskpilot"))
            return "Torrent file path conflicts with reserved recovery files.";
        const auto base = lower.substr(0, lower.find('.'));
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" ||
            base == "conout$" ||
            (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' &&
             base[3] <= '9') ||
            ((base.starts_with("com") || base.starts_with("lpt")) &&
             (base.substr(3) == "\xC2\xB9" || base.substr(3) == "\xC2\xB2" || base.substr(3) == "\xC2\xB3")))
            return "Torrent contains a reserved Windows device name.";
        try {
            const std::filesystem::path path(std::u8string(component.begin(), component.end()));
            if (path.native().size() > 255)
                return "Torrent file path component is too long.";
        } catch (const std::system_error&) {
            return "Torrent file path contains invalid Unicode.";
        }
        if (end == std::string::npos)
            return {};
        begin = end + 1;
    }
    return "Torrent file path ends with a directory separator.";
}
bool torrentFileWanted(const TorrentOptions& options, const TorrentFile& file) {
    return !file.padding &&
           (options.allFiles || std::find(options.selectedFiles.begin(), options.selectedFiles.end(),
                                          file.index) != options.selectedFiles.end());
}
} // namespace maskpilot
