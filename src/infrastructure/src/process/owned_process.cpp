#include "owned_process.h"
#include <array>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace maskpilot::process {
struct OwnedProcessGroup::Impl {
#ifdef _WIN32
    struct Handle {
        HANDLE value = nullptr;
        ~Handle() {
            if (value)
                CloseHandle(value);
        }
    } job_;
    struct Attributes {
        std::vector<unsigned char> memory;
        LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
        ~Attributes() {
            if (value)
                DeleteProcThreadAttributeList(value);
        }
    } attributes_;
    STARTUPINFOEXW startup_{};
    std::array<HANDLE, 3> inherited_{};
#elif defined(__linux__)
    // PIDFD_SIGNAL_PROCESS_GROUP, available since Linux 6.9. Unlike a numeric
    // process-group ID this retained kernel reference cannot target a reused PID.
    static constexpr unsigned groupSignal = 1U << 2;
    int pidfd_ = -1;
    QMetaObject::Connection started_, finished_;
    void killGroup() noexcept {
        if (pidfd_ >= 0)
            syscall(SYS_pidfd_send_signal, pidfd_, SIGKILL, nullptr, groupSignal);
    }
#endif
  public:
    Impl() {
#ifdef _WIN32
        job_.value = CreateJobObjectW(nullptr, nullptr);
        if (!job_.value)
            throw std::runtime_error("Cannot create private process group.");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("Cannot isolate private process group.");
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        attributes_.memory.resize(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_.memory.data());
        if (!InitializeProcThreadAttributeList(list, 2, 0, &size))
            throw std::runtime_error("Cannot initialize private launch policy.");
        attributes_.value = list;
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_.value, sizeof(HANDLE),
                                       nullptr, nullptr))
            throw std::runtime_error("Cannot bind private process group.");
#elif defined(__linux__)
        const auto self = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
        if (self < 0)
            throw std::runtime_error("Private helpers require Linux pidfd support.");
        const auto result = syscall(SYS_pidfd_send_signal, self, 0, nullptr, groupSignal);
        const auto error = errno;
        close(self);
        if (result < 0 && error != ESRCH)
            throw std::runtime_error("Private helpers require Linux 6.9+ process-group signaling.");
#endif
    }
    ~Impl() {
#ifdef __linux__
        QObject::disconnect(started_);
        QObject::disconnect(finished_);
        killGroup();
        if (pidfd_ >= 0)
            close(pidfd_);
#endif
    }
    void prepare(QProcess& process) {
#ifdef _WIN32
        process.setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments* args) {
            startup_.StartupInfo = *reinterpret_cast<STARTUPINFOW*>(args->startupInfo);
            startup_.StartupInfo.cb = sizeof(startup_);
            startup_.lpAttributeList = attributes_.value;
            inherited_ = {startup_.StartupInfo.hStdInput, startup_.StartupInfo.hStdOutput,
                          startup_.StartupInfo.hStdError};
            if (!UpdateProcThreadAttribute(attributes_.value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           inherited_.data(), sizeof(HANDLE) * inherited_.size(), nullptr,
                                           nullptr)) {
                args->applicationName = L"";
                return;
            }
            args->startupInfo = reinterpret_cast<Q_STARTUPINFO*>(&startup_);
            args->flags |= EXTENDED_STARTUPINFO_PRESENT;
            args->inheritHandles = true;
        });
#elif defined(__linux__)
        if (pidfd_ >= 0)
            throw std::logic_error("Private process group is already in use.");
        QProcess::UnixProcessParameters parameters;
        parameters.flags = QProcess::UnixProcessFlag::CloseFileDescriptors;
        process.setUnixProcessParameters(parameters);
        const auto owner = getpid();
        process.setChildProcessModifier([&process, owner] {
            if (setsid() < 0)
                process.failChildProcessModifier("Cannot isolate private helper session", errno);
            if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0)
                process.failChildProcessModifier("Cannot bind private helper lifetime", errno);
            const auto ownerFd = static_cast<int>(syscall(SYS_pidfd_open, owner, 0));
            if (ownerFd < 0)
                process.failChildProcessModifier("Cannot retain private helper owner", errno);
            if (getppid() != owner)
                _exit(130);
            // The detached guardian remains in this private group, preventing
            // group-ID reuse. PDEATHSIG alone does not cover grandchildren.
            // Only async-signal-safe operations are allowed before exec here.
            const auto launcher = syscall(SYS_clone, SIGCHLD, nullptr, nullptr, nullptr, 0);
            if (launcher < 0)
                process.failChildProcessModifier("Cannot start private lifetime guardian", errno);
            if (launcher == 0) {
                const auto watcher = syscall(SYS_clone, SIGCHLD, nullptr, nullptr, nullptr, 0);
                if (watcher < 0)
                    _exit(131);
                if (watcher > 0)
                    _exit(0);
                // In particular, close Qt's startup pipe and all secret input
                // channels. Retain only the owner kernel reference.
                if ((ownerFd > 0 && syscall(SYS_close_range, 0U, unsigned(ownerFd - 1), 0U) != 0) ||
                    syscall(SYS_close_range, unsigned(ownerFd + 1), ~0U, 0U) != 0) {
                    kill(-getpgrp(), SIGKILL);
                    _exit(132);
                }
                pollfd event{ownerFd, POLLIN, 0};
                int result;
                do {
                    result = poll(&event, 1, -1);
                } while (result < 0 && errno == EINTR);
                kill(-getpgrp(), SIGKILL);
                _exit(130);
            }
            close(ownerFd);
            int status = 0;
            pid_t waited;
            do {
                waited = waitpid(static_cast<pid_t>(launcher), &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
                process.failChildProcessModifier("Cannot arm private lifetime guardian", ECHILD);
        });
        started_ = QObject::connect(&process, &QProcess::started, &process, [this, &process] {
            pidfd_ = static_cast<int>(syscall(SYS_pidfd_open, process.processId(), 0));
            if (pidfd_ < 0 || syscall(SYS_pidfd_send_signal, pidfd_, 0, nullptr, groupSignal) != 0)
                process.kill();
        });
        finished_ = QObject::connect(&process, &QProcess::finished, &process, [this] { killGroup(); });
#else
        (void)process;
#endif
    }
    void stop(QProcess& process) {
#ifdef _WIN32
        TerminateJobObject(job_.value, 130);
#elif defined(__linux__)
        killGroup();
        if (pidfd_ < 0 && process.state() != QProcess::NotRunning)
            process.kill();
#else
        process.kill();
#endif
        process.waitForFinished(5000);
    }
};

OwnedProcessGroup::OwnedProcessGroup() : impl_(std::make_unique<Impl>()) {}
OwnedProcessGroup::~OwnedProcessGroup() = default;
void OwnedProcessGroup::prepare(QProcess& process) { impl_->prepare(process); }
void OwnedProcessGroup::stop(QProcess& process) { impl_->stop(process); }
} // namespace maskpilot::process
