#include "Infrastructure/TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/NativeTools/Windows/WindowsDesktopArtifactService.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"

#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {
using Json = nlohmann::json;
using Service = NativeTools::Windows::WindowsDesktopArtifactService;
using Infrastructure::Windows::WindowsAtomicFileStore;
using Infrastructure::Windows::WindowsWorkspaceAuthority;
using Infrastructure::Windows::WindowsWorkspaceAuthorityPolicy;
using Microsoft::WRL::ComPtr;
namespace Detail = Infrastructure::Windows::Detail;
using namespace std::chrono_literals;

Domain::PathText pathText(const std::filesystem::path& value) {
    return take(Domain::PathText::create(take(Detail::strictUtf16ToUtf8(value.native()))));
}
Domain::OperationContext context() { return TestContext{}.active(); }
void hr(HRESULT result, std::string_view message) { require(SUCCEEDED(result), message); }
class Fixture final {
public:
    Fixture() {
        static unsigned sequence{};
        std::wstring temporary(32768U, L'\0');
        const auto count = ::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        require(count != 0U && count < temporary.size(), "GetTempPathW failed.");
        temporary.resize(count);
        root = std::filesystem::path{temporary} / ("ForgeConductor.DesktopArtifact." + std::to_string(::GetCurrentProcessId()) +
            '.' + std::to_string(::GetTickCount64()) + '.' + std::to_string(++sequence));
        std::filesystem::create_directories(root);
        root = std::filesystem::canonical(root);
        reset(Domain::FileAccess::Write, {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create, Domain::FileAccess::Execute}, true);
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    void reset(Domain::FileAccess intent, std::vector<Domain::FileAccess> grants, bool shell) {
        const auto project = parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001");
        issuer = std::make_unique<WindowsWorkspaceAuthority>(std::vector<WindowsWorkspaceAuthorityPolicy>{
            {parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"), project,
             parse<Domain::ClientId>("desktop-artifact-tests"), {pathText(root)}, intent,
             std::move(grants), {}, shell, 1U}});
        authority = std::make_unique<Contracts::WorkspaceAuthority>(take(issuer->authorityFor(project, context())));
        service = std::make_unique<Service>(*issuer, files);
    }
    Domain::Result<std::string> invoke(std::string_view name, const Json& input) {
        return service->execute(name, input.dump(), *authority, context());
    }
    Json execute(std::string_view name, const Json& input) { return Json::parse(take(invoke(name, input))); }
    std::filesystem::path root;
    WindowsAtomicFileStore files;
    std::unique_ptr<WindowsWorkspaceAuthority> issuer;
    std::unique_ptr<Contracts::WorkspaceAuthority> authority;
    std::unique_ptr<Service> service;
};
std::vector<BYTE> read(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(static_cast<bool>(input), "Could not read an image fixture.");
    const std::vector<char> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return {bytes.begin(), bytes.end()};
}
void write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "Could not write a malformed image fixture.");
}
struct Decoded final { UINT width{}; UINT height{}; std::vector<BYTE> pixels; };
Decoded decode(std::vector<BYTE> bytes, const GUID& pixelFormat = GUID_WICPixelFormat32bppBGRA) {
    ComPtr<IWICImagingFactory> factory;
    hr(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC factory failed.");
    ComPtr<IWICStream> stream;
    hr(factory->CreateStream(&stream), "WIC stream failed.");
    hr(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size())), "WIC memory initialization failed.");
    ComPtr<IWICBitmapDecoder> decoder;
    hr(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder), "Generated PNG did not decode.");
    GUID format{};
    hr(decoder->GetContainerFormat(&format), "Generated image format was unavailable.");
    require(format == GUID_ContainerFormatPng, "Generated artifact is not PNG.");
    UINT frames{};
    hr(decoder->GetFrameCount(&frames), "PNG frame count unavailable.");
    require(frames == 1U, "Generated PNG frame count is incorrect.");
    ComPtr<IWICBitmapFrameDecode> frame;
    hr(decoder->GetFrame(0U, &frame), "Generated PNG frame missing.");
    Decoded decoded;
    hr(frame->GetSize(&decoded.width, &decoded.height), "PNG dimensions unavailable.");
    ComPtr<IWICFormatConverter> converted;
    hr(factory->CreateFormatConverter(&converted), "WIC converter failed.");
    hr(converted->Initialize(frame.Get(), pixelFormat, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom), "Generated PNG pixels could not convert.");
    decoded.pixels.resize(static_cast<std::size_t>(decoded.width) * decoded.height * 4U);
    hr(converted->CopyPixels(nullptr, decoded.width * 4U, static_cast<UINT>(decoded.pixels.size()), decoded.pixels.data()),
        "Generated PNG pixel decode was incomplete.");
    return decoded;
}
std::string referenceSha256(std::span<const BYTE> bytes) {
    struct Hash final {
        HCRYPTPROV provider{};
        HCRYPTHASH value{};
        ~Hash() { if (value) ::CryptDestroyHash(value); if (provider) ::CryptReleaseContext(provider, 0U); }
    } hash;
    require(::CryptAcquireContextW(&hash.provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE,
        "The independent CryptoAPI hash provider failed.");
    require(::CryptCreateHash(hash.provider, CALG_SHA_256, 0U, 0U, &hash.value) != FALSE,
        "The independent SHA-256 hash could not be created.");
    require(::CryptHashData(hash.value, bytes.data(), static_cast<DWORD>(bytes.size()), 0U) != FALSE,
        "The independent SHA-256 hash could not read its bytes.");
    std::array<BYTE, 32> hashed{};
    DWORD count = static_cast<DWORD>(hashed.size());
    require(::CryptGetHashParam(hash.value, HP_HASHVAL, hashed.data(), &count, 0U) != FALSE && count == hashed.size(),
        "The independent SHA-256 digest was incomplete.");
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    result.reserve(hashed.size() * 2U);
    for (const auto value : hashed) { result += hex[value >> 4U]; result += hex[value & 15U]; }
    return result;
}
void writeRgbaPng(const std::filesystem::path& path, UINT width, UINT height, std::span<const BYTE> rgba) {
    require(rgba.size() == static_cast<std::size_t>(width) * height * 4U, "The owned RGBA fixture has the wrong pixel count.");
    std::vector<BYTE> bgra{rgba.begin(), rgba.end()};
    for (std::size_t offset = 0; offset < bgra.size(); offset += 4U) std::swap(bgra[offset], bgra[offset + 2U]);
    ComPtr<IWICImagingFactory> factory;
    hr(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "RGBA fixture codec failed.");
    ComPtr<IWICStream> stream;
    hr(factory->CreateStream(&stream), "RGBA fixture stream failed.");
    hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "RGBA fixture destination failed.");
    ComPtr<IWICBitmapEncoder> encoder;
    hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "RGBA fixture encoder failed.");
    hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "RGBA fixture encoder initialization failed.");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    hr(encoder->CreateNewFrame(&frame, &options), "RGBA fixture frame failed.");
    hr(frame->Initialize(options.Get()), "RGBA fixture frame initialization failed.");
    hr(frame->SetSize(width, height), "RGBA fixture size failed.");
    auto format = GUID_WICPixelFormat32bppBGRA;
    hr(frame->SetPixelFormat(&format), "RGBA fixture pixel format failed.");
    require(format == GUID_WICPixelFormat32bppBGRA, "The RGBA fixture encoder lost its alpha pixel format.");
    hr(frame->WritePixels(height, width * 4U, static_cast<UINT>(bgra.size()), bgra.data()), "RGBA fixture pixels failed.");
    hr(frame->Commit(), "RGBA fixture frame commit failed.");
    hr(encoder->Commit(), "RGBA fixture commit failed.");
}
std::vector<BYTE> previewBytes(const Json& result) {
    const auto encoded = result.at("image_base64").get<std::string>();
    DWORD count{};
    require(::CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64,
        nullptr, &count, nullptr, nullptr) != FALSE, "Image preview is invalid base64.");
    std::vector<BYTE> bytes(count);
    require(::CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64,
        bytes.data(), &count, nullptr, nullptr) != FALSE, "Image preview could not decode.");
    return bytes;
}
std::array<BYTE, 4> pixel(const Decoded& image, UINT x, UINT y) {
    require(x < image.width && y < image.height, "Pixel coordinate outside image.");
    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
    return {image.pixels[offset], image.pixels[offset + 1U], image.pixels[offset + 2U], image.pixels[offset + 3U]};
}
void writeNoisyPng(const std::filesystem::path& path) {
    constexpr UINT width = 1024U, height = 768U, stride = width * 3U;
    std::vector<BYTE> pixels(static_cast<std::size_t>(stride) * height);
    std::uint32_t state = 0x82a7c4e9U;
    for (auto& value : pixels) {
        state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
        value = static_cast<BYTE>(state & 0xffU);
    }
    ComPtr<IWICImagingFactory> factory;
    hr(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Noisy fixture codec failed.");
    ComPtr<IWICStream> stream;
    hr(factory->CreateStream(&stream), "Noisy fixture stream failed.");
    hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Noisy fixture destination failed.");
    ComPtr<IWICBitmapEncoder> encoder;
    hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Noisy fixture encoder failed.");
    hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Noisy fixture encoder initialization failed.");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    hr(encoder->CreateNewFrame(&frame, &options), "Noisy fixture frame failed.");
    hr(frame->Initialize(options.Get()), "Noisy fixture frame initialization failed.");
    hr(frame->SetSize(width, height), "Noisy fixture size failed.");
    auto format = GUID_WICPixelFormat24bppBGR;
    hr(frame->SetPixelFormat(&format), "Noisy fixture pixel format failed.");
    require(format == GUID_WICPixelFormat24bppBGR, "Noisy fixture changed its pixel format.");
    hr(frame->WritePixels(height, stride, static_cast<UINT>(pixels.size()), pixels.data()), "Noisy fixture pixels failed.");
    hr(frame->Commit(), "Noisy fixture frame commit failed.");
    hr(encoder->Commit(), "Noisy fixture commit failed.");
}
Json imageArguments(const std::filesystem::path& path) {
    return Json{{"path", pathText(path).value()}, {"width", 128}, {"height", 64}, {"background", "#FFFFFF"},
        {"elements", Json::array({Json{{"type", "rectangle"}, {"x", 2}, {"y", 2}, {"width", 8}, {"height", 8}, {"color", "#FF0000"}},
            Json{{"type", "text"}, {"x", 12}, {"y", 18}, {"size", 18}, {"text", "Unicode \xce\xa9\xe2\x82\xac"}}})}};
}
void imageCodecPixelsUnicodeAndOverwrite() {
    Fixture fixture;
    const auto path = fixture.root / L"nested" / L"\u03a9\u20ac" / L"image.png";
    auto input = imageArguments(path);
    const auto receipt = fixture.execute("image_write", input);
    const auto bytes = read(path);
    const auto image = decode(bytes);
    require(receipt.at("ok") == true && receipt.at("bytes_written") == bytes.size() && receipt.at("width") == 128 &&
        receipt.at("height") == 64 && receipt.at("format") == "png", "Image write receipt is inaccurate.");
    require(image.width == 128 && image.height == 64 && pixel(image, 4, 4) == std::array<BYTE, 4>{0, 0, 255, 255} &&
        pixel(image, 127, 63) == std::array<BYTE, 4>{255, 255, 255, 255}, "Generated image pixels or RGB channels are incorrect.");
    bool renderedText{};
    for (UINT y = 18; y < 45; ++y) for (UINT x = 12; x < 125; ++x)
        renderedText = renderedText || pixel(image, x, y) != std::array<BYTE, 4>{255, 255, 255, 255};
    require(renderedText, "Unicode image text did not render.");
    const auto thumbnail = decode(previewBytes(receipt));
    require(thumbnail.width == 128 && thumbnail.height == 64 && thumbnail.pixels == image.pixels,
        "Small image preview does not preserve generated pixels.");
    const auto observed = fixture.execute("image_read", Json{{"path", pathText(path).value()}});
    require(observed.at("width") == 128 && observed.at("height") == 64 && observed.at("format") == "png" &&
        decode(previewBytes(observed)).pixels == image.pixels, "Image read did not actually decode the generated image.");
    input["background"] = "#0000FF";
    input["elements"] = Json::array();
    const auto replaced = fixture.execute("image_write", input);
    require(replaced.at("ok") == true && read(path) != bytes && pixel(decode(read(path)), 40, 40) == std::array<BYTE, 4>{255, 0, 0, 255},
        "Write-authorized image overwrite failed or preserved stale pixels.");
    input["width"] = 512; input["height"] = 128;
    const auto scaled = fixture.execute("image_write", input);
    const auto preview = decode(previewBytes(scaled));
    require(preview.width == 256 && preview.height == 64 && scaled.at("preview_width") == 256 && scaled.at("preview_height") == 64,
        "Image preview dimensions did not honor the bounded aspect ratio.");
    input["path"] = "relative\\image.png";
    fixture.execute("image_write", input);
    require(std::filesystem::exists(fixture.root / L"relative" / L"image.png") &&
        fixture.execute("image_read", Json{{"path", "relative\\image.png"}}).at("width") == 512,
        "Relative image paths did not use the default registered workspace.");
}
void rectangleDimensionsAndCanvasClipping() {
    Fixture fixture;
    const auto path = fixture.root / L"exact-rectangle-pixels.png";
    Json input{{"path", pathText(path).value()}, {"width", 8}, {"height", 8}, {"background", "#FFFFFF"},
        {"elements", Json::array({
            Json{{"type", "rectangle"}, {"x", 1}, {"y", 1}, {"width", 1}, {"height", 1}, {"color", "#FF0000"}},
            Json{{"type", "rectangle"}, {"x", 4}, {"y", 2}, {"width", 2}, {"height", 2}, {"color", "#00FF00"}},
            Json{{"type", "rectangle"}, {"x", -1}, {"y", 6}, {"width", 3}, {"height", 3}, {"color", "#0000FF"}},
            Json{{"type", "rectangle"}, {"x", 7}, {"y", -1}, {"width", 2}, {"height", 3}, {"color", "#FFFF00"}}})}};
    const auto receipt = fixture.execute("image_write", input);
    const auto image = decode(read(path));
    require(image.width == 8U && image.height == 8U && receipt.at("width") == 8 && receipt.at("height") == 8,
        "Clipped rectangles changed the owned canvas dimensions.");
    constexpr std::array<std::string_view, 8> expectedRows{
        ".......Y", ".R.....Y", "....GG..", "....GG..", "........", "........", "BB......", "BB......"};
    for (UINT y = 0U; y < 8U; ++y) for (UINT x = 0U; x < 8U; ++x) {
        const auto expected = [&]() -> std::array<BYTE, 4> {
            switch (expectedRows[y][x]) {
            case 'R': return {0, 0, 255, 255};
            case 'G': return {0, 255, 0, 255};
            case 'B': return {255, 0, 0, 255};
            case 'Y': return {0, 255, 255, 255};
            default: return {255, 255, 255, 255};
            }
        }();
        require(pixel(image, x, y) == expected,
            "Rectangle dimensions, boundaries or canvas clipping differ from the exact owned PNG pixel grid.");
    }
    require(decode(previewBytes(receipt)).pixels == image.pixels,
        "The exact small rectangle preview changed the written PNG pixels.");
}
void imagePreviewResolutionAndEncodedBound() {
    Fixture fixture;
    const auto path = fixture.root / L"detail.png";
    auto input = imageArguments(path);
    input["width"] = 1024; input["height"] = 768; input["preview_max_dimension"] = 1024;
    input["elements"] = Json::array({Json{{"type", "rectangle"}, {"x", 500}, {"y", 360},
        {"width", 2}, {"height", 2}, {"color", "#FF0000"}}});
    const auto written = fixture.execute("image_write", input);
    const auto preview = decode(previewBytes(written));
    require(preview.width == 1024U && preview.height == 768U,
        "A requested high-resolution preview remains constrained to the old 256-pixel thumbnail.");
    const auto source = read(path);
    const auto sourcePixels = decode(source);
    for (const auto& [x, y] : std::array<std::pair<UINT, UINT>, 4>{{{500, 360}, {501, 360}, {500, 361}, {501, 361}}})
        require(pixel(sourcePixels, x, y) == std::array<BYTE, 4>{0, 0, 255, 255},
            "The owned source PNG did not contain its requested two-pixel rectangle detail.");
    require(pixel(sourcePixels, 502, 361) == std::array<BYTE, 4>{255, 255, 255, 255} &&
        pixel(sourcePixels, 501, 362) == std::array<BYTE, 4>{255, 255, 255, 255},
        "The owned source PNG rectangle extended beyond its requested dimensions.");
    require(preview.pixels == sourcePixels.pixels,
        "The requested full-size preview altered the owned source PNG pixels.");
    require(written.at("preview_max_dimension_requested") == 1024 && written.at("preview_reduced_for_byte_limit") == false &&
        written.at("preview_encoded_byte_limit") == 512U * 1024U &&
        written.at("preview_encoded_bytes") == written.at("image_base64").get_ref<const std::string&>().size(),
        "High-resolution preview metadata did not describe its actual request and byte budget.");
    const auto defaultRead = fixture.execute("image_read", Json{{"path", pathText(path).value()}});
    require(defaultRead.at("preview_max_dimension_requested") == 256 && defaultRead.at("preview_width") == 256 &&
        defaultRead.at("preview_height") == 192 && defaultRead.at("preview_reduced_for_byte_limit") == false,
        "The optional image preview parameter changed the existing 256-pixel default.");
    const auto detailedRead = fixture.execute("image_read", Json{{"path", pathText(path).value()}, {"preview_max_dimension", 2048}});
    require(decode(previewBytes(detailedRead)).pixels == sourcePixels.pixels && detailedRead.at("preview_width") == 1024 &&
        detailedRead.at("preview_height") == 768, "Image read did not honor the requested limit or unnecessarily upscale the source.");
    const auto minimumRead = fixture.execute("image_read", Json{{"path", pathText(path).value()}, {"preview_max_dimension", 128}});
    require(minimumRead.at("preview_width") == 128 && minimumRead.at("preview_height") == 96,
        "The minimum supported preview dimension did not preserve the aspect ratio.");
    for (const auto& invalid : std::vector<Json>{127, 2049, 0, -1, 512.5, "1024", true, (std::numeric_limits<std::uint64_t>::max)()}) {
        auto changed = input; changed["background"] = "#0000FF"; changed["preview_max_dimension"] = invalid;
        requireError(fixture.invoke("image_write", changed), Domain::ErrorCodes::InvalidRequest,
            "Invalid preview dimensions were accepted for image publication.");
        require(read(path) == source, "An invalid preview dimension changed the existing destination.");
        const auto absent = fixture.root / L"invalid-preview-directory";
        changed["path"] = pathText(absent / L"new.png").value();
        requireError(fixture.invoke("image_write", changed), Domain::ErrorCodes::InvalidRequest,
            "Invalid preview dimensions created an image.");
        require(!std::filesystem::exists(absent), "Invalid preview dimensions created a parent directory.");
        requireError(fixture.invoke("image_read", Json{{"path", pathText(path).value()}, {"preview_max_dimension", invalid}}),
            Domain::ErrorCodes::InvalidRequest, "Image read accepted an invalid preview dimension.");
    }
    const auto noisyPath = fixture.root / L"noisy.png";
    writeNoisyPng(noisyPath);
    const auto noisySource = read(noisyPath);
    require(noisySource.size() > 512U * 1024U, "The noisy fixture did not exceed the preview transport byte budget.");
    const auto noisy = fixture.execute("image_read", Json{{"path", pathText(noisyPath).value()}, {"preview_max_dimension", 2048}});
    const auto noisyPreview = decode(previewBytes(noisy));
    require(noisy.at("preview_reduced_for_byte_limit") == true && noisy.at("preview_max_dimension_requested") == 2048 &&
        noisy.at("image_base64").get_ref<const std::string&>().size() <= 512U * 1024U && noisyPreview.width < 1024U &&
        noisyPreview.width == noisy.at("preview_width") && noisyPreview.height == noisy.at("preview_height") &&
        noisy.at("preview_encoded_bytes") == noisy.at("image_base64").get_ref<const std::string&>().size(),
        "A noisy high-resolution preview did not adapt within the exact encoded transport bound.");
    require(read(noisyPath) == noisySource, "Adaptive preview resizing modified the authorized source image.");
}
void imageReadCanonicalPixelsAlphaHashesAndSamples() {
    Fixture fixture;
    const auto path = fixture.root / L"actual-alpha.png";
    constexpr std::array<BYTE, 24> rgba{
        255, 0, 0, 255, 0, 128, 255, 64, 17, 34, 51, 0,
        1, 2, 3, 1, 19, 37, 73, 128, 0, 255, 0, 255};
    constexpr std::string_view referenceDigest = "d51aa77f186d81a1558eb33ebd4a22ac44921762b9eb0a1d0eb42fafb518fc29";
    require(referenceSha256(rgba) == referenceDigest, "The independent SHA-256 did not match the pinned owned RGBA frame.");
    writeRgbaPng(path, 3U, 2U, rgba);
    const auto source = read(path);
    const auto independent = decode(source, GUID_WICPixelFormat32bppRGBA);
    require(independent.width == 3U && independent.height == 2U &&
        independent.pixels == std::vector<BYTE>{rgba.begin(), rgba.end()},
        "The independently decoded owned PNG changed its RGB channels, row order or nonopaque alpha.");
    fixture.reset(Domain::FileAccess::Read, {Domain::FileAccess::Read}, false);
    auto arguments = Json{{"path", pathText(path).value()}};
    const auto defaultReceipt = fixture.execute("image_read", arguments);
    require(defaultReceipt.at("decoded_pixel_format") == "RGBA8" && defaultReceipt.at("decoded_frame_index") == 0 &&
        defaultReceipt.at("decoded_width") == 3 && defaultReceipt.at("decoded_height") == 2 &&
        defaultReceipt.at("decoded_row_stride_bytes") == 12 && defaultReceipt.at("decoded_rgba8_sha256").get<std::string>() == referenceDigest &&
        defaultReceipt.at("decoded_rgba8_sha256") == referenceSha256(independent.pixels),
        "Image read did not hash its actual tightly packed top-to-bottom frame-0 RGBA8 bytes.");
    require(!defaultReceipt.contains("pixel_samples") && !defaultReceipt.contains("expected_pixels") &&
        defaultReceipt.at("preview_max_dimension_requested") == 256,
        "The default image read invented samples or changed its preview default.");
    require(defaultReceipt.at("preview_png_sha256") == referenceSha256(previewBytes(defaultReceipt)),
        "The image preview digest does not cover the exact emitted PNG bytes.");
    const auto coordinates = Json::array({Json{{"x", 2}, {"y", 0}}, Json{{"x", 0}, {"y", 1}},
        Json{{"x", 1}, {"y", 0}}, Json{{"x", 1}, {"y", 1}}, Json{{"x", 0}, {"y", 0}},
        Json{{"x", 2}, {"y", 1}}, Json{{"x", 2}, {"y", 0}}});
    arguments["samples"] = coordinates;
    arguments["preview_max_dimension"] = 2048;
    const auto sampled = fixture.execute("image_read", arguments);
    require(sampled.at("pixel_samples").size() == coordinates.size() &&
        sampled.at("decoded_rgba8_sha256") == defaultReceipt.at("decoded_rgba8_sha256") &&
        sampled.at("preview_png_sha256") == referenceSha256(previewBytes(sampled)),
        "Sampling or preview sizing changed the canonical source hash or the emitted preview hash.");
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        const auto x = coordinates[index].at("x").get<UINT>(), y = coordinates[index].at("y").get<UINT>();
        const auto offset = (static_cast<std::size_t>(y) * 3U + x) * 4U;
        const auto expected = Json{{"x", x}, {"y", y}, {"rgba", Json::array({rgba[offset], rgba[offset + 1U],
            rgba[offset + 2U], rgba[offset + 3U]})}};
        require(sampled.at("pixel_samples").at(index) == expected,
            "Image read changed sampled RGBA channels, fabricated alpha, or reordered/removed duplicate coordinates.");
    }
    arguments["samples"] = Json::array();
    for (unsigned index = 0; index < 64U; ++index) arguments["samples"].push_back(Json{{"x", 1}, {"y", 1}});
    const auto maximum = fixture.execute("image_read", arguments);
    require(maximum.at("pixel_samples").size() == 64U && maximum.at("pixel_samples").front().at("rgba") ==
        Json::array({19, 37, 73, 128}) && maximum.at("pixel_samples").back() == maximum.at("pixel_samples").front(),
        "Image read did not retain its maximum bounded sample request.");
    const auto rowPath = fixture.root / L"row-block.png";
    constexpr UINT rowWidth = 2U, rowHeight = 129U;
    std::vector<BYTE> rowPixels(static_cast<std::size_t>(rowWidth) * rowHeight * 4U);
    for (UINT y = 0U; y < rowHeight; ++y) for (UINT x = 0U; x < rowWidth; ++x) {
        const auto offset = (static_cast<std::size_t>(y) * rowWidth + x) * 4U;
        rowPixels[offset] = static_cast<BYTE>(y); rowPixels[offset + 1U] = static_cast<BYTE>(x * 83U);
        rowPixels[offset + 2U] = static_cast<BYTE>(255U - y); rowPixels[offset + 3U] = static_cast<BYTE>(y + x);
    }
    writeRgbaPng(rowPath, rowWidth, rowHeight, rowPixels);
    const auto rowSource = read(rowPath);
    const auto rowReference = decode(rowSource, GUID_WICPixelFormat32bppRGBA);
    require(rowReference.pixels == rowPixels, "The owned row-block RGBA fixture changed before the native read.");
    const auto rowReceipt = fixture.execute("image_read", Json{{"path", pathText(rowPath).value()},
        {"samples", Json::array({Json{{"x", 1}, {"y", 63}}, Json{{"x", 0}, {"y", 64}}, Json{{"x", 1}, {"y", 128}}})}});
    require(rowReceipt.at("decoded_rgba8_sha256") == referenceSha256(rowReference.pixels) &&
        rowReceipt.at("decoded_row_stride_bytes") == 8 && rowReceipt.at("decoded_height") == rowHeight &&
        rowReceipt.at("pixel_samples").at(0).at("rgba") == Json::array({63, 83, 192, 64}) &&
        rowReceipt.at("pixel_samples").at(1).at("rgba") == Json::array({64, 0, 191, 64}) &&
        rowReceipt.at("pixel_samples").at(2).at("rgba") == Json::array({128, 83, 127, 129}),
        "Canonical RGBA decoding skipped or changed pixels at a row-block boundary or in the final partial block.");
    require(read(path) == source && read(rowPath) == rowSource,
        "Sampling or canonical RGBA decoding changed an authorized source image.");
}
void imageReadInvalidSamplesAndAuthorityHaveNoEffects() {
    Fixture fixture;
    const auto path = fixture.root / L"bounded-samples.png";
    constexpr std::array<BYTE, 8> rgba{4, 8, 16, 32, 64, 128, 255, 0};
    writeRgbaPng(path, 2U, 1U, rgba);
    const auto original = read(path);
    auto oversized = Json::array();
    for (unsigned index = 0; index < 65U; ++index) oversized.push_back(Json{{"x", 0}, {"y", 0}});
    std::vector<Json> invalidSamples{nullptr, true, 4, "coordinates", Json::object(), Json::array(), oversized,
        Json::array({nullptr}), Json::array({Json::array({0, 0})}), Json::array({Json::object()}),
        Json::array({Json{{"x", 0}}}), Json::array({Json{{"y", 0}}}),
        Json::array({Json{{"x", 0}, {"y", 0}, {"rgba", Json::array({1, 2, 3, 4})}}}),
        Json::array({Json{{"x", 2}, {"y", 0}}}), Json::array({Json{{"x", 0}, {"y", 1}}})};
    for (const auto& value : std::vector<Json>{-1, 4096, 0.0, 0.5, "0", true, nullptr,
        (std::numeric_limits<std::uint64_t>::max)()}) {
        invalidSamples.push_back(Json::array({Json{{"x", value}, {"y", 0}}}));
        invalidSamples.push_back(Json::array({Json{{"x", 0}, {"y", value}}}));
    }
    for (const auto& samples : invalidSamples) {
        requireError(fixture.invoke("image_read", Json{{"path", pathText(path).value()}, {"samples", samples}}),
            Domain::ErrorCodes::InvalidRequest, "Image read accepted an invalid or out-of-frame pixel sample.");
        require(read(path) == original, "Invalid pixel samples changed the owned source bytes.");
    }
    const auto absent = fixture.root / L"must-stay-absent";
    requireError(fixture.invoke("image_read", Json{{"path", pathText(absent / L"source.png").value()},
        {"samples", Json::array()}}), Domain::ErrorCodes::InvalidRequest,
        "Malformed samples were not validated before reading a missing source.");
    require(!std::filesystem::exists(absent), "Invalid sampling created a source directory.");
    const auto valid = Json{{"path", pathText(path).value()}, {"samples", Json::array({Json{{"x", 1}, {"y", 0}}})}};
    TestContext cancelled; cancelled.cancellation.request_stop();
    requireError(fixture.service->execute("image_read", valid.dump(), *fixture.authority, cancelled.active()),
        Domain::ErrorCodes::Cancelled, "Cancelled pixel inspection was accepted.");
    requireError(fixture.service->execute("image_read", valid.dump(), *fixture.authority, TestContext{}.expired()),
        Domain::ErrorCodes::DeadlineExceeded, "Expired pixel inspection was accepted.");
    auto outside = valid; outside["path"] = pathText(fixture.root.parent_path() / L"outside-pixel-authority.png").value();
    requireError(fixture.invoke("image_read", outside), Domain::ErrorCodes::PathOutsideAuthority,
        "Pixel inspection escaped the workspace authority.");
    fixture.reset(Domain::FileAccess::Create, {Domain::FileAccess::Create}, false);
    requireError(fixture.invoke("image_read", valid), Domain::ErrorCodes::Unauthorized,
        "Pixel inspection exposed source pixels without read authority.");
    require(read(path) == original, "Invalid, denied or cancelled pixel inspection changed the source image.");
}
void permissionsInvalidAndCancelledHaveNoWriteEffects() {
    Fixture fixture;
    const auto path = fixture.root / L"existing.png";
    const auto input = imageArguments(path);
    fixture.execute("image_write", input);
    const auto original = read(path);
    {
        Detail::UniqueHandle locked{::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(static_cast<bool>(locked), "Could not lock owned image against replacement.");
        auto changed = input; changed["background"] = "#0000FF";
        require(!fixture.invoke("image_write", changed), "Image writer reported success for a destination locked against replacement.");
    }
    require(read(path) == original, "Failed atomic replacement changed existing PNG bytes.");
    fixture.reset(Domain::FileAccess::Read, {Domain::FileAccess::Read}, false);
    require(fixture.execute("image_read", Json{{"path", pathText(path).value()}}).at("ok") == true,
        "Read-only authority could not inspect an image.");
    requireError(fixture.invoke("image_write", input), Domain::ErrorCodes::Unauthorized, "Read-only capability created an image.");
    requireError(fixture.invoke("browser_open", Json{{"url", "https://example.com/"}}), Domain::ErrorCodes::Unauthorized,
        "Read-only capability launched a browser.");
    fixture.reset(Domain::FileAccess::Create, {Domain::FileAccess::Read, Domain::FileAccess::Create}, false);
    requireError(fixture.invoke("image_write", input), Domain::ErrorCodes::Unauthorized, "Create-only capability overwrote an image.");
    require(read(path) == original, "Denied overwrite altered the existing image.");
    auto fresh = input; fresh["path"] = pathText(fixture.root / L"new.png").value();
    require(fixture.execute("image_write", fresh).at("ok") == true, "Create-only capability could not create a new image.");
    fixture.reset(Domain::FileAccess::Write, {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create, Domain::FileAccess::Execute}, true);
    const auto absentDirectory = fixture.root / L"invalid-must-not-create";
    auto invalid = imageArguments(absentDirectory / L"image.png");
    invalid["elements"].push_back(Json{{"type", "unsupported"}});
    requireError(fixture.invoke("image_write", invalid), Domain::ErrorCodes::InvalidRequest, "Invalid shape was accepted.");
    require(!std::filesystem::exists(absentDirectory), "Invalid drawing created destination directories.");
    invalid = input; invalid["width"] = 4097;
    requireError(fixture.invoke("image_write", invalid), Domain::ErrorCodes::InvalidRequest, "Oversized drawing dimension was accepted.");
    invalid = input; invalid["background"] = 3;
    requireError(fixture.invoke("image_write", invalid), Domain::ErrorCodes::InvalidRequest, "Invalid color type was accepted.");
    invalid = input; invalid["elements"][0]["color"] = "#FF00Q0";
    requireError(fixture.invoke("image_write", invalid), Domain::ErrorCodes::InvalidRequest, "Invalid hexadecimal color was accepted.");
    const auto outside = fixture.root.parent_path() / L"outside-authority.png";
    invalid = input; invalid["path"] = pathText(outside).value();
    requireError(fixture.invoke("image_write", invalid), Domain::ErrorCodes::PathOutsideAuthority, "Image escaped its workspace authority.");
    requireError(fixture.service->execute("image_write", "{\"path\":\"first\",\"path\":\"second\"}", *fixture.authority, context()),
        Domain::ErrorCodes::InvalidRequest, "Duplicate image argument keys were accepted.");
    requireError(fixture.service->execute("image_write", std::string(Service::MaximumArgumentBytes + 1U, ' '), *fixture.authority, context()),
        Domain::ErrorCodes::PayloadTooLarge, "Unbounded image JSON was accepted.");
    TestContext cancelled; cancelled.cancellation.request_stop();
    requireError(fixture.service->execute("image_write", input.dump(), *fixture.authority, cancelled.active()),
        Domain::ErrorCodes::Cancelled, "Cancelled image request was accepted.");
    requireError(fixture.service->execute("image_write", input.dump(), *fixture.authority, TestContext{}.expired()),
        Domain::ErrorCodes::DeadlineExceeded, "Expired image request was accepted.");
    require(read(path) == original, "Invalid/cancelled image operation changed the destination.");
    write(fixture.root / L"malformed.png", "not an image");
    requireError(fixture.invoke("image_read", Json{{"path", pathText(fixture.root / L"malformed.png").value()}}),
        Domain::ErrorCodes::InvalidRequest, "Malformed image bytes were treated as an image.");
    requireError(fixture.invoke("desktop_type", Json{{"text", "must never send"}}), Domain::ErrorCodes::InvalidRequest,
        "Missing desktop identity was accepted.");
    requireError(fixture.invoke("browser_open", Json{{"url", "https://"}}), Domain::ErrorCodes::InvalidRequest,
        "Browser launch accepted an empty host.");
}
class OwnedWindow final {
public:
    explicit OwnedWindow(std::wstring initialText = {},
        std::wstring title = L"Forge Conductor owned desktop test", int additionalButtons = 0,
        std::wstring buttonPrefix = L"Paged Unicode \u03a9\u20ac ", std::wstring lateText = {})
        : worker_{[this, initialText = std::move(initialText), title = std::move(title), additionalButtons,
            buttonPrefix = std::move(buttonPrefix), lateText = std::move(lateText)](std::stop_token stop) {
                run(stop, initialText, title, additionalButtons, buttonPrefix, lateText); }} {
        std::unique_lock lock{mutex_};
        if (!ready_.wait_for(lock, 5s, [this] { return initialized_; }) || window_ == nullptr || edit_ == nullptr || button_ == nullptr) {
            worker_.request_stop();
            ::PostThreadMessageW(::GetThreadId(worker_.native_handle()), WM_QUIT, 0, 0);
            throw TestFailure{"Owned desktop window creation failed or timed out."};
        }
    }
    ~OwnedWindow() {
        worker_.request_stop();
        ::PostThreadMessageW(::GetThreadId(worker_.native_handle()), WM_QUIT, 0, 0);
        worker_.join();
    }
    Json identity() const { return Json{{"window_id", reinterpret_cast<std::uintptr_t>(window_)}, {"pid", ::GetCurrentProcessId()}}; }
    bool isForeground() const { return ::GetForegroundWindow() == window_; }
    HWND focusedControl() const {
        GUITHREADINFO information{};
        information.cbSize = sizeof(information);
        require(::GetGUIThreadInfo(::GetWindowThreadProcessId(window_, nullptr), &information) != FALSE,
            "The owned window's GUI-thread focus could not be observed.");
        return information.hwndFocus;
    }
    bool inputReady() const { return isForeground() && focusedControl() == edit_; }
    void requestActivation() const { static_cast<void>(::SetForegroundWindow(window_)); }
    std::wstring observedText() const {
        std::array<wchar_t, 256> text{};
        DWORD_PTR copied{};
        require(::SendMessageTimeoutW(edit_, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()),
            SMTO_ABORTIFHUNG, 1000U, &copied) != 0, "Owned edit text readback failed.");
        return text.data();
    }
private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_ACTIVATE && LOWORD(wparam) != WA_INACTIVE) {
            if (const auto edit = ::GetDlgItem(window, 1)) {
                ::SetFocus(edit);
                // DefWindowProc would assign focus to the top-level window
                // after this handler selected its edit control.
                return 0;
            }
        }
        if (message == WM_SETFOCUS) {
            if (const auto edit = ::GetDlgItem(window, 1)) {
                ::SetFocus(edit);
                return 0;
            }
        }
        if (message == WM_DESTROY) { ::PostQuitMessage(0); return 0; }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }
    void run(std::stop_token stop, const std::wstring& initialText, const std::wstring& title,
        const int additionalButtons, const std::wstring& buttonPrefix, const std::wstring& lateText) {
        WNDCLASSW type{};
        type.lpfnWndProc = &procedure; type.hInstance = ::GetModuleHandleW(nullptr);
        type.lpszClassName = L"ForgeConductor.OwnedDesktopArtifactTest";
        ::RegisterClassW(&type);
        const auto window = ::CreateWindowExW(0, type.lpszClassName, title.c_str(),
            WS_OVERLAPPEDWINDOW, 100, 100, 340, 180, nullptr, nullptr, type.hInstance, nullptr);
        const auto edit = window ? ::CreateWindowExW(0, L"EDIT", initialText.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            10, 10, 280, 32, window, reinterpret_cast<HMENU>(1), type.hInstance, nullptr) : nullptr;
        const auto button = window ? ::CreateWindowExW(0, L"BUTTON", L"Owned action", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 55, 120, 28, window, reinterpret_cast<HMENU>(2), type.hInstance, nullptr) : nullptr;
        for (int index{}; window && index < additionalButtons; ++index) {
            const auto label = buttonPrefix + std::to_wstring(index);
            if (!::CreateWindowExW(0, L"BUTTON", label.c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    10, 90, 120, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(index + 10)), type.hInstance, nullptr)) {
                ::DestroyWindow(window);
                { std::lock_guard lock{mutex_}; initialized_ = true; }
                ready_.notify_one();
                return;
            }
        }
        if (window && !lateText.empty())
            ::CreateWindowExW(0, L"EDIT", lateText.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                10, 125, 280, 32, window, reinterpret_cast<HMENU>(3), type.hInstance, nullptr);
        if (window) ::ShowWindow(window, SW_SHOWNOACTIVATE);
        { std::lock_guard lock{mutex_}; window_ = window; edit_ = edit; button_ = button; initialized_ = true; }
        ready_.notify_one();
        if (!window) return;
        MSG message{};
        while (!stop.stop_requested() && ::GetMessageW(&message, nullptr, 0, 0) > 0) { ::TranslateMessage(&message); ::DispatchMessageW(&message); }
        ::DestroyWindow(window);
        ::UnregisterClassW(type.lpszClassName, type.hInstance);
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    bool initialized_{};
    HWND window_{};
    HWND edit_{};
    HWND button_{};
    std::jthread worker_;
};
void desktopInventoryBoundsActualSerializedTitles() {
    Fixture fixture;
    fixture.reset(Domain::FileAccess::Read, {Domain::FileAccess::Read}, false);
    const auto foreground = ::GetForegroundWindow();
    std::vector<std::unique_ptr<OwnedWindow>> windows;
    for (int index{}; index < 20; ++index) {
        auto title = L"Forge bounded inventory " + std::to_wstring(index) + L" ";
        title.append(3600U, L'\u4e8c');
        windows.push_back(std::make_unique<OwnedWindow>(L"", std::move(title)));
    }
    const auto observed = fixture.execute("desktop_list", Json{{"limit", 500}});
    require(observed.at("ok") == true && observed.at("truncated") == true &&
                observed.dump().size() <= 48U * 1024U,
        "The desktop inventory exceeded its serialized UTF-8 budget or hid truncation.");
    bool retainedOwnedIdentity{};
    for (const auto& row : observed.at("windows")) {
        if (row.at("title").get<std::string>().starts_with("Forge bounded inventory ")) {
            const auto handle = reinterpret_cast<HWND>(row.at("window_id").get<std::uintptr_t>());
            DWORD pid{};
            require(::IsWindow(handle) != FALSE && ::GetWindowThreadProcessId(handle, &pid) != 0 &&
                pid == ::GetCurrentProcessId() && row.at("pid") == pid,
                "Bounded desktop inventory lost an actual owned HWND/PID identity.");
            retainedOwnedIdentity = true;
        }
    }
    require(retainedOwnedIdentity && ::GetForegroundWindow() == foreground,
        "Bounded inventory omitted all long-title owned fixtures or activated a window.");
}
void ownedWindowAccessibilityReadWithoutInput() {
    Fixture fixture;
    fixture.reset(Domain::FileAccess::Read, {Domain::FileAccess::Read}, false);
    OwnedWindow owned{L"Owned Unicode \u03a9\u20ac"};
    const auto foreground = ::GetForegroundWindow();
    auto request = owned.identity(); request["limit"] = 20;
    const auto observed = fixture.execute("desktop_read", request);
    bool editValue{}, buttonWithoutText{};
    for (const auto& element : observed.at("elements")) {
        if (element.at("control_type") == UIA_EditControlTypeId && element.contains("text"))
            editValue = element.at("text") == "Owned Unicode \xce\xa9\xe2\x82\xac";
        if (element.at("control_type") == UIA_ButtonControlTypeId && element.at("name") == "Owned action")
            buttonWithoutText = !element.contains("text");
    }
    require(observed.at("ok") == true && editValue && buttonWithoutText,
        "Accessibility read did not retain a real edit value and safely omit unsupported button text patterns.");
    require(::GetForegroundWindow() == foreground && owned.observedText() == L"Owned Unicode \u03a9\u20ac",
        "Read-only accessibility inspection activated or changed the owned window.");
}
void ownedWindowCapturePreviewBound() {
    Fixture fixture;
    OwnedWindow owned{L"Owned capture value", L"Forge Conductor owned capture preview test"};
    const auto foreground = ::GetForegroundWindow();
    const auto path = fixture.root / L"capture-preview.png";
    auto request = owned.identity(); request["path"] = pathText(path).value(); request["preview_max_dimension"] = 128;
    const auto receipt = fixture.execute("desktop_capture", request);
    const auto bytes = read(path);
    const auto captured = decode(bytes);
    const auto thumbnail = decode(previewBytes(receipt));
    require(receipt.at("preview_max_dimension_requested") == 128 && (std::max)(thumbnail.width, thumbnail.height) == 128U &&
        receipt.at("preview_width") == thumbnail.width && receipt.at("preview_height") == thumbnail.height &&
        receipt.at("width") == captured.width && receipt.at("height") == captured.height &&
        receipt.at("capture_source") == "visible_desktop_window_region" && receipt.at("occlusion_possible") == true &&
        ::GetForegroundWindow() == foreground, "Desktop capture ignored its preview request, lost full dimensions, or activated the window.");
    request["preview_max_dimension"] = 127;
    requireError(fixture.invoke("desktop_capture", request), Domain::ErrorCodes::InvalidRequest,
        "Desktop capture accepted an invalid preview limit.");
    require(read(path) == bytes && ::GetForegroundWindow() == foreground,
        "An invalid desktop preview request replaced the destination or activated a window.");
    const auto absent = fixture.root / L"invalid-capture-preview-directory";
    request["path"] = pathText(absent / L"capture.png").value(); request["preview_max_dimension"] = 2049;
    requireError(fixture.invoke("desktop_capture", request), Domain::ErrorCodes::InvalidRequest,
        "Desktop capture accepted an oversized preview limit.");
    require(!std::filesystem::exists(absent), "Invalid desktop preview request created destination parents.");
}
void ownedWindowAccessibilityPaging() {
    Fixture fixture;
    fixture.reset(Domain::FileAccess::Read, {Domain::FileAccess::Read}, false);
    const std::string expectedLateText{"Late Unicode \xce\xa9\xe2\x82\xac beyond the first 300 controls"};
    OwnedWindow owned{L"Initial value", L"Forge Conductor owned paging test", 350,
        L"Paged Unicode \u03a9\u20ac ", L"Late Unicode \u03a9\u20ac beyond the first 300 controls"};
    const auto foreground = ::GetForegroundWindow();
    auto request = owned.identity(); request["limit"] = 300;
    auto page = fixture.execute("desktop_read", request);
    require(page.contains("offset") && page.contains("next_offset") && page.contains("has_more"),
        "desktop_read does not expose offsets for controls beyond its bounded first page.");
    require(page.at("offset") == 0 && page.at("has_more") == true && page.at("total_elements") > 350,
        "The owned paging fixture did not exceed the first page or preserve the default offset.");
    std::set<int> indices;
    std::size_t returnedButtons{};
    bool lateText{};
    std::int64_t offset{};
    for (unsigned attempt{}; ; ++attempt) {
        require(attempt < 20U && page.dump().size() <= 64U * 1024U,
            "Accessibility paging did not terminate within its serialized result bound.");
        require(page.at("offset") == offset && page.at("returned_elements") == page.at("elements").size(),
            "Accessibility page metadata did not describe its emitted controls.");
        const auto scanned = page.at("scanned_elements").get<std::int64_t>();
        require(scanned >= 0 && scanned <= 300, "Accessibility page scanned beyond its requested limit.");
        for (const auto& element : page.at("elements")) {
            const auto index = element.at("index").get<int>();
            require(index >= offset && index < offset + scanned && indices.insert(index).second,
                "Accessibility paging skipped its index range or returned duplicate controls.");
            if (element.at("name").get<std::string>().starts_with("Paged Unicode ")) ++returnedButtons;
            if (element.contains("text") && element.at("text") == expectedLateText) {
                require(index >= 300, "The late Unicode fixture was not beyond the original inaccessible first page.");
                lateText = true;
            }
        }
        if (!page.at("has_more").get<bool>()) {
            require(page.at("next_offset").is_null() && offset + scanned == page.at("total_elements"),
                "The terminal accessibility page lost its ending position.");
            break;
        }
        const auto next = page.at("next_offset").get<std::int64_t>();
        require(scanned > 0 && next == offset + scanned && next > offset,
            "Accessibility paging did not advance to the first unconsumed control.");
        offset = next; request["offset"] = offset;
        page = fixture.execute("desktop_read", request);
    }
    require(returnedButtons == 350U && lateText && ::GetForegroundWindow() == foreground && owned.observedText() == L"Initial value",
        "Paging omitted owned controls/the late Unicode value, activated the window, or changed its text.");
    request["offset"] = page.at("total_elements");
    const auto empty = fixture.execute("desktop_read", request);
    require(empty.at("elements").empty() && empty.at("scanned_elements") == 0 && empty.at("has_more") == false &&
        empty.at("next_offset").is_null(), "An end offset did not produce a terminal empty page.");
    request["offset"] = page.at("total_elements").get<std::int64_t>() + 1;
    requireError(fixture.invoke("desktop_read", request), Domain::ErrorCodes::InvalidRequest,
        "Accessibility paging accepted an offset beyond the observed tree.");
    request["offset"] = -1;
    requireError(fixture.invoke("desktop_read", request), Domain::ErrorCodes::InvalidRequest,
        "Accessibility paging accepted a negative offset.");
}
void ownedWindowAccessibilityByteBoundPaging(const bool escapedLabels) {
    Fixture fixture;
    fixture.reset(Domain::FileAccess::Read, {Domain::FileAccess::Read}, false);
    OwnedWindow owned{L"Initial value", L"Forge Conductor owned escaped paging test", 40,
        escapedLabels ? std::wstring(3500U, L'\x0001') : std::wstring(1800U, L'\u4e8c'),
        L"Late Unicode \u03a9\u20ac after bounded text"};
    const auto foreground = ::GetForegroundWindow();
    auto request = owned.identity(); request["limit"] = 300;
    std::set<int> indices;
    unsigned longNames{};
    bool lateText{}, boundedBeforeLimit{};
    for (unsigned attempt{}; ; ++attempt) {
        require(attempt < 50U, "Byte-bounded accessibility pages did not terminate.");
        const auto page = fixture.execute("desktop_read", request);
        require(page.dump().size() <= 64U * 1024U, "Escaped accessibility text exceeded the canonical JSON byte bound.");
        const auto offset = page.at("offset").get<int>();
        const auto scanned = page.at("scanned_elements").get<int>();
        std::size_t textBytes{};
        for (const auto& element : page.at("elements")) {
            const auto index = element.at("index").get<int>();
            require(index >= offset && index < offset + scanned && indices.insert(index).second,
                "Byte-bounded paging repeated or skipped its observed index range.");
            const auto& name = element.at("name").get_ref<const std::string&>();
            textBytes += name.size();
            if (element.contains("text")) textBytes += element.at("text").get_ref<const std::string&>().size();
            const auto prefix = escapedLabels ? std::string(3500U, '\x01') : std::string{"\xe4\xba\x8c"};
            if (name.starts_with(prefix)) {
                ++longNames;
                require(take(Detail::strictUtf8ToUtf16(name)).size() > 0U,
                    "A bounded accessibility label split a UTF-8 character.");
                if (!escapedLabels) require(element.at("name_truncated") == true,
                    "An oversized Unicode accessibility name hid its per-control truncation.");
            }
            if (element.contains("text") && element.at("text") == "Late Unicode \xce\xa9\xe2\x82\xac after bounded text") lateText = true;
        }
        require(textBytes <= 32U * 1024U, "Accessibility paging exceeded its logical UTF-8 text budget.");
        if (!page.at("has_more").get<bool>()) break;
        boundedBeforeLimit = boundedBeforeLimit || scanned < 300;
        require(scanned > 0 && page.at("next_offset") == offset + scanned,
            "The result byte budget discarded an unread control or prevented forward progress.");
        request["offset"] = page.at("next_offset");
    }
    require(longNames == 40U && lateText && boundedBeforeLimit && ::GetForegroundWindow() == foreground,
        "Byte-bounded paging did not retrieve every escaped label and late Unicode value without activation.");
}
void ownedWindowInputReadAndCapture(const bool waitForExternalForeground = false) {
    Fixture fixture;
    OwnedWindow owned;
    auto identity = owned.identity();
    auto invalid = identity; invalid["x"] = 20; invalid["y"] = 30; invalid["button"] = "invalid";
    const auto initialForeground = ::GetForegroundWindow();
    requireError(fixture.invoke("desktop_click", invalid), Domain::ErrorCodes::InvalidRequest, "Invalid mouse button was accepted.");
    require(::GetForegroundWindow() == initialForeground, "Invalid input activated a window before validation.");
    invalid = identity; invalid["pid"] = static_cast<std::uint64_t>(::GetCurrentProcessId()) + 1U; invalid["text"] = "wrong pid";
    requireError(fixture.invoke("desktop_type", invalid), Domain::ErrorCodes::HostCapabilityUnavailable, "Stale PID was accepted.");
    fixture.reset(Domain::FileAccess::Read, {Domain::FileAccess::Read}, false);
    auto type = identity; type["text"] = "Unicode \xce\xa9\xe2\x82\xac";
    requireError(fixture.invoke("desktop_type", type), Domain::ErrorCodes::Unauthorized, "Read-only capability injected desktop input.");
    fixture.reset(Domain::FileAccess::Write, {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create, Domain::FileAccess::Execute}, true);
    if (waitForExternalForeground) {
        auto ready = identity;
        ready["title"] = "Forge Conductor owned desktop test";
        ready["foreground_wait_seconds"] = 120;
        std::cout << "OWNED_WINDOW_READY " << ready.dump() << '\n' << std::flush;
        const auto until = std::chrono::steady_clock::now() + 120s;
        while (!owned.inputReady() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(20ms);
        require(owned.inputReady(), "The owned window and edit control did not receive external foreground/focus within 120 seconds; no input was sent.");
    } else {
        owned.requestActivation();
        const auto until = std::chrono::steady_clock::now() + 3s;
        while (!owned.inputReady() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(20ms);
        require(owned.inputReady(), "Windows did not activate/focus the owned edit fixture; no input was sent.");
    }
    const auto accepted = fixture.execute("desktop_type", type);
    const std::wstring expected{L"Unicode \u03a9\u20ac"};
    const auto until = std::chrono::steady_clock::now() + 3s;
    while (owned.observedText() != expected && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(10ms);
    const auto actual = owned.observedText();
    const auto diagnostic = Json{{"actual_text", take(Detail::strictUtf16ToUtf8(actual))},
        {"focused_control", reinterpret_cast<std::uintptr_t>(owned.focusedControl())},
        {"foreground_window", reinterpret_cast<std::uintptr_t>(::GetForegroundWindow())}}.dump();
    require(accepted.at("requires_observation") == true && actual == expected,
        "Unicode input did not reach the owned edit window: " + diagnostic);
    auto key = identity; key["key"] = "End";
    fixture.execute("desktop_key", key);
    auto click = identity; click["x"] = 30; click["y"] = 50;
    fixture.execute("desktop_click", click);
    auto readRequest = identity; readRequest["limit"] = 10;
    const auto observed = fixture.execute("desktop_read", readRequest);
    require(observed.at("ok") == true, "Owned window accessibility read failed.");
    bool accessibleText{};
    for (const auto& element : observed.at("elements")) {
        if (element.contains("text")) accessibleText = accessibleText || element.at("text").get<std::string>().starts_with(type.at("text").get<std::string>());
    }
    require(accessibleText, "Accessibility read did not expose the owned edit's actual Unicode value.");
    auto capture = identity; capture["path"] = pathText(fixture.root / L"capture.png").value();
    const auto receipt = fixture.execute("desktop_capture", capture);
    const auto pixels = decode(read(fixture.root / L"capture.png"));
    require(receipt.at("capture_source") == "visible_desktop_window_region" && receipt.at("occlusion_possible") == true &&
        pixels.width > 0 && pixels.height > 0, "Desktop capture did not produce an honest valid PNG receipt.");
}
} // namespace
} // namespace ForgeConductor::Tests

int main(int argc, char** argv) {
    using namespace ForgeConductor::Tests;
    const auto apartment = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) { std::cerr << "FAIL COM initialization\n"; return 1; }
    int result{};
    try {
        imageCodecPixelsUnicodeAndOverwrite(); std::cout << "PASS image-codec-pixels-unicode-overwrite\n";
        rectangleDimensionsAndCanvasClipping(); std::cout << "PASS rectangle-dimensions-and-canvas-clipping\n";
        imagePreviewResolutionAndEncodedBound(); std::cout << "PASS image-preview-resolution-and-encoded-bound\n";
        imageReadCanonicalPixelsAlphaHashesAndSamples(); std::cout << "PASS image-read-canonical-pixels-alpha-hashes-samples\n";
        imageReadInvalidSamplesAndAuthorityHaveNoEffects(); std::cout << "PASS image-read-invalid-samples-authority-no-effects\n";
        permissionsInvalidAndCancelledHaveNoWriteEffects(); std::cout << "PASS image-authority-invalid-cancel\n";
        ownedWindowAccessibilityReadWithoutInput(); std::cout << "PASS owned-window-accessibility-read-without-input\n";
        ownedWindowCapturePreviewBound(); std::cout << "PASS owned-window-capture-preview-bound\n";
        ownedWindowAccessibilityPaging(); std::cout << "PASS owned-window-accessibility-paging\n";
        ownedWindowAccessibilityByteBoundPaging(true); std::cout << "PASS owned-window-accessibility-byte-bound-paging\n";
        ownedWindowAccessibilityByteBoundPaging(false); std::cout << "PASS owned-window-accessibility-unicode-text-bound-paging\n";
        desktopInventoryBoundsActualSerializedTitles(); std::cout << "PASS desktop-inventory-serialized-title-bound\n";
        if (argc == 2 && (std::string_view{argv[1]} == "--owned-window-input" ||
                std::string_view{argv[1]} == "--owned-window-input-wait")) {
            ownedWindowInputReadAndCapture(std::string_view{argv[1]} == "--owned-window-input-wait");
            std::cout << "PASS owned-window-input-read-capture\n";
        } else if (argc != 1) throw TestFailure{"Only --owned-window-input or --owned-window-input-wait is supported."};
    } catch (const std::exception& error) { std::cerr << "FAIL desktop artifact: " << error.what() << '\n'; result = 1; }
    ::CoUninitialize();
    return result;
}
