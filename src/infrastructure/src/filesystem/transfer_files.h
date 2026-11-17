#pragma once
#include <cstdio>
#include <filesystem>
#include <string>

// Private native I/O shared by protocol adapters; no UI or protocol policy here.
namespace maskpilot::transfer_files {
FILE* openFile(const std::filesystem::path& path, const wchar_t* mode);
void syncFile(FILE* file);
void replaceOwnedFile(const std::filesystem::path& from, const std::filesystem::path& to);
void atomicText(const std::filesystem::path& path, const std::string& data);
void publishFile(const std::filesystem::path& stage, const std::filesystem::path& destination);
} // namespace maskpilot::transfer_files
