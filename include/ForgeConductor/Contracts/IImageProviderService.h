#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Domain/ImageJobModels.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Contracts {

struct ImageProviderHttpResponse final {
    std::uint32_t status{};
    std::string contentType;
    std::vector<std::byte> body;
};

class IImageProviderHttpTransport {
public:
    virtual ~IImageProviderHttpTransport() = default;
    [[nodiscard]] virtual Domain::Result<ImageProviderHttpResponse> request(
        const Domain::ImageProviderConfig& provider, std::string_view method,
        std::string_view route, std::string_view contentType, std::span<const std::byte> body,
        std::size_t maximumResponseBytes, const Domain::OperationContext& context) noexcept = 0;
};

class IImageProviderService {
public:
    virtual ~IImageProviderService() = default;
    [[nodiscard]] virtual Domain::Result<std::string> execute(
        std::string_view name, std::string_view arguments, const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
