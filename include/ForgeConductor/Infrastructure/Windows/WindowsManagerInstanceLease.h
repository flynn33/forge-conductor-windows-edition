#pragma once

#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {

struct WindowsManagerInstanceLeaseOptions final {
    // A purpose suffix is intended for isolated process/integration tests. The
    // production name is selected by leaving it empty. A dot separator is added
    // by the implementation and is not part of this value.
    std::string purposeSuffix;
};

class WindowsManagerInstanceNames final {
public:
    [[nodiscard]] std::wstring_view mutexName() const noexcept
    {
        return mutexName_;
    }

    [[nodiscard]] std::wstring_view pipeName() const noexcept
    {
        return pipeName_;
    }

private:
    friend class WindowsManagerInstanceLease;

    WindowsManagerInstanceNames(
        std::wstring mutexName,
        std::wstring pipeName) noexcept
        : mutexName_{std::move(mutexName)},
          pipeName_{std::move(pipeName)}
    {
    }

    std::wstring mutexName_;
    std::wstring pipeName_;
};

class WindowsManagerInstanceLease final {
public:
    static constexpr std::size_t MaximumPurposeSuffixCharacters = 48U;
    static constexpr std::size_t MaximumMutexNameCharacters = 160U;
    static constexpr std::size_t MaximumPipeNameCharacters = 176U;

    [[nodiscard]] static Domain::Result<WindowsManagerInstanceNames> namesFor(
        const WindowsCurrentUserIdentity& identity,
        const WindowsManagerInstanceLeaseOptions& options = {}) noexcept;

    [[nodiscard]] static Domain::Result<WindowsManagerInstanceLease> acquire(
        const WindowsCurrentUserIdentity& identity,
        const WindowsManagerInstanceLeaseOptions& options = {}) noexcept;

    ~WindowsManagerInstanceLease() noexcept;

    WindowsManagerInstanceLease(const WindowsManagerInstanceLease&) = delete;
    WindowsManagerInstanceLease& operator=(const WindowsManagerInstanceLease&) = delete;
    WindowsManagerInstanceLease(WindowsManagerInstanceLease&& other) noexcept;
    WindowsManagerInstanceLease& operator=(WindowsManagerInstanceLease&& other) noexcept;

    [[nodiscard]] bool owns() const noexcept;
    [[nodiscard]] std::wstring_view mutexName() const noexcept;
    [[nodiscard]] std::wstring_view pipeName() const noexcept;

private:
    class Impl;

    explicit WindowsManagerInstanceLease(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
