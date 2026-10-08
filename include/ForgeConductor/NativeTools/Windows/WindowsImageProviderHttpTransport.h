#pragma once
#include "ForgeConductor/Contracts/IImageProviderService.h"

namespace ForgeConductor::NativeTools::Windows {
class WindowsImageProviderHttpTransport final : public Contracts::IImageProviderHttpTransport {
public:
    [[nodiscard]] Domain::Result<Contracts::ImageProviderHttpResponse> request(
        const Domain::ImageProviderConfig& provider, std::string_view method,
        std::string_view route, std::string_view contentType, std::span<const std::byte> body,
        std::size_t maximumResponseBytes, const Domain::OperationContext& context) noexcept override;
};
} // namespace ForgeConductor::NativeTools::Windows
