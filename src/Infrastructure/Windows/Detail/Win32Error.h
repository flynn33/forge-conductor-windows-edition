#pragma once

#include "ForgeConductor/Domain/Error.h"

#include <Windows.h>
#include <bcrypt.h>

#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

[[nodiscard]] Domain::Error makeWin32Error(
    std::string_view action,
    DWORD nativeCode,
    std::string_view stableCode = Domain::ErrorCodes::InternalFailure,
    bool retryable = false) noexcept;

[[nodiscard]] Domain::Error makeNtStatusError(
    std::string_view action,
    NTSTATUS nativeStatus,
    std::string_view stableCode = Domain::ErrorCodes::InternalFailure,
    bool retryable = false) noexcept;

} // namespace ForgeConductor::Infrastructure::Windows::Detail
