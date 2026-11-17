#include <algorithm>
#include <chrono>
#include <iostream>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
void readOptions(maskpilot::DownloadSpec& spec, int argc, char** argv, int start) {
    for (int n = start; n < argc; ++n) {
        const std::string option = argv[n];
        if (option == "--ftp-explicit-tls")
            spec.ftpExplicitTls = true;
        else if (option == "--ftp-active")
            spec.ftpActiveMode = true;
        else {
            if (++n >= argc)
                throw std::invalid_argument("Missing option value.");
            const std::string value = argv[n];
            if (option == "--connections")
                spec.connections = std::stoi(value);
            else if (option == "--sha256")
                spec.expectedSha256 = value;
            else if (option == "--proxy")
                spec.proxy = value;
            else if (option == "--ftp-user")
                spec.ftpUsername = value;
            else if (option == "--ftp-password")
                spec.ftpPassword = value;
            else if (option == "--ftp-ca")
                spec.ftpCaBundle = std::filesystem::path(std::u8string(value.begin(), value.end()));
            else
                throw std::invalid_argument("Unknown option.");
        }
    }
}
} // namespace
int runClient(int argc, char** argv) {
    if (argc < 4) {
        std::cout
            << "MaskPilot verification client\nUsage: maskpilot_cli URL DESTINATION DATA_DIRECTORY "
               "[connections] [sha256]\n       maskpilot_cli --resume TASK_ID DATA_DIRECTORY [options]\n"
               "Options: --connections N --sha256 HEX --proxy URL --ftp-user USER --ftp-password PASS\n"
               "         --ftp-explicit-tls --ftp-active --ftp-ca PEM_FILE\n";
        return argc == 1 ? 0 : 2;
    }
    try {
        auto data = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[3])));
        maskpilot::DownloadService service(maskpilot::makeSqliteRepository(data / "tasks.db"),
                                           maskpilot::makeCurlTransfer, data / "parts");
        std::string id;
        if (std::string(argv[1]) == "--resume" || std::string(argv[1]) == "--change-url") {
            id = argv[2];
            const auto tasks = service.snapshot();
            const auto existing =
                std::find_if(tasks.begin(), tasks.end(), [&](const auto& task) { return task.id == id; });
            if (existing == tasks.end())
                throw std::invalid_argument("Task was not found.");
            auto spec = existing->spec;
            int start = 4;
            if (std::string(argv[1]) == "--change-url") {
                if (argc < 5)
                    return 2;
                spec.url = argv[4];
                start = 5;
                spec.cookie.clear();
                spec.authorization.clear();
                spec.headers.clear();
                spec.referrer.clear();
                spec.ftpUsername.clear();
                spec.ftpPassword.clear();
                if (!maskpilot::isFtpUrl(spec.url)) {
                    spec.ftpExplicitTls = false;
                    spec.ftpActiveMode = false;
                    spec.ftpCaBundle.clear();
                }
            }
            readOptions(spec, argc, argv, start);
            if (const auto error = maskpilot::validateSpec(spec); !error.empty())
                throw std::invalid_argument(error);
            service.updateRequest(id, std::move(spec));
            service.resume(id);
        } else {
            maskpilot::DownloadSpec spec;
            spec.url = argv[1];
            spec.destination =
                std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[2])));
            int next = 4;
            if (next < argc && !std::string(argv[next]).starts_with("--")) {
                spec.connections = std::stoi(argv[next++]);
                for (auto* value : {&spec.expectedSha256, &spec.proxy, &spec.cookie, &spec.authorization}) {
                    if (next >= argc || std::string(argv[next]).starts_with("--"))
                        break;
                    *value = argv[next++];
                }
            }
            readOptions(spec, argc, argv, next);
            id = service.add(spec).get();
        }
        std::cout << "TASK_ID " << id << std::endl;
        for (int i = 0; i < 1800; ++i) {
            for (const auto& task : service.snapshot())
                if (task.id == id) {
                    if (task.state == maskpilot::TaskState::Completed) {
                        std::cout << "Completed " << task.downloaded << " bytes\n";
                        return 0;
                    }
                    if (task.state == maskpilot::TaskState::Failed ||
                        task.state == maskpilot::TaskState::NeedsAttention) {
                        std::cerr << task.error << '\n';
                        return 1;
                    }
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        service.pause(id);
        std::cerr << "Verification timeout\n";
        return 3;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wideArguments) {
    std::vector<std::string> arguments;
    for (int n = 0; n < argc; ++n) {
        const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[n], -1, nullptr,
                                               0, nullptr, nullptr);
        if (count <= 0)
            return 2;
        std::string value(static_cast<size_t>(count), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[n], -1, value.data(), count,
                                 nullptr, nullptr))
            return 2;
        value.pop_back();
        arguments.push_back(std::move(value));
    }
    std::vector<char*> pointers;
    for (auto& argument : arguments)
        pointers.push_back(argument.data());
    return runClient(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return runClient(argc, argv); }
#endif
