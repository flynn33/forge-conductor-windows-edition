#pragma once

#include "ForgeConductor/Contracts/IWebAccessService.h"
#include <cstddef>
#include <string>

namespace ForgeConductor::NativeTools::Windows {
class WindowsWebAccessService final : public Contracts::IWebAccessService {
public:
    static constexpr std::size_t MaximumBodyBytes = 48U * 1024U;
    static constexpr std::size_t DefaultBodyBytes = 32U * 1024U;
    static constexpr std::size_t MaximumRequestBodyBytes = 256U * 1024U;
    // The endpoint is injectable for deterministic HTTP fixtures; production
    // uses the public search page, with query supplied only as a URL parameter.
    explicit WindowsWebAccessService(
        std::string searchEndpoint = "https://lite.duckduckgo.com/lite/");
    [[nodiscard]] Domain::Result<std::string> execute(
        std::string_view toolName, std::string_view arguments,
        const Domain::OperationContext& context) noexcept override;
private:
    std::string searchEndpoint_;
};
} // namespace ForgeConductor::NativeTools::Windows
