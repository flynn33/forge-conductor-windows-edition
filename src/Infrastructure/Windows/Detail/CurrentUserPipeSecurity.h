#pragma once

#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"

#include <Windows.h>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class CurrentUserPipeSecurity final {
public:
    static constexpr DWORD GrantedAccess =
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_CREATE_PIPE_INSTANCE;

    [[nodiscard]] static Domain::Result<std::unique_ptr<CurrentUserPipeSecurity>>
    create(const WindowsCurrentUserIdentity& identity) noexcept;

    ~CurrentUserPipeSecurity() noexcept = default;
    CurrentUserPipeSecurity(const CurrentUserPipeSecurity&) = delete;
    CurrentUserPipeSecurity& operator=(const CurrentUserPipeSecurity&) = delete;
    CurrentUserPipeSecurity(CurrentUserPipeSecurity&&) = delete;
    CurrentUserPipeSecurity& operator=(CurrentUserPipeSecurity&&) = delete;

    [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept
    {
        return &attributes_;
    }

    [[nodiscard]] const SECURITY_ATTRIBUTES* attributes() const noexcept
    {
        return &attributes_;
    }

private:
    CurrentUserPipeSecurity() noexcept = default;

    std::vector<std::byte> aclStorage_;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
};

[[nodiscard]] Domain::Result<void> verifyNamedPipeClientSid(
    HANDLE pipe,
    std::span<const std::byte> expectedSid) noexcept;

} // namespace ForgeConductor::Infrastructure::Windows::Detail
