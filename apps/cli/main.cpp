#include <chrono>
#include <iostream>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

int runClient(int argc, char** argv) {
    if (argc < 4) {
        std::cout << "MaskPilot verification client\nUsage: maskpilot_cli URL DESTINATION DATA_DIRECTORY "
                     "[connections] [sha256]\n       maskpilot_cli --resume TASK_ID DATA_DIRECTORY\n";
        return argc == 1 ? 0 : 2;
    }
    try {
        auto data = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[3])));
        maskpilot::DownloadService service(maskpilot::makeSqliteRepository(data / "tasks.db"),
                                           maskpilot::makeCurlTransfer, data / "parts");
        std::string id;
        if (std::string(argv[1]) == "--resume" || std::string(argv[1]) == "--change-url") {
            id = argv[2];
            if (std::string(argv[1]) == "--change-url") {
                if (argc < 5)
                    return 2;
                service.updateUrl(id, argv[4]);
            }
            service.resume(id);
        } else {
            maskpilot::DownloadSpec spec;
            spec.url = argv[1];
            spec.destination =
                std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[2])));
            if (argc > 4)
                spec.connections = std::stoi(argv[4]);
            if (argc > 5)
                spec.expectedSha256 = argv[5];
            if (argc > 6)
                spec.proxy = argv[6];
            if (argc > 7)
                spec.cookie = argv[7];
            if (argc > 8)
                spec.authorization = argv[8];
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
