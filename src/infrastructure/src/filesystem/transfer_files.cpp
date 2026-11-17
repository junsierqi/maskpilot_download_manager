#include "transfer_files.h"
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace maskpilot::transfer_files {
FILE* openFile(const std::filesystem::path& path, const wchar_t* mode) {
#ifdef _WIN32
    FILE* file = nullptr;
    _wfopen_s(&file, path.c_str(), mode);
    return file;
#else
    return fopen(path.c_str(), mode[0] == L'a' ? "ab" : mode[0] == L'r' ? "rb" : "wb");
#endif
}
void syncFile(FILE* file) {
    if (fflush(file) != 0)
        throw std::runtime_error("Cannot flush downloaded data.");
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0)
#else
    if (fsync(fileno(file)) != 0)
#endif
        throw std::runtime_error("Cannot synchronize downloaded data.");
}
void replaceOwnedFile(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot commit recovery metadata.");
#else
    std::filesystem::rename(from, to);
#endif
}
void atomicText(const std::filesystem::path& path, const std::string& data) {
    auto temp = path;
    temp += L".new";
    auto* file = openFile(temp, L"wb");
    if (!file)
        throw std::runtime_error("Cannot write recovery metadata.");
    try {
        if (fwrite(data.data(), 1, data.size(), file) != data.size())
            throw std::runtime_error("Cannot write recovery metadata.");
        syncFile(file);
        fclose(file);
        file = nullptr;
        replaceOwnedFile(temp, path);
    } catch (...) {
        if (file)
            fclose(file);
        throw;
    }
}
void publishFile(const std::filesystem::path& stage, const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(stage.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish completed file. Existing files were preserved.");
#else
    std::filesystem::create_hard_link(stage, destination);
    std::filesystem::remove(stage);
#endif
}
} // namespace maskpilot::transfer_files
