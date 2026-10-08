#include "ImageProviderCodec.h"

#include "ForgeConductor/Domain/ManagedRunModels.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"

#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <span>

namespace ForgeConductor::NativeTools::Windows::Detail {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::size_t MaximumBytes = 16U * 1024U * 1024U;
struct Failure final { Domain::Error error; };
void check(const Domain::OperationContext& context) {
    auto valid = Infrastructure::Windows::Detail::validateOperationContext(context,
        std::chrono::steady_clock::now(), "image provider codec");
    if (!valid) throw Failure{valid.error()};
}
void hr(HRESULT result, const char* action) {
    if (FAILED(result)) throw Failure{Domain::makeError(Domain::ErrorCodes::InvalidRequest, action)};
}
class Apartment final {
public:
    Apartment() { const auto value = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (value != RPC_E_CHANGED_MODE) hr(value, "Initialize image codec COM."); initialized_ = SUCCEEDED(value); }
    ~Apartment() { if (initialized_) ::CoUninitialize(); }
private: bool initialized_{};
};
ComPtr<IWICImagingFactory> factory() {
    ComPtr<IWICImagingFactory> result;
    hr(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&result)), "Create image codec.");
    return result;
}
void validate(const ImageProviderPixels& image) {
    if (!image.width || !image.height || image.width > 1024U || image.height > 1024U ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4U)
        throw Failure{Domain::makeError(Domain::ErrorCodes::InvalidRequest,
            "Provider image must be a bounded RGBA8 frame, at most 1024 by 1024.")};
}
std::string base64(std::span<const std::byte> bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2U) / 3U * 4U);
    for (std::size_t i = 0; i < bytes.size(); i += 3U) {
        const auto a = std::to_integer<unsigned>(bytes[i]);
        const auto b = i + 1U < bytes.size() ? std::to_integer<unsigned>(bytes[i + 1U]) : 0U;
        const auto c = i + 2U < bytes.size() ? std::to_integer<unsigned>(bytes[i + 2U]) : 0U;
        out += alphabet[a >> 2U]; out += alphabet[((a & 3U) << 4U) | (b >> 4U)];
        out += i + 1U < bytes.size() ? alphabet[((b & 15U) << 2U) | (c >> 6U)] : '=';
        out += i + 2U < bytes.size() ? alphabet[c & 63U] : '=';
    }
    return out;
}
std::vector<std::byte> encode(const ImageProviderPixels& image, const Domain::OperationContext& context) {
    check(context); validate(image); Apartment apartment;
    auto codec = factory();
    ComPtr<IStream> stream; hr(::CreateStreamOnHGlobal(nullptr, TRUE, &stream), "Create PNG stream.");
    ComPtr<IWICBitmapEncoder> encoder;
    hr(codec->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder.");
    hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder.");
    ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> options;
    hr(encoder->CreateNewFrame(&frame, &options), "Create PNG frame.");
    hr(frame->Initialize(options.Get()), "Initialize PNG frame.");
    hr(frame->SetSize(image.width, image.height), "Set PNG dimensions.");
    auto format = GUID_WICPixelFormat32bppBGRA;
    hr(frame->SetPixelFormat(&format), "Set PNG alpha format.");
    if (format != GUID_WICPixelFormat32bppBGRA) throw Failure{Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable, "PNG encoder did not preserve alpha.")};
    auto bgra = image.rgba;
    for (std::size_t i = 0; i < bgra.size(); i += 4U) {
        if ((i & 65535U) == 0U) check(context);
        std::swap(bgra[i], bgra[i + 2U]);
    }
    hr(frame->WritePixels(image.height, image.width * 4U, static_cast<UINT>(bgra.size()),
        reinterpret_cast<BYTE*>(bgra.data())), "Write PNG pixels.");
    hr(frame->Commit(), "Commit PNG frame."); hr(encoder->Commit(), "Commit PNG.");
    STATSTG stat{}; hr(stream->Stat(&stat, STATFLAG_NONAME), "Read PNG length.");
    if (stat.cbSize.QuadPart > MaximumBytes) throw Failure{Domain::makeError(
        Domain::ErrorCodes::PayloadTooLarge, "Encoded PNG exceeds 16 MiB.")};
    LARGE_INTEGER start{}; hr(stream->Seek(start, STREAM_SEEK_SET, nullptr), "Rewind PNG.");
    std::vector<std::byte> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart)); ULONG read{};
    hr(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), "Read PNG.");
    if (read != bytes.size()) throw Failure{Domain::makeError(Domain::ErrorCodes::IntegrityFailure, "Incomplete PNG stream.")};
    check(context); return bytes;
}
}

Domain::Result<ImageProviderPixels> decodeProviderImage(std::span<const std::byte> bytes,
    const Domain::OperationContext& context) {
    try {
        check(context);
        if (bytes.empty() || bytes.size() > MaximumBytes) throw Failure{Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge, "Image input exceeds the 16 MiB bound or is empty.")};
        Apartment apartment; auto codec = factory();
        ComPtr<IWICStream> stream; hr(codec->CreateStream(&stream), "Create image stream.");
        hr(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),
            static_cast<DWORD>(bytes.size())), "Open image bytes.");
        ComPtr<IWICBitmapDecoder> decoder;
        hr(codec->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder), "Decode image.");
        UINT frames{}; hr(decoder->GetFrameCount(&frames), "Read image frame count.");
        if (frames != 1U) throw Failure{Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Provider input must contain one image frame.")};
        ComPtr<IWICBitmapFrameDecode> frame; hr(decoder->GetFrame(0U, &frame), "Read image frame.");
        ImageProviderPixels image; hr(frame->GetSize(&image.width, &image.height), "Read image dimensions.");
        if (!image.width || !image.height || image.width > 1024U || image.height > 1024U)
            throw Failure{Domain::makeError(Domain::ErrorCodes::PayloadTooLarge, "Image dimensions exceed 1024 by 1024.")};
        ComPtr<IWICFormatConverter> converted; hr(codec->CreateFormatConverter(&converted), "Create RGBA converter.");
        hr(converted->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom), "Convert image to RGBA8.");
        image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4U);
        for (UINT y = 0; y < image.height; y += 64U) {
            check(context); const auto rows = (std::min)(64U, image.height - y);
            WICRect rect{0, static_cast<INT>(y), static_cast<INT>(image.width), static_cast<INT>(rows)};
            hr(converted->CopyPixels(&rect, image.width * 4U, rows * image.width * 4U,
                reinterpret_cast<BYTE*>(image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4U)), "Read RGBA pixels.");
        }
        check(context); return Domain::Result<ImageProviderPixels>::success(std::move(image));
    } catch (const Failure& error) { return Domain::Result<ImageProviderPixels>::failure(error.error); }
    catch (...) { return Domain::Result<ImageProviderPixels>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Image decoding failed.")); }
}
Domain::Result<std::vector<std::byte>> encodeProviderImage(const ImageProviderPixels& image,
    const Domain::OperationContext& context) {
    try { return Domain::Result<std::vector<std::byte>>::success(encode(image, context)); }
    catch (const Failure& error) { return Domain::Result<std::vector<std::byte>>::failure(error.error); }
    catch (...) { return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "PNG encoding failed.")); }
}
Domain::Result<ImageProviderPreview> previewProviderImage(const ImageProviderPixels& image,
    std::uint32_t maximumDimension, const Domain::OperationContext& context) {
    try {
        check(context); validate(image);
        if (maximumDimension < 128U || maximumDimension > 2048U) throw Failure{Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "Preview dimension must be 128 through 2048.")};
        Apartment apartment; auto codec = factory();
        double scale = (std::min)(1.0, static_cast<double>(maximumDimension) / (std::max)(image.width, image.height));
        bool reduced{};
        for (;;) {
            check(context);
            ImageProviderPixels preview{(std::max)(1U, static_cast<UINT>(image.width * scale)),
                (std::max)(1U, static_cast<UINT>(image.height * scale)), {}};
            if (preview.width == image.width && preview.height == image.height) preview.rgba = image.rgba;
            else {
                ComPtr<IWICBitmap> bitmap;
                hr(codec->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppRGBA,
                    image.width * 4U, static_cast<UINT>(image.rgba.size()),
                    reinterpret_cast<BYTE*>(const_cast<std::byte*>(image.rgba.data())), &bitmap), "Create preview bitmap.");
                ComPtr<IWICBitmapScaler> scaler; hr(codec->CreateBitmapScaler(&scaler), "Create preview scaler.");
                hr(scaler->Initialize(bitmap.Get(), preview.width, preview.height, WICBitmapInterpolationModeFant), "Scale preview.");
                preview.rgba.resize(static_cast<std::size_t>(preview.width) * preview.height * 4U);
                hr(scaler->CopyPixels(nullptr, preview.width * 4U, static_cast<UINT>(preview.rgba.size()),
                    reinterpret_cast<BYTE*>(preview.rgba.data())), "Read preview pixels.");
            }
            auto png = encode(preview, context);
            if ((png.size() + 2U) / 3U * 4U <= Domain::MaximumManagedImagePreviewBase64Bytes) {
                Infrastructure::Windows::BCryptSha256Hasher hasher;
                auto hash = hasher.sha256(png); if (!hash) throw Failure{hash.error()};
                auto encoded = base64(png);
                check(context);
                return Domain::Result<ImageProviderPreview>::success({std::move(encoded), hash.value().value(),
                    preview.width, preview.height, reduced});
            }
            if (preview.width == 1U && preview.height == 1U) throw Failure{Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "Preview cannot fit the encoded transport bound.")};
            reduced = true; scale *= 0.75;
        }
    } catch (const Failure& error) { return Domain::Result<ImageProviderPreview>::failure(error.error); }
    catch (...) { return Domain::Result<ImageProviderPreview>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Preview encoding failed.")); }
}
Domain::Result<void> compositeProviderMask(ImageProviderPixels& generated, const ImageProviderPixels& original,
    const ImageProviderPixels& mask, const Domain::OperationContext& context) {
    try {
        check(context); validate(generated); validate(original); validate(mask);
        if (generated.width != original.width || generated.height != original.height ||
            mask.width != original.width || mask.height != original.height)
            throw Failure{Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Edit source, mask and generated dimensions must match.")};
        for (std::size_t i = 0; i < generated.rgba.size(); i += 4U) {
            if ((i & 65535U) == 0U) check(context);
            const auto weight = std::to_integer<unsigned>(mask.rgba[i]);
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                const auto a = std::to_integer<unsigned>(generated.rgba[i + channel]);
                const auto b = std::to_integer<unsigned>(original.rgba[i + channel]);
                generated.rgba[i + channel] = static_cast<std::byte>((a * weight + b * (255U - weight) + 127U) / 255U);
            }
        }
        check(context); return Domain::Result<void>::success();
    } catch (const Failure& error) { return Domain::Result<void>::failure(error.error); }
    catch (...) { return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Mask compositing failed.")); }
}
} // namespace ForgeConductor::NativeTools::Windows::Detail
