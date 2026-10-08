#include "Infrastructure/TestSupport.h"
#include "NativeTools/Windows/ImageProviderCodec.h"
#include "ForgeConductor/Domain/ManagedRunModels.h"

#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace ForgeConductor::Tests;
namespace Domain = ForgeConductor::Domain;
namespace Codec = ForgeConductor::NativeTools::Windows::Detail;
using Microsoft::WRL::ComPtr;

// Independent PNG fixtures: straight RGBA, including RGB values beneath zero
// alpha. They were encoded from these literal pixels with standard PNG chunks
// and zlib, independently of the production WIC encoder.
constexpr std::array<BYTE, 24U> ReferenceRgba{
    17, 34, 51, 0, 203, 71, 19, 1, 29, 181, 73, 64,
    7, 83, 227, 128, 121, 9, 173, 254, 241, 157, 3, 255};
constexpr std::array<BYTE, 92U> ReferencePng{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x9d, 0x74, 0x66,
    0x1a, 0x00, 0x00, 0x00, 0x23, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x10, 0x54, 0x32, 0x66,
    0x38, 0xed, 0x2e, 0xcc, 0x28, 0xbb, 0xd5, 0xd3, 0x81, 0x81, 0x3d, 0xf8, 0x71, 0x43, 0x25, 0xe7,
    0xda, 0x7f, 0x1f, 0xe7, 0x32, 0xff, 0x07, 0x00, 0x5b, 0x28, 0x09, 0x62, 0x7f, 0x6d, 0xcf, 0x7f,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
constexpr std::array<BYTE, 91U> OversizedPng{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xaf, 0xcc, 0xb7,
    0x0b, 0x00, 0x00, 0x00, 0x22, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0xed, 0xc3, 0x01, 0x0d, 0x00,
    0x00, 0x08, 0x03, 0x20, 0x0b, 0x98, 0xc1, 0x08, 0xef, 0x9f, 0x4e, 0x7b, 0x38, 0xd8, 0xa8, 0x9e,
    0xac, 0xaa, 0xaa, 0xaa, 0xfa, 0xff, 0x01, 0x23, 0xee, 0x69, 0xa6, 0x28, 0x08, 0xb1, 0x49, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

void hr(const HRESULT value, const std::string_view message) { require(SUCCEEDED(value), message); }

[[nodiscard]] Codec::ImageProviderPixels referenceImage()
{
    return {3U, 2U, {reinterpret_cast<const std::byte*>(ReferenceRgba.data()),
        reinterpret_cast<const std::byte*>(ReferenceRgba.data() + ReferenceRgba.size())}};
}

struct ObservedPixels final {
    UINT width{};
    UINT height{};
    std::vector<BYTE> rgba;
};

[[nodiscard]] ObservedPixels independentDecode(std::span<const std::byte> encoded)
{
    std::vector<BYTE> buffer(encoded.size());
    std::transform(encoded.begin(), encoded.end(), buffer.begin(), [](std::byte value) { return std::to_integer<BYTE>(value); });
    ComPtr<IWICImagingFactory> factory;
    hr(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Independent WIC factory failed.");
    ComPtr<IWICStream> stream; hr(factory->CreateStream(&stream), "Independent WIC stream failed.");
    hr(stream->InitializeFromMemory(buffer.data(), static_cast<DWORD>(buffer.size())), "Independent PNG stream initialization failed.");
    ComPtr<IWICBitmapDecoder> decoder;
    hr(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder), "Emitted PNG did not independently decode.");
    GUID format{}; hr(decoder->GetContainerFormat(&format), "Emitted PNG format was unavailable.");
    require(format == GUID_ContainerFormatPng, "Emitted bytes are not a PNG container.");
    UINT frames{}; hr(decoder->GetFrameCount(&frames), "Emitted PNG frame count unavailable.");
    require(frames == 1U, "Emitted PNG contains an unexpected frame count.");
    ComPtr<IWICBitmapFrameDecode> frame; hr(decoder->GetFrame(0U, &frame), "Emitted PNG frame missing.");
    ObservedPixels result; hr(frame->GetSize(&result.width, &result.height), "Emitted PNG dimensions unavailable.");
    ComPtr<IWICFormatConverter> converter; hr(factory->CreateFormatConverter(&converter), "Independent RGBA converter failed.");
    hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
        "Emitted PNG could not independently convert to RGBA.");
    result.rgba.resize(static_cast<std::size_t>(result.width) * result.height * 4U);
    hr(converter->CopyPixels(nullptr, result.width * 4U, static_cast<UINT>(result.rgba.size()), result.rgba.data()),
        "Independent emitted PNG pixel copy was incomplete.");
    return result;
}

[[nodiscard]] std::vector<std::byte> independentBase64(const std::string& encoded)
{
    DWORD count{};
    require(::CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64,
        nullptr, &count, nullptr, nullptr) != FALSE, "Preview is invalid base64.");
    std::vector<std::byte> result(count);
    require(::CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64,
        reinterpret_cast<BYTE*>(result.data()), &count, nullptr, nullptr) != FALSE && count == result.size(),
        "Preview base64 did not independently decode completely.");
    return result;
}

[[nodiscard]] std::string independentSha256(std::span<const std::byte> bytes)
{
    struct Hash final {
        HCRYPTPROV provider{}; HCRYPTHASH value{};
        ~Hash() { if (value) ::CryptDestroyHash(value); if (provider) ::CryptReleaseContext(provider, 0U); }
    } hash;
    require(::CryptAcquireContextW(&hash.provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE,
        "Independent CryptoAPI provider failed.");
    require(::CryptCreateHash(hash.provider, CALG_SHA_256, 0U, 0U, &hash.value) != FALSE, "Independent SHA-256 creation failed.");
    require(::CryptHashData(hash.value, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0U) != FALSE,
        "Independent SHA-256 could not read emitted bytes.");
    std::array<BYTE, 32U> value{}; DWORD count = static_cast<DWORD>(value.size());
    require(::CryptGetHashParam(hash.value, HP_HASHVAL, value.data(), &count, 0U) != FALSE && count == value.size(),
        "Independent SHA-256 digest was incomplete.");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : value) { result += hex[byte >> 4U]; result += hex[byte & 15U]; }
    return result;
}

[[nodiscard]] bool exactPixels(const ObservedPixels& actual, const Codec::ImageProviderPixels& expected)
{
    return actual.width == expected.width && actual.height == expected.height && actual.rgba.size() == expected.rgba.size() &&
        std::equal(actual.rgba.begin(), actual.rgba.end(), expected.rgba.begin(),
            [](BYTE a, std::byte b) { return a == std::to_integer<BYTE>(b); });
}

void independentFixtureDecodeAndAlphaRoundTrip()
{
    const auto expected = referenceImage();
    auto decoded = take(Codec::decodeProviderImage(std::as_bytes(std::span{ReferencePng}), TestContext{}.active()));
    require(decoded.width == expected.width && decoded.height == expected.height && decoded.rgba == expected.rgba,
        "Independent RGBA fixture decode changed channels, alpha, dimensions or RGB beneath zero alpha.");
    const auto encoded = take(Codec::encodeProviderImage(expected, TestContext{}.active()));
    require(exactPixels(independentDecode(encoded), expected), "Native PNG encode changed straight RGBA pixels or lost alpha.");
    const auto roundTrip = take(Codec::decodeProviderImage(encoded, TestContext{}.active()));
    require(roundTrip.width == expected.width && roundTrip.height == expected.height && roundTrip.rgba == expected.rgba,
        "Native RGBA encode/decode round trip changed exact pixels.");
}

void smallPreviewPreservesPixelsAndActualHash()
{
    const auto image = referenceImage();
    for (const auto dimension : {256U, 1024U, 2048U}) {
        const auto preview = take(Codec::previewProviderImage(image, dimension, TestContext{}.active()));
        require(preview.width == image.width && preview.height == image.height && !preview.reducedForBytes,
            "Small preview was enlarged or inaccurately reported byte reduction.");
        require(preview.base64.size() <= 512U * 1024U, "Small preview exceeded the encoded transport bound.");
        const auto bytes = independentBase64(preview.base64);
        require(exactPixels(independentDecode(bytes), image), "Small preview changed measured RGBA pixels.");
        require(preview.sha256 == independentSha256(bytes), "Preview hash does not identify the emitted PNG bytes.");
    }
}

void noisyPreviewAdaptsToEncodedBoundAndReportsActualDimensions()
{
    Codec::ImageProviderPixels image{1024U, 768U, std::vector<std::byte>(1024U * 768U * 4U)};
    std::uint32_t state = 0x82a7c4e9U;
    for (auto& value : image.rgba) {
        state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
        value = static_cast<std::byte>(state & 0xffU);
    }
    const auto before = image.rgba;
    const auto full = take(Codec::encodeProviderImage(image, TestContext{}.active()));
    require((full.size() + 2U) / 3U * 4U > 512U * 1024U, "Noisy fixture does not force adaptive preview reduction.");
    require(Domain::MaximumManagedImagePreviewBase64Bytes == 512U * 1024U, "The reviewed 512 KiB preview contract changed.");
    const auto ordinary = take(Codec::previewProviderImage(image, 256U, TestContext{}.active()));
    require(ordinary.width == 256U && ordinary.height == 192U && !ordinary.reducedForBytes,
        "Default 256-pixel preview did not preserve the source aspect ratio or fit its byte budget.");
    const auto detail = take(Codec::previewProviderImage(image, 1024U, TestContext{}.active()));
    require(detail.reducedForBytes && detail.width > 0U && detail.height > 0U && detail.width < 1024U && detail.height < 768U,
        "Requested 1024-pixel noisy preview did not report its actual adaptive reduction.");
    for (const auto* preview : {&ordinary, &detail}) {
        require(!preview->base64.empty() && preview->base64.size() <= 512U * 1024U, "Adaptive preview exceeded the encoded byte bound.");
        const auto bytes = independentBase64(preview->base64);
        const auto observed = independentDecode(bytes);
        require(observed.width == preview->width && observed.height == preview->height,
            "Preview receipt dimensions differ from the emitted PNG.");
        const auto x = static_cast<std::int64_t>(observed.width) * 3;
        const auto y = static_cast<std::int64_t>(observed.height) * 4;
        require(x - y >= -4 && x - y <= 4, "Adaptive preview distorted source aspect ratio beyond integer rounding.");
        require(preview->sha256 == independentSha256(bytes), "Adaptive preview SHA-256 does not identify its emitted PNG.");
    }
    require(image.rgba == before, "Preview generation altered the original RGBA frame.");
}

void maskRedWeightsPreserveOutsideRgbaAndBlendAllChannels()
{
    const auto frame = [](const std::initializer_list<unsigned> values) {
        Codec::ImageProviderPixels result{3U, 1U, {}};
        for (const auto value : values) result.rgba.push_back(static_cast<std::byte>(value));
        return result;
    };
    const auto original = frame({11, 22, 33, 0, 21, 42, 63, 64, 13, 31, 47, 127});
    const auto mask = frame({0, 255, 128, 255, 255, 0, 0, 0, 128, 1, 249, 255});
    auto generated = frame({210, 130, 50, 255, 91, 81, 71, 9, 201, 177, 149, 255});
    const auto sourceBefore = original.rgba, maskBefore = mask.rgba;
    const auto expected = frame({11, 22, 33, 0, 91, 81, 71, 9, 107, 104, 98, 191});
    take(Codec::compositeProviderMask(generated, original, mask, TestContext{}.active()));
    require(generated.rgba == expected.rgba,
        "Mask red=0/255/128 did not preserve outside RGBA, keep generated pixels or produce the exact independently calculated blend.");
    require(original.rgba == sourceBefore && mask.rgba == maskBefore, "Compositing mutated source or mask pixels.");
    const auto encoded = take(Codec::encodeProviderImage(generated, TestContext{}.active()));
    require(exactPixels(independentDecode(encoded), expected), "Final PNG encoding lost exact outside-mask RGBA or blended alpha.");
}

void invalidPngDimensionsAndMasksFailBeforeMutation()
{
    const std::array<std::byte, 4U> invalid{std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}};
    requireError(Codec::decodeProviderImage(invalid, TestContext{}.active()), Domain::ErrorCodes::InvalidRequest, "Malformed PNG was accepted.");
    requireError(Codec::decodeProviderImage({}, TestContext{}.active()), Domain::ErrorCodes::PayloadTooLarge, "Empty image input was accepted.");
    requireError(Codec::decodeProviderImage(std::as_bytes(std::span{OversizedPng}), TestContext{}.active()),
        Domain::ErrorCodes::PayloadTooLarge, "A valid 1025-pixel PNG exceeded the decode dimension boundary.");
    const std::vector<std::byte> oversized(16U * 1024U * 1024U + 1U);
    requireError(Codec::decodeProviderImage(oversized, TestContext{}.active()), Domain::ErrorCodes::PayloadTooLarge, "Oversized input bytes were accepted.");
    auto image = referenceImage();
    auto invalidFrame = image; invalidFrame.rgba.pop_back();
    requireError(Codec::encodeProviderImage(invalidFrame, TestContext{}.active()), Domain::ErrorCodes::InvalidRequest, "An incomplete RGBA frame was encoded.");
    invalidFrame = image; invalidFrame.width = 0U;
    requireError(Codec::previewProviderImage(invalidFrame, 256U, TestContext{}.active()), Domain::ErrorCodes::InvalidRequest, "A zero-width preview frame was accepted.");
    for (const auto dimension : {127U, 2049U})
        requireError(Codec::previewProviderImage(image, dimension, TestContext{}.active()), Domain::ErrorCodes::InvalidRequest, "An invalid preview dimension was accepted.");
    const auto before = image.rgba;
    auto wrongShape = image; wrongShape.width = 2U; wrongShape.height = 3U;
    requireError(Codec::compositeProviderMask(image, wrongShape, referenceImage(), TestContext{}.active()),
        Domain::ErrorCodes::InvalidRequest, "Mismatched source dimensions were composited.");
    require(image.rgba == before, "Dimension rejection altered the generated image.");
    requireError(Codec::compositeProviderMask(image, referenceImage(), wrongShape, TestContext{}.active()),
        Domain::ErrorCodes::InvalidRequest, "Mismatched mask dimensions were composited.");
    require(image.rgba == before, "Mask dimension rejection altered the generated image.");
}

void cancelledAndExpiredCodecOperationsReturnNoArtifactOrMutation()
{
    const auto original = referenceImage(), mask = referenceImage();
    auto generated = referenceImage(); const auto before = generated.rgba;
    TestContext cancelled; cancelled.cancellation.request_stop();
    TestContext expired;
    for (const auto& [context, code] : std::vector<std::pair<Domain::OperationContext, std::string_view>>{
        {cancelled.active(), Domain::ErrorCodes::Cancelled}, {expired.expired(), Domain::ErrorCodes::DeadlineExceeded}}) {
        requireError(Codec::decodeProviderImage(std::as_bytes(std::span{ReferencePng}), context), code, "Stopped decode returned image bytes.");
        requireError(Codec::encodeProviderImage(generated, context), code, "Stopped encode returned a PNG artifact.");
        requireError(Codec::previewProviderImage(generated, 256U, context), code, "Stopped preview returned an image artifact.");
        requireError(Codec::compositeProviderMask(generated, original, mask, context), code, "Stopped mask operation succeeded.");
        require(generated.rgba == before, "Stopped compositing altered the generated RGBA frame.");
    }
}
} // namespace

int main()
{
    const auto apartment = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) { std::cerr << "FAIL codec test COM initialization\n"; return 1; }
    TestRegistry tests;
    addTest(tests, "image_codec.independent_rgba_alpha_round_trip", independentFixtureDecodeAndAlphaRoundTrip);
    addTest(tests, "image_codec.small_preview_pixels_hash", smallPreviewPreservesPixelsAndActualHash);
    addTest(tests, "image_codec.noisy_adaptive_preview_bound", noisyPreviewAdaptsToEncodedBoundAndReportsActualDimensions);
    addTest(tests, "image_codec.mask_rgba_red_weights", maskRedWeightsPreserveOutsideRgbaAndBlendAllChannels);
    addTest(tests, "image_codec.invalid_boundaries_no_mutation", invalidPngDimensionsAndMasksFailBeforeMutation);
    addTest(tests, "image_codec.cancelled_expired_no_artifact", cancelledAndExpiredCodecOperationsReturnNoArtifactOrMutation);
    std::size_t passed{};
    for (const auto& [name, test] : tests) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    ::CoUninitialize();
    std::cout << "SUMMARY passed=" << passed << " failed=" << tests.size() - passed << '\n';
    return passed == tests.size() ? 0 : 1;
}
