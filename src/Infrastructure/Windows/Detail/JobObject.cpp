#include "JobObject.h"

#include "ForgeConductor/Domain/Error.h"

#include <string>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error win32Failure(
    const std::string_view action,
    const DWORD error)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        std::string{action} + " failed with Win32 error " +
            std::to_string(error) + ".");
}

} // namespace

Domain::Result<std::shared_ptr<JobObject>> JobObject::create()
{
    UniqueHandle handle{::CreateJobObjectW(nullptr, nullptr)};
    if (!handle) {
        return Domain::Result<std::shared_ptr<JobObject>>::failure(
            win32Failure("CreateJobObjectW", ::GetLastError()));
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(
            handle.get(),
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits))) {
        return Domain::Result<std::shared_ptr<JobObject>>::failure(
            win32Failure("SetInformationJobObject", ::GetLastError()));
    }

    return Domain::Result<std::shared_ptr<JobObject>>::success(
        std::shared_ptr<JobObject>{new JobObject{std::move(handle)}});
}

Domain::Result<void> JobObject::assign(const HANDLE process) const
{
    if (!::AssignProcessToJobObject(handle_.get(), process)) {
        return Domain::Result<void>::failure(
            win32Failure("AssignProcessToJobObject", ::GetLastError()));
    }
    return Domain::Result<void>::success();
}

void JobObject::terminate(const UINT exitCode) const noexcept
{
    static_cast<void>(::TerminateJobObject(handle_.get(), exitCode));
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
