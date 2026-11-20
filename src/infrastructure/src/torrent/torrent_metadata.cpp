#include "torrent_internal.h"
#include <QString>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace maskpilot::torrent_internal {
namespace lt = libtorrent;
namespace {
void checkComponent(const std::string& component) {
    if (component.find_first_of("/\\") != std::string::npos || !validateTorrentPath(component).empty())
        throw std::invalid_argument("Torrent contains an unsafe original path component.");
}
void checkAttributes(const lt::bdecode_node& file) {
    const auto attributes = file.dict_find_string_value("attr");
    if (attributes.find('l') != decltype(attributes)::npos)
        throw std::invalid_argument("Torrent symlinks are not supported.");
}
void inspectFileTree(const lt::bdecode_node& tree, int depth, std::size_t& count) {
    if (depth > 64 || tree.type() != lt::bdecode_node::dict_t)
        throw std::invalid_argument("Torrent file tree is invalid or too deep.");
    for (int i = 0; i < tree.dict_size(); ++i) {
        const auto [component, child] = tree.dict_at(i);
        if (component.empty()) {
            if (child.type() != lt::bdecode_node::dict_t || ++count > maxTorrentFiles)
                throw std::invalid_argument("Torrent has invalid or too many files.");
            checkAttributes(child);
        } else {
            checkComponent(std::string(component));
            inspectFileTree(child, depth + 1, count);
        }
    }
}
void inspectOriginalPaths(const std::vector<char>& bytes) {
    lt::error_code error;
    const auto decoded = lt::bdecode(lt::span<char const>(bytes.data(), bytes.size()), error);
    if (error || decoded.type() != lt::bdecode_node::dict_t)
        throw std::invalid_argument("Torrent metadata is not a valid metainfo dictionary.");
    const auto info = decoded.dict_find_dict("info");
    if (error || !info)
        throw std::invalid_argument("Torrent metadata is not a valid metainfo dictionary.");
    for (const auto* field : {"name", "name.utf-8"}) {
        const auto name = info.dict_find_string(field);
        if (name)
            checkComponent(std::string(name.string_value()));
    }
    checkAttributes(info);
    const auto files = info.dict_find_list("files");
    if (files) {
        if (files.list_size() > static_cast<int>(maxTorrentFiles))
            throw std::invalid_argument("Torrent contains too many files.");
        for (int i = 0; i < files.list_size(); ++i) {
            const auto file = files.list_at(i);
            if (file.type() != lt::bdecode_node::dict_t)
                throw std::invalid_argument("Torrent has an invalid file entry.");
            checkAttributes(file);
            for (const auto* field : {"path", "path.utf-8"}) {
                const auto path = file.dict_find_list(field);
                if (!path)
                    continue;
                if (path.list_size() < 1 || path.list_size() > 64)
                    throw std::invalid_argument("Torrent path is empty or too deep.");
                for (int j = 0; j < path.list_size(); ++j) {
                    const auto component = path.list_at(j);
                    if (component.type() != lt::bdecode_node::string_t)
                        throw std::invalid_argument("Torrent path component is not text.");
                    checkComponent(std::string(component.string_value()));
                }
            }
        }
    }
    if (const auto tree = info.dict_find_dict("file tree")) {
        std::size_t count = 0;
        inspectFileTree(tree, 0, count);
    }
}
std::wstring foldedPath(const std::string& path) {
#ifdef _WIN32
    auto wide = nativePath(path).wstring();
    const auto size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(),
                                    static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
    if (size <= 0)
        throw std::invalid_argument("Torrent file path contains invalid Unicode.");
    std::wstring folded(static_cast<std::size_t>(size), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(), static_cast<int>(wide.size()),
                       folded.data(), size, nullptr, nullptr, 0))
        throw std::invalid_argument("Cannot normalize torrent file paths.");
    return folded;
#else
    const auto unicode = QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()));
    if (unicode.toUtf8().toStdString() != path)
        throw std::invalid_argument("Torrent file path contains invalid Unicode.");
    return unicode.toUpper().toStdWString();
#endif
}
#ifdef __linux__
bool localMount(const std::filesystem::path& path) {
    // Reject network and automounter paths before opening any component. The
    // subsequent descriptor walk separately refuses symlinks and special files.
    std::ifstream mounts("/proc/self/mountinfo");
    if (!mounts)
        return false;
    std::string line, bestType;
    std::size_t bestLength = 0, entries = 0;
    const auto candidate = path.native();
    while (std::getline(mounts, line)) {
        if (++entries > 16384 || line.size() > 65536)
            return false;
        std::istringstream fields(line);
        std::string id, parent, device, root, encoded, options, token, type;
        if (!(fields >> id >> parent >> device >> root >> encoded >> options))
            return false;
        while (fields >> token && token != "-") {
        }
        if (token != "-" || !(fields >> type))
            return false;
        std::string mount;
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] == '\\') {
                if (i + 3 >= encoded.size() || encoded[i + 1] < '0' || encoded[i + 1] > '7' ||
                    encoded[i + 2] < '0' || encoded[i + 2] > '7' || encoded[i + 3] < '0' ||
                    encoded[i + 3] > '7')
                    return false;
                mount += static_cast<char>((encoded[i + 1] - '0') * 64 + (encoded[i + 2] - '0') * 8 +
                                           encoded[i + 3] - '0');
                i += 3;
            } else
                mount += encoded[i];
        }
        if ((mount == "/" || candidate == mount || candidate.starts_with(mount + "/")) &&
            mount.size() >= bestLength) {
            bestLength = mount.size();
            bestType = std::move(type);
        }
    }
    return bestType == "ext2" || bestType == "ext3" || bestType == "ext4" || bestType == "xfs" ||
           bestType == "btrfs" || bestType == "tmpfs" || bestType == "ramfs" || bestType == "overlay" ||
           bestType == "f2fs" || bestType == "zfs";
}
struct Descriptor {
    int value = -1;
    explicit Descriptor(int descriptor) : value(descriptor) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() {
        if (value >= 0)
            close(value);
    }
    void reset(int next) {
        if (value >= 0)
            close(value);
        value = next;
    }
};
#endif
TorrentMetadata readLocalMetadata(const std::string& source) {
#ifdef _WIN32
    constexpr auto error = "Copy the torrent file to a directly mounted local disk before adding it.";
    auto text = source;
    std::replace(text.begin(), text.end(), '/', '\\');
    // Perform syntactic checks before any filesystem API. In particular, never query a UNC/device root.
    if (text.size() < 4 || text.size() > 32768 ||
        !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z')) || text[1] != ':' ||
        text[2] != '\\' || text.find(':', 2) != std::string::npos ||
        text.find_first_of("<>\"|?*") != std::string::npos ||
        std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::invalid_argument(error);
    const auto path = nativePath(text).lexically_normal();
    const auto drive = path.root_name().wstring();
    if (drive.size() != 2 || drive[1] != L':')
        throw std::invalid_argument(error);
    wchar_t device[32768];
    const auto count = QueryDosDeviceW(drive.c_str(), device, static_cast<DWORD>(std::size(device)));
    if (!count)
        throw std::invalid_argument(error);
    const std::wstring_view mapping(device);
    // SUBST aliases can hide a reparse ancestor. Reject them instead of resolving into an untrusted path.
    if (mapping.starts_with(L"\\??\\") || mapping.starts_with(L"\\DosDevices\\") ||
        mapping.starts_with(L"\\Device\\Mup") || mapping.starts_with(L"\\Device\\LanmanRedirector"))
        throw std::invalid_argument(error);
    const auto type = GetDriveTypeW(path.root_path().c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_CDROM && type != DRIVE_RAMDISK)
        throw std::invalid_argument(error);
    // Walk from the proven local root downward. Querying the whole path first could already follow a
    // junction to a network share and trigger authentication before we notice the reparse point.
    auto cursor = path.root_path();
    for (const auto& component : path.relative_path()) {
        cursor /= component;
        const auto attributes = GetFileAttributesW(cursor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::invalid_argument(error);
    }
    struct SourceFile {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~SourceFile() {
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
        }
    } file;
    file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    BY_HANDLE_FILE_INFORMATION information{};
    LARGE_INTEGER length{};
    if (file.handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.handle, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !GetFileSizeEx(file.handle, &length) || length.QuadPart <= 0 ||
        length.QuadPart > static_cast<LONGLONG>(maxTorrentMetadataBytes))
        throw std::invalid_argument("Local torrent file is unavailable or exceeds the metadata size limit.");
    auto bytes = std::make_shared<std::vector<char>>(static_cast<std::size_t>(length.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.handle, bytes->data(), static_cast<DWORD>(bytes->size()), &read, nullptr) ||
        read != bytes->size())
        throw std::invalid_argument("Cannot read the bounded local torrent file.");
    return bytes;
#elif defined(__linux__)
    constexpr auto error = "Choose a regular torrent file on a supported local disk without symlinks.";
    const auto path = nativePath(source);
    const auto unicodeSource = QString::fromUtf8(source.data(), static_cast<qsizetype>(source.size()));
    if (!path.is_absolute() || path.filename().empty() || path.lexically_normal() != path ||
        unicodeSource.toUtf8().toStdString() != source ||
        source.size() > 32768 || source.starts_with("//") || source.starts_with("\\\\") ||
        std::any_of(source.begin(), source.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
        !localMount(path))
        throw std::invalid_argument(error);
    Descriptor directory{open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory.value < 0)
        throw std::invalid_argument(error);
    std::filesystem::path cursor("/");
    for (const auto& part : path.parent_path().relative_path()) {
        cursor /= part;
        if (!localMount(cursor))
            throw std::invalid_argument(error);
        const auto next =
            openat(directory.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
            throw std::invalid_argument(error);
        directory.reset(next);
    }
    Descriptor file{
        openat(directory.value, path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)};
    struct stat before{}, after{};
    if (file.value < 0 || fstat(file.value, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 || static_cast<std::uint64_t>(before.st_size) > maxTorrentMetadataBytes)
        throw std::invalid_argument(error);
    auto bytes = std::make_shared<std::vector<char>>(static_cast<std::size_t>(before.st_size));
    std::size_t offset = 0;
    while (offset < bytes->size()) {
        const auto count = read(file.value, bytes->data() + offset, bytes->size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::invalid_argument("Cannot read the bounded local torrent file.");
        offset += static_cast<std::size_t>(count);
    }
    if (fstat(file.value, &after) != 0 || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        throw std::invalid_argument("Local torrent metadata changed while being captured.");
    return bytes;
#else
    (void)source;
    throw std::invalid_argument(
        "Local torrent file capture is currently supported on Windows local disks only.");
#endif
}
template <typename Hash> std::string hexHash(const Hash& hash) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    const auto bytes = hash.to_string();
    result.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        result.push_back(alphabet[byte >> 4]);
        result.push_back(alphabet[byte & 15]);
    }
    return result;
}
} // namespace
TorrentMetadata readMetadata(const DownloadSpec& spec) {
    if (spec.torrent.metadata)
        return spec.torrent
            .metadata; // A historical source path is never opened when bytes are already captured.
    if (spec.url.starts_with("magnet:?"))
        return {};
    if (!spec.url.starts_with("torrent:"))
        throw std::invalid_argument("A local torrent source or magnet link is required.");
    return readLocalMetadata(spec.url.substr(8));
}
std::filesystem::path nativePath(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
std::string pathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
std::string hashIdentity(const lt::info_hash_t& hashes) {
    std::string result;
    if (hashes.has_v1())
        result = "v1:" + hexHash(hashes.v1);
    if (hashes.has_v2())
        result += (result.empty() ? "" : "|") + std::string("v2:") + hexHash(hashes.v2);
    if (result.empty())
        throw std::invalid_argument("Torrent is missing a content identity.");
    return result;
}
bool hashesMatch(const lt::info_hash_t& expected, const lt::info_hash_t& actual) {
    return (!expected.has_v1() || (actual.has_v1() && expected.v1 == actual.v1)) &&
           (!expected.has_v2() || (actual.has_v2() && expected.v2 == actual.v2)) &&
           (expected.has_v1() || expected.has_v2());
}
TransferDescription describe(const lt::torrent_info& info, const std::string& identity) {
    const auto& storage = info.layout();
    if (storage.num_files() < 1 || storage.num_files() > static_cast<int>(maxTorrentFiles))
        throw std::invalid_argument("Torrent file count is unsupported.");
    auto files = std::make_shared<std::vector<TorrentFile>>();
    files->reserve(static_cast<std::size_t>(storage.num_files()));
    std::set<std::wstring> filePaths, directories, paddingPaths, paddingDirectories;
    std::int64_t total = 0;
    for (int n = 0; n < storage.num_files(); ++n) {
        const lt::file_index_t index(n);
        if (storage.file_flags(index) & lt::file_storage::flag_symlink)
            throw std::invalid_argument("Torrent symlinks are not supported.");
        auto path = storage.file_path(index);
        std::replace(path.begin(), path.end(), '\\', '/');
        if (const auto error = validateTorrentPath(path); !error.empty())
            throw std::invalid_argument(error);
        const auto size = storage.file_size(index);
        if (size < 0 || size > std::numeric_limits<std::int64_t>::max() - total)
            throw std::invalid_argument("Torrent size is invalid or too large.");
        total += size;
        const bool padding = storage.pad_file_at(index);
        const auto folded = foldedPath(path);
        if (!padding) {
            if (filePaths.contains(folded) || directories.contains(folded) || paddingPaths.contains(folded) ||
                paddingDirectories.contains(folded))
                throw std::invalid_argument("Torrent contains colliding Windows file paths.");
            for (auto slash = folded.find(L'/'); slash != std::wstring::npos;
                 slash = folded.find(L'/', slash + 1)) {
                const auto directory = folded.substr(0, slash);
                if (filePaths.contains(directory) || paddingPaths.contains(directory))
                    throw std::invalid_argument("Torrent file path conflicts with a directory.");
                directories.insert(directory);
            }
            filePaths.insert(folded);
        } else {
            if (filePaths.contains(folded) || directories.contains(folded))
                throw std::invalid_argument("Torrent padding conflicts with a payload file path.");
            for (auto slash = folded.find(L'/'); slash != std::wstring::npos;
                 slash = folded.find(L'/', slash + 1)) {
                const auto directory = folded.substr(0, slash);
                if (filePaths.contains(directory))
                    throw std::invalid_argument("Torrent padding conflicts with a payload directory.");
                paddingDirectories.insert(directory);
            }
            paddingPaths.insert(folded); // Repeated virtual padding names are allowed.
        }
        files->push_back({n, std::move(path), size, padding});
    }
    if (filePaths.empty())
        throw std::invalid_argument("Torrent contains no downloadable payload files.");
    return {info.name(), identity.empty() ? hashIdentity(info.info_hashes()) : identity, files};
}
ParsedTorrent parse(const DownloadSpec& spec) {
    ParsedTorrent result;
    lt::add_torrent_params magnet;
    const bool isMagnet = spec.url.starts_with("magnet:?");
    if (isMagnet) {
        lt::error_code error;
        lt::parse_magnet_uri(spec.url, magnet, error);
        if (error)
            throw std::invalid_argument("Magnet link is invalid or has no supported content identity.");
        result.params = magnet;
        result.description = {
            magnet.name.empty() ? "BitTorrent" : magnet.name, hashIdentity(magnet.info_hashes), {}};
    }
    result.metadata = readMetadata(spec);
    if (result.metadata) {
        if (result.metadata->empty() || result.metadata->size() > maxTorrentMetadataBytes)
            throw std::invalid_argument("Torrent metadata is empty or too large.");
        inspectOriginalPaths(*result.metadata);
        lt::error_code error;
        lt::load_torrent_limits limits;
        limits.max_buffer_size = static_cast<int>(maxTorrentMetadataBytes);
        result.params = lt::load_torrent_buffer(
            lt::span<char const>(result.metadata->data(), result.metadata->size()), error, limits);
        if (error || !result.params.ti)
            throw std::invalid_argument("Torrent metadata cannot be parsed.");
        if (isMagnet && !hashesMatch(magnet.info_hashes, result.params.ti->info_hashes()))
            throw std::invalid_argument("Torrent metadata does not match the magnet identity.");
        result.description = describe(*result.params.ti, isMagnet ? hashIdentity(magnet.info_hashes) : "");
        if (isMagnet) {
            result.params.peers = magnet.peers;
            for (const auto& tracker : magnet.trackers)
                if (std::find(result.params.trackers.begin(), result.params.trackers.end(), tracker) ==
                    result.params.trackers.end())
                    result.params.trackers.push_back(tracker);
            for (const auto& seed : magnet.url_seeds)
                if (std::find(result.params.url_seeds.begin(), result.params.url_seeds.end(), seed) ==
                    result.params.url_seeds.end())
                    result.params.url_seeds.push_back(seed);
        }
        if (!spec.torrent.allFiles)
            for (const auto index : spec.torrent.selectedFiles)
                if (index < 0 || index >= static_cast<int>(result.description.files->size()) ||
                    (*result.description.files)[static_cast<std::size_t>(index)].padding)
                    throw std::invalid_argument("Torrent selection contains an invalid file index.");
    } else if (!isMagnet) {
        throw std::invalid_argument("A torrent file or magnet link is required.");
    }
    if (result.params.peers.size() > 256 || result.params.trackers.size() > 256 ||
        result.params.url_seeds.size() > 256)
        throw std::invalid_argument("Torrent contains too many initial peers, trackers or web seeds.");
    return result;
}
TorrentMetadata metadataFrom(const lt::add_torrent_params& params) {
    // Magnet metadata is the hash-bound info dictionary. An in-progress v2 resume contains partial
    // Merkle trees, which write_torrent_file_buf rejects even with allow_missing_piece_layer. Persist
    // the authoritative metainfo without those optional layers; libtorrent explicitly accepts this
    // form and obtains/verifies the necessary hashes when the transfer starts.
    lt::add_torrent_params metainfo;
    metainfo.ti = params.ti;
    metainfo.info_hashes = params.info_hashes;
    metainfo.name = params.name;
    metainfo.trackers = params.trackers;
    metainfo.tracker_tiers = params.tracker_tiers;
    metainfo.url_seeds = params.url_seeds;
    metainfo.comment = params.comment;
    metainfo.created_by = params.created_by;
    metainfo.creation_date = params.creation_date;
    auto bytes = std::make_shared<std::vector<char>>(
        lt::write_torrent_file_buf(metainfo, lt::write_flags::allow_missing_piece_layer));
    if (bytes->empty() || bytes->size() > maxTorrentMetadataBytes)
        throw std::invalid_argument("Received torrent metadata exceeds the supported size.");
    return bytes;
}
} // namespace maskpilot::torrent_internal

namespace maskpilot {
TransferDescription describeTorrent(const DownloadSpec& spec) {
    return torrent_internal::parse(spec).description;
}
} // namespace maskpilot
