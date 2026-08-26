#pragma once

#include "ForgeConductor/Domain/Result.h"
#include "UniqueHandle.h"

#include <Windows.h>

#include <memory>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class JobObject final {
public:
    [[nodiscard]] static Domain::Result<std::shared_ptr<JobObject>> create();

    ~JobObject() = default;
    JobObject(const JobObject&) = delete;
    JobObject& operator=(const JobObject&) = delete;
    JobObject(JobObject&&) = delete;
    JobObject& operator=(JobObject&&) = delete;

    [[nodiscard]] Domain::Result<void> assign(HANDLE process) const;
    void terminate(UINT exitCode) const noexcept;
    [[nodiscard]] HANDLE nativeHandle() const noexcept
    {
        return handle_.get();
    }

private:
    explicit JobObject(UniqueHandle handle) noexcept
        : handle_{std::move(handle)}
    {
    }

    UniqueHandle handle_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
