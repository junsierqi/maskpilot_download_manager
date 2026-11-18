// This launcher is the only FFmpeg/ffprobe entry point exposed to media parsers.
// Downloads belong to the transfer engine; these tools may process owned local files only.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using Text = fs::path::string_type;
#ifdef _WIN32
#define TEXT_VALUE(value) L##value
Text environment(const wchar_t* name) {
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size)
        return {};
    Text value(size, L'\0');
    const auto written = GetEnvironmentVariableW(name, value.data(), size);
    if (!written || written >= size)
        return {};
    value.resize(written);
    return value;
}
#else
#define TEXT_VALUE(value) value
Text environment(const char* name) {
    const auto* value = std::getenv(name);
    return value ? value : "";
}
#endif
bool contained(const fs::path& root, const fs::path& path) {
    auto child = path.begin();
    for (const auto& part : root) {
        if (child == path.end())
            return false;
        auto a = part.native(), b = child->native();
#ifdef _WIN32
        std::transform(a.begin(), a.end(), a.begin(), towlower);
        std::transform(b.begin(), b.end(), b.begin(), towlower);
#endif
        if (a != b)
            return false;
        ++child;
    }
    return child != path.end();
}
void localPath(Text value, const fs::path& root, bool input) {
    if (!input && value == TEXT_VALUE("-"))
        return;
    if (value.starts_with(TEXT_VALUE("file:")))
        value.erase(0, 5);
    if (value.empty() || value.starts_with(TEXT_VALUE("\\\\")) || value.starts_with(TEXT_VALUE("//")))
        throw std::runtime_error("Invalid local path");
    const auto colon = value.find(TEXT_VALUE(':'));
#ifdef _WIN32
    if (colon != Text::npos && (colon != 1 || value.size() < 3 || (value[2] != L'\\' && value[2] != L'/')))
        throw std::runtime_error("Remote media input is disabled");
    if (colon != Text::npos && value.find(L':', colon + 1) != Text::npos)
        throw std::runtime_error("Alternate streams are disabled");
#else
    if (colon != Text::npos)
        throw std::runtime_error("Remote media input is disabled");
#endif
    fs::path path(value);
    if (path.is_relative())
        path = root / path;
    const auto canonical = input ? fs::canonical(path) : fs::weakly_canonical(path);
    if (!contained(root, canonical) || (input && !fs::is_regular_file(canonical)))
        throw std::runtime_error("Media path escapes its work directory");
}
std::vector<Text> guardedArguments(int argc, const fs::path::value_type* const* argv, const fs::path& root,
                                   bool probe) {
    const Text protocols = TEXT_VALUE("file,pipe");
    // Exclude playlist/script demuxers even for files deceptively named .mp4.
    const Text formats =
        TEXT_VALUE("mov,matroska,webm,mpegts,mp3,flac,ogg,aac,wav,h264,hevc,ivf,obu,m4v,avi,flv");
    std::vector<Text> result;
    auto guard = [&] {
        result.insert(result.end(), {TEXT_VALUE("-protocol_whitelist"), protocols,
                                     TEXT_VALUE("-format_whitelist"), formats});
    };
    bool input = false;
    for (int i = 1; i < argc; ++i) {
        const Text argument(argv[i]);
        if (argument == TEXT_VALUE("-i")) {
            if (++i >= argc)
                throw std::runtime_error("Missing media input");
            localPath(argv[i], root, true);
            guard();
            result.emplace_back(TEXT_VALUE("-i"));
            result.emplace_back(argv[i]);
            input = true;
        } else if (probe && i == argc - 1) {
            localPath(argument, root, true);
            guard();
            result.push_back(argument);
            input = true;
        } else {
            result.push_back(argument);
        }
    }
    if (!input)
        throw std::runtime_error("Missing local media input");
    if (!probe && argc > 2 && Text(argv[argc - 2]) != TEXT_VALUE("-i"))
        localPath(argv[argc - 1], root, false);
    return result;
}
bool query(int argc, const fs::path::value_type* const* argv) {
    return argc == 2 && (Text(argv[1]) == TEXT_VALUE("-version") || Text(argv[1]) == TEXT_VALUE("-bsfs") ||
                         Text(argv[1]) == TEXT_VALUE("-filters"));
}
#ifdef _WIN32
Text quote(const Text& value) {
    Text result = L"\"";
    size_t slashes = 0;
    for (auto ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
        if (ch == L'"')
            result += L'\\';
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
int run(const fs::path& executable, const std::vector<Text>& args, const fs::path& root) {
    for (const auto* name :
         {L"HTTP_PROXY", L"HTTPS_PROXY", L"FTP_PROXY", L"ALL_PROXY", L"NO_PROXY", L"FFREPORT"})
        SetEnvironmentVariableW(name, nullptr);
    Text command = quote(executable.native());
    for (const auto& arg : args)
        command += L" " + quote(arg);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        root.c_str(), &startup, &process))
        throw std::runtime_error("Cannot start local media tool");
    CloseHandle(process.hThread);
    const auto waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
#else
int run(const fs::path& executable, const std::vector<Text>& args, const fs::path& root) {
    for (const auto* name : {"http_proxy", "https_proxy", "ftp_proxy", "all_proxy", "no_proxy", "HTTP_PROXY",
                             "HTTPS_PROXY", "FTP_PROXY", "ALL_PROXY", "NO_PROXY", "FFREPORT"})
        unsetenv(name);
    if (chdir(root.c_str()))
        throw std::runtime_error("Cannot enter local media directory");
    std::vector<char*> raw{const_cast<char*>(executable.c_str())};
    for (const auto& arg : args)
        raw.push_back(const_cast<char*>(arg.c_str()));
    raw.push_back(nullptr);
    execv(executable.c_str(), raw.data());
    throw std::runtime_error("Cannot start local media tool");
}
#endif
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
#ifdef MASKPILOT_MEDIA_PROBE
        constexpr bool probe = true;
        auto executable = environment(TEXT_VALUE("MASKPILOT_MEDIA_REAL_FFPROBE"));
#else
        constexpr bool probe = false;
        auto executable = environment(TEXT_VALUE("MASKPILOT_MEDIA_REAL_FFMPEG"));
#endif
        const auto directory = environment(TEXT_VALUE("MASKPILOT_MEDIA_WORK_DIR"));
        if (executable.empty() || directory.empty())
            throw std::runtime_error("Media tools are not configured");
        const auto root = fs::canonical(fs::path(directory));
        const auto real = fs::canonical(fs::path(executable));
#ifdef _WIN32
        std::vector<wchar_t> module(32768);
        const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (!length || length >= module.size())
            throw std::runtime_error("Cannot identify media launcher");
        const fs::path self(Text(module.data(), length));
#else
        const fs::path self(argv[0]);
#endif
        if (!fs::is_directory(root) || !fs::is_regular_file(real) || fs::equivalent(real, self))
            throw std::runtime_error("Invalid media configuration");
        std::vector<Text> args;
        if (query(argc, argv))
            args.emplace_back(argv[1]);
        else
            args = guardedArguments(argc, argv, root, probe);
        return run(real, args, root);
    } catch (...) {
        std::fputs("Local media processing was rejected. Check the media input and configured tools.\n",
                   stderr);
        return 1;
    }
}
