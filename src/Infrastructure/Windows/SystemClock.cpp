#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"

#include <chrono>

namespace ForgeConductor::Infrastructure::Windows {

Domain::UtcTimePoint SystemClock::utcNow() const noexcept
{
    return std::chrono::system_clock::now();
}

Domain::MonotonicTimePoint SystemClock::monotonicNow() const noexcept
{
    return std::chrono::steady_clock::now();
}

} // namespace ForgeConductor::Infrastructure::Windows
