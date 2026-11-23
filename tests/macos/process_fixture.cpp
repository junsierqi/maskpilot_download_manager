#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    const std::string nonce(argv[2]);
    if (nonce.size() != 32 || nonce.find_first_not_of("0123456789abcdef") != std::string::npos)
        return 2;
    std::string input;
    char buffer[64];
    ssize_t count;
    while ((count = read(STDIN_FILENO, buffer, sizeof(buffer))) != 0) {
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return 3;
        }
        input.append(buffer, static_cast<std::size_t>(count));
        if (input.size() > 64)
            return 3;
    }
    if (input != "owned-private-input\n")
        return 4;
    const auto child = fork();
    if (child < 0)
        return 5;
    if (child > 0) {
        const auto json = "{\"parent\":" + std::to_string(getpid()) + ",\"child\":" + std::to_string(child) +
                          ",\"nonce\":\"" + nonce + "\",\"inputClosed\":true}";
        const int file = open(argv[1], O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (file < 0) {
            kill(child, SIGKILL);
            return 6;
        }
        const auto written = write(file, json.data(), json.size());
        close(file);
        if (written != static_cast<ssize_t>(json.size())) {
            kill(child, SIGKILL);
            return 6;
        }
    }
    // Bounded even if the qualification owner itself fails before cleanup.
    sleep(180);
    return 0;
}
