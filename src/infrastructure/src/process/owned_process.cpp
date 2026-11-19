#include "owned_process.h"
#include <array>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
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
#else
        (void)process;
#endif
    }
    void stop(QProcess& process) {
#ifdef _WIN32
        TerminateJobObject(job_.value, 130);
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
