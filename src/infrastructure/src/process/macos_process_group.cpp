#include "macos_process_group.h"
#include <QTemporaryDir>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace maskpilot::process {
MacProcessGroup::MacProcessGroup() {
    std::size_t length = sizeof(descriptorLimit_);
    if (sysctlbyname("kern.maxfilesperproc", &descriptorLimit_, &length, nullptr, 0) != 0 ||
        length != sizeof(descriptorLimit_) || descriptorLimit_ < 4 || descriptorLimit_ > 1048576)
        throw std::runtime_error("Cannot establish a bounded private helper descriptor policy.");
    // open(O_CLOEXEC) is atomic on Darwin; pipe()+fcntl() would leak writers across concurrent exec.
    QTemporaryDir directory;
    if (!directory.isValid())
        throw std::runtime_error("Cannot create private helper lifetime capability.");
    const auto name = directory.filePath("lifetime").toUtf8();
    try {
        if (mkfifo(name.constData(), 0600) != 0)
            throw std::runtime_error("Cannot create private helper lifetime capability.");
        reader_ = open(name.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (reader_ < 0)
            throw std::runtime_error("Cannot open private helper lifetime capability.");
        writer_ = open(name.constData(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (writer_ < 0 || reader_ >= descriptorLimit_ || writer_ >= descriptorLimit_ ||
            fcntl(reader_, F_SETFL, 0) != 0 || unlink(name.constData()) != 0)
            throw std::runtime_error("Cannot arm private helper lifetime capability.");
    } catch (...) {
        if (reader_ >= 0)
            close(reader_);
        if (writer_ >= 0)
            close(writer_);
        reader_ = writer_ = -1;
        throw;
    }
}
void MacProcessGroup::releaseWriter() noexcept {
    if (writer_ >= 0) {
        close(writer_);
        writer_ = -1;
    }
}
MacProcessGroup::~MacProcessGroup() {
    QObject::disconnect(finished_);
    releaseWriter();
    if (reader_ >= 0)
        close(reader_);
}
void MacProcessGroup::prepare(QProcess& process) {
    if (prepared_ || writer_ < 0)
        throw std::logic_error("Private helper capability is already in use.");
    prepared_ = true;
    QProcess::UnixProcessParameters parameters;
    parameters.flags = QProcess::UnixProcessFlag::CloseFileDescriptors;
    process.setUnixProcessParameters(parameters);
    const int reader = reader_, writer = writer_, limit = descriptorLimit_;
    process.setChildProcessModifier([&process, reader, writer, limit] {
        if (setsid() < 0)
            process.failChildProcessModifier("Cannot isolate private helper session", errno);
        close(writer);
        const auto launcher = fork();
        if (launcher < 0)
            process.failChildProcessModifier("Cannot create private lifetime guardian", errno);
        if (launcher == 0) {
            const auto guardian = fork();
            if (guardian < 0)
                _exit(132);
            if (guardian > 0)
                _exit(0);
            // Keep our own group membership until termination. It pins the group identity.
            // In particular close Qt's startup pipe, all input channels and other lifetime writers.
            for (int descriptor = 0; descriptor < limit; ++descriptor)
                if (descriptor != reader)
                    close(descriptor);
            char ignored;
            ssize_t received;
            do {
                received = read(reader, &ignored, 1);
            } while (received < 0 && errno == EINTR);
            kill(-getpgrp(), SIGKILL);
            _exit(130);
        }
        close(reader);
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(launcher, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            process.failChildProcessModifier("Cannot arm private lifetime guardian", ECHILD);
    });
    finished_ = QObject::connect(&process, &QProcess::finished, &process, [this] { releaseWriter(); });
}
void MacProcessGroup::stop(QProcess& process) {
    releaseWriter();
    if (process.state() != QProcess::NotRunning && !process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(5000);
    }
}
} // namespace maskpilot::process
