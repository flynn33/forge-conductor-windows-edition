#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {

// The host may supply its verified installed package AUMID. An empty ID uses
// the current process's ForgeConductor.Windows package family or verifies its
// executable's exact installed package directory. No registration or Windows
// notification settings are changed.
class WindowsScheduledTaskNotifier final {
public:
    static constexpr std::size_t MaximumEventBytes = 64U * 1024U;
    static constexpr std::size_t MaximumPayloadBytes = 4096U;
    explicit WindowsScheduledTaskNotifier(std::string installedApplicationId = {});

    [[nodiscard]] static Domain::Result<std::string> buildPayload(
        std::string_view scheduleEvent) noexcept;
    [[nodiscard]] static Domain::Result<std::string> applicationIdForInstalledExecutable(
        std::string_view executablePath) noexcept;
    [[nodiscard]] Domain::Result<std::string> resolveApplicationId(
        const Domain::OperationContext&) const noexcept;
    [[nodiscard]] Domain::Result<std::string> submit(
        std::string_view scheduleEvent, const Domain::OperationContext&) const noexcept;

private:
    std::string applicationId_;
};

} // namespace ForgeConductor::Infrastructure::Windows
