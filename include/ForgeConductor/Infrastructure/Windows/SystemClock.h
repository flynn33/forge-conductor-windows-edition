#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"

namespace ForgeConductor::Infrastructure::Windows {

class SystemClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override;
    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override;
};

} // namespace ForgeConductor::Infrastructure::Windows
