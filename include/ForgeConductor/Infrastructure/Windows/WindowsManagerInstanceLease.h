#pragma once

#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {

struct WindowsManagerInstanceLeaseOptions final {
    // A purpose suffix is intended for isolated process/integration tests. The
    // production name is selected by leaving it empty. A dot separator is added
    // by the implementation and is not part of this value.
    std::string purposeSuffix;
};

class WindowsManagerInstanceLease final {
public:
    static constexpr std::size_t MaximumPurposeSuffixCharacters = 48U;
    static constexpr std::size_t MaximumMutexNameCharacters = 160U;
    static constexpr std::size_t MaximumPipeNameCharacters = 176U;

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
