#pragma once

#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/OperationContext.h"

#include <string_view>

namespace ForgeConductor::Persistence::Windows::Detail {

enum class WinsqliteInterruptionReason {
    None,
    Cancelled,
    DeadlineExceeded,
    BusyTimeout
};

[[nodiscard]] Domain::Error makeWinsqliteError(
    int nativeCode,
    std::string_view action,
    const char* nativeMessage,
    WinsqliteInterruptionReason interruptionReason,
    const Domain::OperationContext* context) noexcept;

} // namespace ForgeConductor::Persistence::Windows::Detail
