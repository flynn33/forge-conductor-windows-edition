#pragma once

#include "ForgeConductor/Domain/DiagnosticsModels.h"

#include <Windows.h>
#include <evntrace.h>
#include <evntprov.h>

#include <atomic>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class EtwProvider final {
public:
    EtwProvider() noexcept;
    ~EtwProvider() noexcept;

    EtwProvider(const EtwProvider&) = delete;
    EtwProvider& operator=(const EtwProvider&) = delete;
    EtwProvider(EtwProvider&&) = delete;
    EtwProvider& operator=(EtwProvider&&) = delete;

    [[nodiscard]] bool healthy() const noexcept;

    // Only fixed numeric descriptors cross the ETW boundary. Arbitrary diagnostic
    // strings remain in the already-redacted bounded file channel.
    [[nodiscard]] bool write(
        const Domain::DiagnosticEnvelope& envelope) noexcept;

    void shutdown() noexcept;

private:
    std::atomic<REGHANDLE> registration_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
