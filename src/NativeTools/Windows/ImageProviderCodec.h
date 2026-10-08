#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ForgeConductor::NativeTools::Windows::Detail {

struct ImageProviderPixels final {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba;
};
struct ImageProviderPreview final {
    std::string base64;
    std::string sha256;
    std::uint32_t width{};
    std::uint32_t height{};
    bool reducedForBytes{};
};

[[nodiscard]] Domain::Result<ImageProviderPixels> decodeProviderImage(
    std::span<const std::byte> bytes, const Domain::OperationContext& context);
[[nodiscard]] Domain::Result<std::vector<std::byte>> encodeProviderImage(
    const ImageProviderPixels& image, const Domain::OperationContext& context);
[[nodiscard]] Domain::Result<ImageProviderPreview> previewProviderImage(
    const ImageProviderPixels& image, std::uint32_t maximumDimension,
    const Domain::OperationContext& context);
[[nodiscard]] Domain::Result<void> compositeProviderMask(ImageProviderPixels& generated,
    const ImageProviderPixels& original, const ImageProviderPixels& mask,
    const Domain::OperationContext& context);

} // namespace ForgeConductor::NativeTools::Windows::Detail
