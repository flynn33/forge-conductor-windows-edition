#include "ForgeConductor/NativeTools/Windows/WindowsDesktopArtifactService.h"

#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Domain/ManagedRunModels.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "NativeFileOperations.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <Shellapi.h>
#include <winhttp.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_set>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {
namespace {
using Json = nlohmann::json;
using Microsoft::WRL::ComPtr;
namespace Conversion = ForgeConductor::Infrastructure::Windows::Detail;
constexpr auto MaximumImageBytes = Contracts::IDesktopArtifactService::MaximumImageBytes;
constexpr std::size_t MaximumDesktopInventoryBytes = 48U * 1024U;
constexpr std::size_t MaximumDesktopReadBytes = 64U * 1024U;

struct Failure final { Domain::Error error; };
[[noreturn]] void reject(std::string_view code, std::string message) {
    throw Failure{Domain::makeError(code, std::move(message))};
}
void check(const Domain::OperationContext& context) {
    if (context.isCancellationRequested()) reject(Domain::ErrorCodes::Cancelled, "Desktop/artifact operation cancelled.");
    if (context.isExpired(std::chrono::steady_clock::now())) reject(Domain::ErrorCodes::DeadlineExceeded, "Desktop/artifact deadline expired.");
}
void requireHr(HRESULT value, std::string_view operation) {
    if (FAILED(value)) reject(Domain::ErrorCodes::HostCapabilityUnavailable,
        std::string{operation} + " failed (HRESULT " + std::to_string(static_cast<std::uint32_t>(value)) + ").");
}
void requireImageHr(HRESULT value, std::string_view operation) {
    if (FAILED(value)) reject(Domain::ErrorCodes::InvalidRequest,
        std::string{operation} + " failed: the image bytes or encoding are invalid (HRESULT " +
        std::to_string(static_cast<std::uint32_t>(value)) + ").");
}
std::wstring wide(std::string_view value) {
    auto converted = Conversion::strictUtf8ToUtf16(value);
    if (!converted) throw Failure{converted.error()};
    return std::move(converted).value();
}
std::string utf8(std::wstring_view value) {
    auto converted = Conversion::strictUtf16ToUtf8(value);
    if (!converted) throw Failure{converted.error()};
    return std::move(converted).value();
}
std::string text(const Json& arguments, std::string_view key, std::size_t maximum = 65'536U) {
    const auto item = arguments.find(std::string{key});
    if (item == arguments.end() || !item->is_string()) reject(Domain::ErrorCodes::InvalidRequest, std::string{key} + " must be text.");
    auto result = item->get<std::string>();
    if (result.size() > maximum || result.find('\0') != std::string::npos || !Domain::isValidUtf8(result))
        reject(Domain::ErrorCodes::InvalidRequest, std::string{key} + " exceeds its UTF-8 bound or contains NUL.");
    return result;
}
std::string optionalText(const Json& arguments, std::string_view key, std::string_view fallback, std::size_t maximum = 65'536U) {
    return arguments.contains(std::string{key}) ? text(arguments, key, maximum) : std::string{fallback};
}
Json parseArguments(std::string_view serialized, const Domain::OperationContext& context) {
    if (serialized.size() > Contracts::IDesktopArtifactService::MaximumArgumentBytes)
        reject(Domain::ErrorCodes::PayloadTooLarge, "Desktop/artifact arguments exceed 2 MiB.");
    std::vector<std::unordered_set<std::string>> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        check(context);
        if (depth > 16) reject(Domain::ErrorCodes::LimitExceeded, "Desktop/artifact JSON nesting exceeds depth 16.");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            if (keys.empty() || !keys.back().insert(value.get<std::string>()).second)
                reject(Domain::ErrorCodes::InvalidRequest, "Desktop/artifact JSON contains a duplicate object key.");
        } else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    auto value = Json::parse(serialized, callback, true, false);
    if (!value.is_object()) reject(Domain::ErrorCodes::InvalidRequest, "Desktop/artifact arguments must be an object.");
    return value;
}
std::int64_t integer(const Json& arguments, std::string_view key, std::int64_t fallback,
    std::int64_t minimum, std::int64_t maximum) {
    const auto item = arguments.find(std::string{key});
    if (item == arguments.end()) return fallback;
    if (!item->is_number_integer() || (item->is_number_unsigned() &&
        item->get<std::uint64_t>() > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())))
        reject(Domain::ErrorCodes::InvalidRequest, std::string{key} + " must be a bounded integer.");
    const auto value = item->get<std::int64_t>();
    if (value < minimum || value > maximum) reject(Domain::ErrorCodes::InvalidRequest, std::string{key} + " is outside its bounds.");
    return value;
}
struct PixelSample final { UINT x{}; UINT y{}; };
std::optional<std::vector<PixelSample>> pixelSamples(const Json& arguments) {
    const auto item = arguments.find("samples");
    if (item == arguments.end()) return std::nullopt;
    if (!item->is_array() || item->empty() || item->size() > 64U)
        reject(Domain::ErrorCodes::InvalidRequest, "samples must contain between 1 and 64 pixel coordinates.");
    std::vector<PixelSample> samples;
    samples.reserve(item->size());
    for (const auto& sample : *item) {
        if (!sample.is_object() || sample.size() != 2U || !sample.contains("x") || !sample.contains("y"))
            reject(Domain::ErrorCodes::InvalidRequest, "Each pixel sample must contain exactly x and y.");
        samples.push_back({static_cast<UINT>(integer(sample, "x", 0, 0, 4095)),
            static_cast<UINT>(integer(sample, "y", 0, 0, 4095))});
    }
    return samples;
}
std::string digest(std::span<const std::byte> bytes, const Domain::OperationContext& context) {
    check(context);
    Infrastructure::Windows::BCryptSha256Hasher hasher;
    auto hashed = hasher.sha256(bytes);
    if (!hashed) throw Failure{hashed.error()};
    check(context);
    return hashed.value().value();
}
Json decodedPixelReceipt(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
    UINT width, UINT height, const std::optional<std::vector<PixelSample>>& samples,
    const Domain::OperationContext& context) {
    if (samples) for (const auto& sample : *samples) {
        if (sample.x >= width || sample.y >= height)
            reject(Domain::ErrorCodes::InvalidRequest, "Pixel sample coordinates must be inside decoded frame 0.");
    }
    ComPtr<IWICFormatConverter> converter;
    requireHr(factory->CreateFormatConverter(&converter), "Create RGBA8 pixel converter");
    requireImageHr(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
        nullptr, 0.0, WICBitmapPaletteTypeCustom), "Convert decoded frame to RGBA8");
    const UINT stride = width * 4U;
    std::vector<std::byte> rgba(static_cast<std::size_t>(stride) * height);
    for (UINT top = 0U; top < height; top += 64U) {
        check(context);
        const auto rows = (std::min)(64U, height - top);
        const WICRect region{0, static_cast<INT>(top), static_cast<INT>(width), static_cast<INT>(rows)};
        requireImageHr(converter->CopyPixels(&region, stride, rows * stride,
            reinterpret_cast<BYTE*>(rgba.data() + static_cast<std::size_t>(top) * stride)), "Read decoded RGBA8 pixels");
    }
    Json result{{"decoded_frame_index", 0}, {"decoded_pixel_format", "RGBA8"},
        {"decoded_width", width}, {"decoded_height", height}, {"decoded_row_stride_bytes", stride},
        {"decoded_rgba8_sha256", digest(rgba, context)}};
    if (samples) {
        auto values = Json::array();
        for (const auto& sample : *samples) {
            const auto offset = static_cast<std::size_t>(sample.y) * stride + sample.x * 4U;
            values.push_back(Json{{"x", sample.x}, {"y", sample.y}, {"rgba", Json::array({
                std::to_integer<unsigned>(rgba[offset]), std::to_integer<unsigned>(rgba[offset + 1U]),
                std::to_integer<unsigned>(rgba[offset + 2U]), std::to_integer<unsigned>(rgba[offset + 3U])})}});
        }
        result["pixel_samples"] = std::move(values);
    }
    return result;
}
class Apartment final {
public:
    Apartment() {
        const HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result != RPC_E_CHANGED_MODE) requireHr(result, "Initialize COM");
        initialized_ = SUCCEEDED(result);
    }
    ~Apartment() { if (initialized_) ::CoUninitialize(); }
private:
    bool initialized_{};
};
struct Surface final {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    void* pixels{};
    int width{};
    int height{};
    Surface(int requestedWidth, int requestedHeight) : width{requestedWidth}, height{requestedHeight} {
        dc = ::CreateCompatibleDC(nullptr);
        if (!dc) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Create image drawing context failed.");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !pixels) { if (bitmap) ::DeleteObject(bitmap); ::DeleteDC(dc); dc = nullptr; reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Create image bitmap failed."); }
        previous = ::SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) { ::DeleteObject(bitmap); ::DeleteDC(dc); dc = nullptr; reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Select image bitmap failed."); }
        std::memset(pixels, 0, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    ~Surface() { if (dc) { ::SelectObject(dc, previous); ::DeleteObject(bitmap); ::DeleteDC(dc); } }
};
COLORREF color(const std::string& value) {
    if (value.size() != 7U || value.front() != '#') reject(Domain::ErrorCodes::InvalidRequest, "Colors must use #RRGGBB.");
    unsigned encoded{};
    for (std::size_t index = 1; index < value.size(); ++index) {
        const char ch = value[index];
        const unsigned digit = ch >= '0' && ch <= '9' ? static_cast<unsigned>(ch - '0') :
            ch >= 'a' && ch <= 'f' ? static_cast<unsigned>(ch - 'a' + 10) :
            ch >= 'A' && ch <= 'F' ? static_cast<unsigned>(ch - 'A' + 10) : 16U;
        if (digit == 16U) reject(Domain::ErrorCodes::InvalidRequest, "Colors must use #RRGGBB.");
        encoded = encoded * 16U + digit;
    }
    return RGB((encoded >> 16U) & 255U, (encoded >> 8U) & 255U, encoded & 255U);
}
std::vector<std::byte> png(Surface& image) {
    ComPtr<IWICImagingFactory> factory;
    requireHr(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)), "Create image codec");
    ComPtr<IStream> stream;
    requireHr(::CreateStreamOnHGlobal(nullptr, TRUE, &stream), "Create image stream");
    ComPtr<IWICBitmapEncoder> encoder;
    requireHr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
    requireHr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    requireHr(encoder->CreateNewFrame(&frame, &options), "Create PNG frame");
    requireHr(frame->Initialize(options.Get()), "Initialize PNG frame");
    requireHr(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height)), "Set image dimensions");
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
    requireHr(frame->SetPixelFormat(&pixelFormat), "Set PNG pixel format");
    if (pixelFormat != GUID_WICPixelFormat24bppBGR)
        reject(Domain::ErrorCodes::HostCapabilityUnavailable, "PNG encoder did not retain the required 24-bit BGR format.");
    const auto stride = static_cast<UINT>(image.width * 4);
    ComPtr<IWICBitmap> bitmap;
    requireHr(factory->CreateBitmapFromMemory(static_cast<UINT>(image.width), static_cast<UINT>(image.height),
        GUID_WICPixelFormat32bppBGR, stride, stride * static_cast<UINT>(image.height),
        static_cast<BYTE*>(image.pixels), &bitmap), "Read drawing pixels");
    ComPtr<IWICFormatConverter> converter;
    requireHr(factory->CreateFormatConverter(&converter), "Create PNG pixel converter");
    requireHr(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
        nullptr, 0.0, WICBitmapPaletteTypeCustom), "Convert PNG pixels");
    requireHr(frame->WriteSource(converter.Get(), nullptr), "Encode PNG pixels");
    requireHr(frame->Commit(), "Commit PNG frame");
    requireHr(encoder->Commit(), "Commit PNG");
    STATSTG size{};
    requireHr(stream->Stat(&size, STATFLAG_NONAME), "Read image length");
    if (size.cbSize.QuadPart > MaximumImageBytes) reject(Domain::ErrorCodes::PayloadTooLarge, "Encoded image exceeds 16 MiB.");
    LARGE_INTEGER beginning{};
    requireHr(stream->Seek(beginning, STREAM_SEEK_SET, nullptr), "Read image stream");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.cbSize.QuadPart));
    ULONG read{};
    requireHr(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), "Read encoded image");
    if (read != bytes.size()) reject(Domain::ErrorCodes::IntegrityFailure, "PNG stream read was incomplete.");
    return bytes;
}
std::string base64(std::span<const std::byte> bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2U) / 3U * 4U);
    for (std::size_t index{}; index < bytes.size(); index += 3U) {
        const auto remaining = bytes.size() - index;
        const unsigned first = std::to_integer<unsigned char>(bytes[index]);
        const unsigned second = remaining > 1U ? std::to_integer<unsigned char>(bytes[index + 1U]) : 0U;
        const unsigned third = remaining > 2U ? std::to_integer<unsigned char>(bytes[index + 2U]) : 0U;
        result += alphabet[first >> 2U];
        result += alphabet[((first & 3U) << 4U) | (second >> 4U)];
        result += remaining > 1U ? alphabet[((second & 15U) << 2U) | (third >> 6U)] : '=';
        result += remaining > 2U ? alphabet[third & 63U] : '=';
    }
    return result;
}
Json preview(Surface& image, const int maximumDimension, const Domain::OperationContext& context, bool includePngDigest = false) {
    double scale = (std::min)(1.0, static_cast<double>(maximumDimension) /
        static_cast<double>((std::max)(image.width, image.height)));
    bool reducedForBytes{};
    for (;;) {
        check(context);
        const int width = (std::max)(1, static_cast<int>(image.width * scale));
        const int height = (std::max)(1, static_cast<int>(image.height * scale));
        Surface thumbnail{width, height};
        ::SetStretchBltMode(thumbnail.dc, HALFTONE);
        if (!::StretchBlt(thumbnail.dc, 0, 0, width, height, image.dc, 0, 0, image.width, image.height, SRCCOPY))
            reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Create image preview failed.");
        const auto bytes = png(thumbnail);
        const auto encodedBytes = (bytes.size() + 2U) / 3U * 4U;
        if (encodedBytes <= Domain::MaximumManagedImagePreviewBase64Bytes) {
            Json result{{"image_base64", base64(bytes)}, {"image_mime_type", "image/png"},
                {"preview_width", width}, {"preview_height", height},
                {"preview_max_dimension_requested", maximumDimension}, {"preview_encoded_bytes", encodedBytes},
                {"preview_encoded_byte_limit", Domain::MaximumManagedImagePreviewBase64Bytes},
                {"preview_reduced_for_byte_limit", reducedForBytes}};
            if (includePngDigest) result["preview_png_sha256"] = digest(bytes, context);
            return result;
        }
        if (width == 1 && height == 1)
            reject(Domain::ErrorCodes::PayloadTooLarge, "The image preview cannot fit its encoded transport byte limit.");
        reducedForBytes = true;
        scale *= 0.75;
    }
}
HWND window(const Json& arguments) {
    if (!arguments.contains("window_id") || !arguments.contains("pid"))
        reject(Domain::ErrorCodes::InvalidRequest, "Desktop operations require window_id and pid from desktop_list.");
    const auto handle = integer(arguments, "window_id", 0, 1, (std::numeric_limits<std::int64_t>::max)());
    const auto pid = integer(arguments, "pid", 0, 1, MAXDWORD);
    const HWND selected = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    DWORD actual{};
    if (!::IsWindow(selected) || ::GetAncestor(selected, GA_ROOT) != selected || !::IsWindowVisible(selected) || ::GetWindowThreadProcessId(selected, &actual) == 0 ||
        actual != static_cast<DWORD>(pid)) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "The selected visible window/PID is no longer available; refresh desktop_list.");
    return selected;
}
RECT bounds(HWND selected) {
    RECT result{};
    if (!::GetWindowRect(selected, &result) || result.right <= result.left || result.bottom <= result.top)
        reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Window geometry is unavailable.");
    return result;
}
void foreground(HWND selected, const Json& arguments, const Domain::OperationContext& context) {
    check(context);
    if (window(arguments) != selected) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "The selected window changed before activation.");
    if (::GetForegroundWindow() != selected) {
        if (!::SetForegroundWindow(selected) && ::GetForegroundWindow() != selected)
            reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Windows refused to activate the selected window (SetForegroundWindow returned zero); no input was sent.");
        // A different input queue processes activation asynchronously. Confirm
        // completion without attaching our queue or changing Windows focus policy.
        check(context);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            context.deadline - std::chrono::steady_clock::now()).count();
        const auto timeout = static_cast<UINT>((std::max)(1LL, (std::min)(1000LL, static_cast<long long>(remaining))));
        DWORD_PTR response{};
        if (!::SendMessageTimeoutW(selected, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, timeout, &response)) {
            check(context);
            reject(Domain::ErrorCodes::HostCapabilityUnavailable, "The selected window did not confirm activation within the bounded wait; no input was sent.");
        }
    }
    check(context);
    if (window(arguments) != selected || ::GetForegroundWindow() != selected)
        reject(Domain::ErrorCodes::HostCapabilityUnavailable, "The selected window did not retain its identity and foreground focus after activation; no input was sent.");
}
void verifyInputTarget(HWND selected, const Json& arguments, const Domain::OperationContext& context) {
    check(context);
    if (window(arguments) != selected || ::GetForegroundWindow() != selected)
        reject(Domain::ErrorCodes::HostCapabilityUnavailable, "The selected window lost its identity or foreground focus; input may be partial. Observe before retrying.");
}
Json listWindows(const Json& arguments, const Domain::OperationContext& context) {
    struct State {
        Json windows{Json::array()}; std::size_t limit; bool truncated{};
        const Domain::OperationContext& context;
        std::size_t serializedBytes{Json{{"ok", true}, {"windows", Json::array()}, {"truncated", false}}.dump().size()};
    };
    State state{Json::array(), static_cast<std::size_t>(integer(arguments, "limit", 100, 1, 500)), false, context};
    const auto enumerated = ::EnumWindows([](HWND selected, LPARAM parameter) -> BOOL {
        auto& current = *reinterpret_cast<State*>(parameter);
        if (current.context.isCancellationRequested() || current.context.isExpired(std::chrono::steady_clock::now())) return FALSE;
        if (!::IsWindowVisible(selected)) return TRUE;
        const int length = ::GetWindowTextLengthW(selected);
        if (length <= 0) return TRUE;
        if (current.windows.size() >= current.limit) { current.truncated = true; return FALSE; }
        std::wstring title(static_cast<std::size_t>((std::min)(length, 4096)) + 1U, L'\0');
        const int copied = ::GetWindowTextW(selected, title.data(), static_cast<int>(title.size()));
        title.resize(static_cast<std::size_t>((std::max)(copied, 0)));
        DWORD pid{};
        ::GetWindowThreadProcessId(selected, &pid);
        RECT geometry{};
        ::GetWindowRect(selected, &geometry);
        try {
            Json row{{"window_id", reinterpret_cast<std::uintptr_t>(selected)},
            {"pid", pid}, {"title", utf8(title)}, {"x", geometry.left}, {"y", geometry.top},
            {"width", geometry.right - geometry.left}, {"height", geometry.bottom - geometry.top}};
            const auto bytes = row.dump().size() + (current.windows.empty() ? 0U : 1U);
            if (bytes > MaximumDesktopInventoryBytes - current.serializedBytes) {
                current.truncated = true;
                return FALSE;
            }
            current.serializedBytes += bytes;
            current.windows.push_back(std::move(row));
        }
        catch (...) { current.truncated = true; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    check(context);
    if (!enumerated && !state.truncated) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Available desktop windows could not be enumerated.");
    return Json{{"ok", true}, {"windows", std::move(state.windows)}, {"truncated", state.truncated}};
}
std::pair<std::string, bool> boundedAccessibilityText(BSTR value) {
    if (!value) return {{}, false};
    const auto length = ::SysStringLen(value);
    auto units = (std::min)(length, 4096U);
    if (units < length && units > 0U && value[units - 1U] >= 0xd800 && value[units - 1U] <= 0xdbff) --units;
    auto result = utf8(std::wstring_view{value, units});
    const bool truncated = length > units || result.size() > 4096U;
    if (result.size() > 4096U) result.resize(4096U);
    while (!result.empty() && !Domain::isValidUtf8(result)) result.pop_back();
    return {std::move(result), truncated};
}
Json controlText(IUIAutomationElement& element) {
    BOOL password{};
    if (FAILED(element.get_CurrentIsPassword(&password)) || password != FALSE) return Json::object();
    BSTR value{};
    HRESULT read = E_NOINTERFACE;
    std::string source;
    ComPtr<IUIAutomationTextPattern> textPattern;
    if (SUCCEEDED(element.GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&textPattern))) && textPattern) {
        ComPtr<IUIAutomationTextRange> range;
        if (SUCCEEDED(textPattern->get_DocumentRange(&range)) && range) read = range->GetText(4096, &value);
        source = "text_pattern";
    }
    if (FAILED(read)) {
        ::SysFreeString(value); value = nullptr;
        ComPtr<IUIAutomationValuePattern> valuePattern;
        if (SUCCEEDED(element.GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&valuePattern))) && valuePattern)
            read = valuePattern->get_CurrentValue(&value);
        source = "value_pattern";
    }
    const std::unique_ptr<wchar_t, decltype(&::SysFreeString)> ownedValue{value, &::SysFreeString};
    if (FAILED(read)) return Json::object();
    auto [content, truncated] = boundedAccessibilityText(value);
    return Json{{"text", std::move(content)}, {"text_source", source},
        {"text_truncated", truncated || (source == "text_pattern" && value && ::SysStringLen(value) == 4096U)}};
}
Json readWindow(HWND selected, const Json& arguments, const Domain::OperationContext& context) {
    const int limit = static_cast<int>(integer(arguments, "limit", 100, 1, 300));
    const int offset = static_cast<int>(integer(arguments, "offset", 0, 0, (std::numeric_limits<int>::max)()));
    ComPtr<IUIAutomation> automation;
    requireHr(::CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation)), "Create desktop accessibility client");
    ComPtr<IUIAutomation2> bounded;
    requireHr(automation.As(&bounded), "Require bounded accessibility client");
    requireHr(bounded->put_ConnectionTimeout(1000U), "Bound accessibility connection");
    requireHr(bounded->put_TransactionTimeout(1000U), "Bound accessibility transaction");
    ComPtr<IUIAutomationElement> root;
    requireHr(automation->ElementFromHandle(selected, &root), "Read selected accessibility window");
    ComPtr<IUIAutomationCondition> condition;
    requireHr(automation->CreateTrueCondition(&condition), "Create accessibility condition");
    ComPtr<IUIAutomationElementArray> elements;
    requireHr(root->FindAll(TreeScope_Descendants, condition.Get(), &elements), "Read window controls");
    int count{};
    requireHr(elements->get_Length(&count), "Read control count");
    if (offset > count) reject(Domain::ErrorCodes::InvalidRequest, "offset exceeds the current accessibility control count.");
    const auto geometry = bounds(selected);
    Json items = Json::array();
    std::size_t totalText{};
    int index = offset;
    const int end = offset + (std::min)(count - offset, limit);
    const auto result = [&](const int next) {
        const bool hasMore = next < count;
        return Json{{"ok", true}, {"elements", items}, {"total_elements", count}, {"offset", offset},
            {"scanned_elements", next - offset}, {"returned_elements", items.size()},
            {"next_offset", hasMore ? Json(next) : Json(nullptr)}, {"has_more", hasMore}, {"truncated", hasMore}};
    };
    while (index < end) {
        check(context);
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(index, &element))) {
            if (result(index + 1).dump().size() > MaximumDesktopReadBytes) break;
            ++index; continue;
        }
        BSTR rawName{};
        if (FAILED(element->get_CurrentName(&rawName))) {
            if (result(index + 1).dump().size() > MaximumDesktopReadBytes) break;
            ++index; continue;
        }
        const std::unique_ptr<wchar_t, decltype(&::SysFreeString)> ownedName{rawName, &::SysFreeString};
        auto [label, labelTruncated] = boundedAccessibilityText(rawName);
        CONTROLTYPEID type{};
        RECT position{};
        BOOL enabled{}, offscreen{};
        element->get_CurrentControlType(&type);
        element->get_CurrentBoundingRectangle(&position);
        element->get_CurrentIsEnabled(&enabled);
        element->get_CurrentIsOffscreen(&offscreen);
        auto value = controlText(*element.Get());
        const auto textBytes = label.size() + (value.contains("text") ? value.at("text").get_ref<const std::string&>().size() : 0U);
        if (textBytes > 32U * 1024U - totalText) break;
        value.update(Json{{"index", index}, {"name", label}, {"name_truncated", labelTruncated}, {"control_type", type}, {"enabled", enabled != FALSE},
            {"offscreen", offscreen != FALSE}, {"x", position.left - geometry.left},
            {"y", position.top - geometry.top}, {"width", position.right - position.left},
            {"height", position.bottom - position.top}});
        items.push_back(std::move(value));
        if (result(index + 1).dump().size() > MaximumDesktopReadBytes) {
            items.erase(items.size() - 1U);
            break;
        }
        totalText += textBytes;
        ++index;
    }
    return result(index);
}
void send(std::span<INPUT> inputs) {
    if (inputs.size() > MAXDWORD || ::SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) != inputs.size())
        reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Windows input injection failed; outcome may be partial. Observe before retrying.");
}
Json clickWindow(HWND selected, const Json& arguments, const Domain::OperationContext& context) {
    const auto geometry = bounds(selected);
    const auto x = integer(arguments, "x", -1, 0, geometry.right - geometry.left - 1);
    const auto y = integer(arguments, "y", -1, 0, geometry.bottom - geometry.top - 1);
    if (x < 0 || y < 0) reject(Domain::ErrorCodes::InvalidRequest, "Click requires window-relative x and y.");
    const auto button = optionalText(arguments, "button", "left", 16U);
    if (button != "left" && button != "right") reject(Domain::ErrorCodes::InvalidRequest, "Mouse button must be left or right.");
    foreground(selected, arguments, context);
    POINT point{geometry.left + static_cast<LONG>(x), geometry.top + static_cast<LONG>(y)};
    const auto atPoint = ::WindowFromPoint(point);
    if (atPoint != selected && !::IsChild(selected, atPoint))
        reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Another window covers the selected click point.");
    const auto screenWidth = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const auto screenHeight = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (screenWidth <= 1 || screenHeight <= 1) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Desktop coordinate space unavailable.");
    std::array<INPUT, 3> inputs{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = static_cast<LONG>((static_cast<std::int64_t>(point.x - ::GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535) / (screenWidth - 1));
    inputs[0].mi.dy = static_cast<LONG>((static_cast<std::int64_t>(point.y - ::GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535) / (screenHeight - 1));
    inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    inputs[1].type = inputs[2].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = button == "left" ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
    inputs[2].mi.dwFlags = button == "left" ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
    verifyInputTarget(selected, arguments, context);
    send(inputs);
    return Json{{"ok", true}, {"input_submitted", true}, {"requires_observation", true}};
}
Json typeWindow(HWND selected, const Json& arguments, const Domain::OperationContext& context) {
    const auto value = wide(text(arguments, "text"));
    foreground(selected, arguments, context);
    for (const auto ch : value) {
        verifyInputTarget(selected, arguments, context);
        std::array<INPUT, 2> inputs{};
        inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = inputs[1].ki.wScan = ch;
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        send(inputs);
    }
    return Json{{"ok", true}, {"utf16_units_submitted", value.size()}, {"requires_observation", true}};
}
WORD keyCode(const std::string& name) {
    if (name.size() == 1U && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        return static_cast<WORD>(name[0] >= 'a' && name[0] <= 'z' ? name[0] - 'a' + 'A' : name[0]);
    for (const auto& [label, code] : std::array<std::pair<std::string_view, WORD>, 13>{{
        {"Enter", VK_RETURN}, {"Tab", VK_TAB}, {"Escape", VK_ESCAPE}, {"Backspace", VK_BACK},
        {"Delete", VK_DELETE}, {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"Up", VK_UP}, {"Down", VK_DOWN},
        {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}}})
        if (name == label) return code;
    reject(Domain::ErrorCodes::InvalidRequest, "Unsupported desktop key; use letters, digits, Enter, Tab, Escape, arrows, Home, End, PageUp, PageDown, Backspace or Delete.");
}
Json pressWindow(HWND selected, const Json& arguments, const Domain::OperationContext& context) {
    const auto key = keyCode(text(arguments, "key", 32U));
    std::vector<WORD> modifiers;
    for (const auto& [label, code] : std::array<std::pair<std::string_view, WORD>, 3>{{{"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"shift", VK_SHIFT}}}) {
        if (arguments.contains(label) && !arguments.at(label).is_boolean()) reject(Domain::ErrorCodes::InvalidRequest, "Key modifiers must be booleans.");
        if (arguments.value(std::string{label}, false)) modifiers.push_back(code);
    }
    std::vector<INPUT> inputs;
    auto append = [&inputs](WORD code, bool release) { INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wVk = code; input.ki.dwFlags = release ? KEYEVENTF_KEYUP : 0U; inputs.push_back(input); };
    for (const auto modifier : modifiers) append(modifier, false);
    append(key, false); append(key, true);
    for (auto index = modifiers.rbegin(); index != modifiers.rend(); ++index) append(*index, true);
    foreground(selected, arguments, context);
    verifyInputTarget(selected, arguments, context);
    send(inputs);
    return Json{{"ok", true}, {"input_submitted", true}, {"requires_observation", true}};
}
void render(Surface& image, const Json& arguments, const Domain::OperationContext& context) {
    const auto background = color(optionalText(arguments, "background", "#FFFFFF", 7U));
    HBRUSH brush = ::CreateSolidBrush(background);
    if (!brush) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Create drawing brush failed.");
    RECT all{0, 0, image.width, image.height};
    const int filled = ::FillRect(image.dc, &all, brush);
    ::DeleteObject(brush);
    if (!filled) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Fill image background failed.");
    if (!arguments.contains("elements") || !arguments.at("elements").is_array() || arguments.at("elements").size() > 1000U)
        reject(Domain::ErrorCodes::InvalidRequest, "Image elements must be an array of at most 1000 shapes.");
    ::SetBkMode(image.dc, TRANSPARENT);
    for (const auto& element : arguments.at("elements")) {
        check(context);
        if (!element.is_object()) reject(Domain::ErrorCodes::InvalidRequest, "Each image element must be an object.");
        const auto type = text(element, "type", 32U);
        const int x = static_cast<int>(integer(element, "x", 0, -8192, 8192));
        const int y = static_cast<int>(integer(element, "y", 0, -8192, 8192));
        const auto fill = color(optionalText(element, "color", "#000000", 7U));
        if (type == "text") {
            const auto value = wide(text(element, "text", 16'384U));
            const int size = static_cast<int>(integer(element, "size", 24, 6, 256));
            HFONT font = ::CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            if (!font) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Create image font failed.");
            const auto old = ::SelectObject(image.dc, font);
            if (!old || old == HGDI_ERROR) { ::DeleteObject(font); reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Select image font failed."); }
            const BOOL drawn = ::SetTextColor(image.dc, fill) != CLR_INVALID &&
                ::TextOutW(image.dc, x, y, value.data(), static_cast<int>(value.size()));
            ::SelectObject(image.dc, old); ::DeleteObject(font);
            if (!drawn) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Draw image text failed.");
        } else if (type == "rectangle" || type == "ellipse") {
            const int width = static_cast<int>(integer(element, "width", 1, 1, 8192));
            const int height = static_cast<int>(integer(element, "height", 1, 1, 8192));
            HBRUSH shape = ::CreateSolidBrush(fill);
            if (!shape) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Create shape brush failed.");
            const auto oldBrush = ::SelectObject(image.dc, shape);
            if (!oldBrush || oldBrush == HGDI_ERROR) { ::DeleteObject(shape); reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Select shape brush failed."); }
            const auto oldPen = ::SelectObject(image.dc, ::GetStockObject(NULL_PEN));
            if (!oldPen || oldPen == HGDI_ERROR) { ::SelectObject(image.dc, oldBrush); ::DeleteObject(shape); reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Select shape pen failed."); }
            const RECT bounds{x, y, x + width, y + height};
            const BOOL drawn = type == "rectangle" ? ::FillRect(image.dc, &bounds, shape) != 0 : ::Ellipse(image.dc, x, y, x + width, y + height);
            ::SelectObject(image.dc, oldBrush); ::SelectObject(image.dc, oldPen); ::DeleteObject(shape);
            if (!drawn) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Draw image shape failed.");
        } else if (type == "line") {
            const int x2 = static_cast<int>(integer(element, "x2", x, -8192, 8192));
            const int y2 = static_cast<int>(integer(element, "y2", y, -8192, 8192));
            HPEN pen = ::CreatePen(PS_SOLID, static_cast<int>(integer(element, "stroke_width", 2, 1, 128)), fill);
            if (!pen) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Create line pen failed.");
            const auto old = ::SelectObject(image.dc, pen);
            if (!old || old == HGDI_ERROR) { ::DeleteObject(pen); reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Select line pen failed."); }
            const BOOL drawn = ::MoveToEx(image.dc, x, y, nullptr) && ::LineTo(image.dc, x2, y2);
            ::SelectObject(image.dc, old); ::DeleteObject(pen);
            if (!drawn) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Draw image line failed.");
        } else reject(Domain::ErrorCodes::InvalidRequest, "Image element type must be rectangle, ellipse, line or text.");
    }
}
Contracts::AuthorizedPath destination(Contracts::IWorkspaceAuthority& resolver,
    const Contracts::WorkspaceAuthority& authority, const Domain::PathText& requested,
    const Domain::OperationContext& context) {
    auto writable = resolver.authorize(authority, {requested, std::nullopt, Domain::FileAccess::Write, true}, context);
    if (!writable) {
        auto created = resolver.authorize(authority, {requested, std::nullopt, Domain::FileAccess::Create, true}, context);
        if (!created) throw Failure{created.error()};
        return std::move(created).value();
    }
    auto existing = Detail::openAuthorizedObject(writable.value(), Domain::FileAccess::Write,
        Conversion::MissingPathPolicy::Reject, context, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (existing) {
        if (existing.value().isDirectory()) reject(Domain::ErrorCodes::InvalidRequest, "Image destination must name a regular file.");
        return std::move(writable).value();
    }
    if (existing.error().code != Domain::ErrorCodes::RecordNotFound) throw Failure{existing.error()};
    auto created = resolver.authorize(authority, {requested, std::nullopt, Domain::FileAccess::Create, true}, context);
    if (!created) throw Failure{created.error()};
    return std::move(created).value();
}
Domain::PathText requestedImagePath(Contracts::IWorkspaceAuthority& resolver,
    const Contracts::WorkspaceAuthority& authority, const Json& arguments, const Domain::OperationContext& context) {
    auto path = text(arguments, "path", Domain::PathText::MaximumBytes);
    if (!std::filesystem::path{wide(path)}.is_absolute()) {
        auto base = resolver.defaultWorkspacePath(authority, context);
        if (!base) throw Failure{base.error()};
        path = base.value().value() + "\\" + path;
    }
    auto requested = Domain::PathText::create(path);
    if (!requested) throw Failure{requested.error()};
    return std::move(requested).value();
}
void authorizeDesktop(Contracts::IWorkspaceAuthority& resolver, const Contracts::WorkspaceAuthority& authority,
    bool input, const Domain::OperationContext& context) {
    if (authority.trustedRoots().empty()) reject(Domain::ErrorCodes::Unauthorized, "Desktop tools require a current workspace authority.");
    if (input && !authority.shellEnabled()) reject(Domain::ErrorCodes::Unauthorized, "Desktop input and browser launch require the owner's Execute permission.");
    auto authorized = resolver.authorize(authority,
        {authority.trustedRoots().front(), std::nullopt, input ? Domain::FileAccess::Execute : Domain::FileAccess::Read, false}, context);
    if (!authorized) throw Failure{authorized.error()};
}
} // namespace

WindowsDesktopArtifactService::WindowsDesktopArtifactService(
    Contracts::IWorkspaceAuthority& workspaceAuthority, Contracts::IAtomicFileStore& files) noexcept
    : workspaceAuthority_{workspaceAuthority}, files_{files} {}

Domain::Result<std::string> WindowsDesktopArtifactService::execute(
    std::string_view toolName, std::string_view serializedArguments,
    const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& context) noexcept {
    try {
        check(context);
        const auto arguments = parseArguments(serializedArguments, context);
        Apartment apartment;
        Json result = Json::object();
        if (toolName == "desktop_list") {
            authorizeDesktop(workspaceAuthority_, authority, false, context);
            result = listWindows(arguments, context);
        }
        else if (toolName == "browser_open") {
            const auto url = text(arguments, "url", 16'384U);
            if ((!url.starts_with("https://") && !url.starts_with("http://")) || url.find_first_of("\r\n") != std::string::npos)
                reject(Domain::ErrorCodes::InvalidRequest, "Browser URL must be an HTTP or HTTPS address.");
            const auto native = wide(url);
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            parts.dwHostNameLength = static_cast<DWORD>(-1);
            if (!::WinHttpCrackUrl(native.c_str(), static_cast<DWORD>(native.size()), 0, &parts) || parts.dwHostNameLength == 0U ||
                (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS))
                reject(Domain::ErrorCodes::InvalidRequest, "Browser URL must have a valid HTTP or HTTPS host.");
            authorizeDesktop(workspaceAuthority_, authority, true, context);
            check(context);
            SHELLEXECUTEINFOW launch{};
            launch.cbSize = sizeof(launch);
            launch.fMask = SEE_MASK_FLAG_NO_UI;
            launch.lpVerb = L"open";
            launch.lpFile = native.c_str();
            launch.nShow = SW_SHOWNORMAL;
            if (!::ShellExecuteExW(&launch)) reject(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Windows could not open the URL with its registered browser.");
            result = Json{{"ok", true}, {"url", url}, {"launch_accepted", true}, {"navigation_verified", false}};
        } else if (toolName == "desktop_read" || toolName == "desktop_click" ||
            toolName == "desktop_type" || toolName == "desktop_key") {
            const HWND selected = window(arguments);
            authorizeDesktop(workspaceAuthority_, authority, toolName != "desktop_read", context);
            if (toolName == "desktop_read") result = readWindow(selected, arguments, context);
            else if (toolName == "desktop_click") result = clickWindow(selected, arguments, context);
            else if (toolName == "desktop_type") result = typeWindow(selected, arguments, context);
            else result = pressWindow(selected, arguments, context);
        } else if (toolName == "desktop_capture" || toolName == "image_write" || toolName == "image_read") {
            const auto previewDimension = static_cast<int>(integer(arguments, "preview_max_dimension", 256, 128, 2048));
            const bool read = toolName == "image_read";
            const auto samples = read ? pixelSamples(arguments) : std::nullopt;
            const auto requested = requestedImagePath(workspaceAuthority_, authority, arguments, context);
            auto authorized = read ? workspaceAuthority_.authorize(authority,
                {requested, std::nullopt, Domain::FileAccess::Read, false}, context) :
                Domain::Result<Contracts::AuthorizedPath>::success(destination(workspaceAuthority_, authority, requested, context));
            if (!authorized) throw Failure{authorized.error()};
            std::unique_ptr<Surface> image;
            std::string imageFormat{"png"};
            if (read) {
                auto bytes = files_.read(authorized.value(), MaximumImageBytes, context);
                if (!bytes) throw Failure{bytes.error()};
                ComPtr<IWICImagingFactory> factory;
                requireHr(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&factory)), "Create image decoder");
                ComPtr<IWICStream> stream;
                requireHr(factory->CreateStream(&stream), "Create image input stream");
                requireImageHr(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(bytes.value().data()),
                    static_cast<DWORD>(bytes.value().size())), "Load image bytes");
                ComPtr<IWICBitmapDecoder> decoder;
                requireImageHr(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder), "Decode image");
                GUID container{};
                requireImageHr(decoder->GetContainerFormat(&container), "Read image format");
                imageFormat = container == GUID_ContainerFormatPng ? "png" :
                    container == GUID_ContainerFormatJpeg ? "jpeg" :
                    container == GUID_ContainerFormatGif ? "gif" :
                    container == GUID_ContainerFormatBmp ? "bmp" :
                    container == GUID_ContainerFormatTiff ? "tiff" :
                    container == GUID_ContainerFormatIco ? "ico" : "wic_supported_image";
                ComPtr<IWICBitmapFrameDecode> frame;
                requireImageHr(decoder->GetFrame(0, &frame), "Read image frame");
                UINT width{}, height{};
                requireImageHr(frame->GetSize(&width, &height), "Read image dimensions");
                if (width == 0 || height == 0 || width > 4096U || height > 4096U)
                    reject(Domain::ErrorCodes::PayloadTooLarge, "Image dimensions must be at most 4096 by 4096.");
                result.update(decodedPixelReceipt(factory.Get(), frame.Get(), width, height, samples, context));
                image = std::make_unique<Surface>(static_cast<int>(width), static_cast<int>(height));
                ComPtr<IWICFormatConverter> converter;
                requireHr(factory->CreateFormatConverter(&converter), "Create image pixel converter");
                requireImageHr(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone,
                    nullptr, 0.0, WICBitmapPaletteTypeCustom), "Convert image pixels");
                requireImageHr(converter->CopyPixels(nullptr, width * 4U, width * height * 4U,
                    static_cast<BYTE*>(image->pixels)), "Read image pixels");
            } else {
                if (toolName == "desktop_capture") {
                    const HWND selected = window(arguments);
                    authorizeDesktop(workspaceAuthority_, authority, false, context);
                    if (::IsIconic(selected)) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "A minimized window has no visible desktop region to capture.");
                    const auto geometry = bounds(selected);
                    const int width = geometry.right - geometry.left;
                    const int height = geometry.bottom - geometry.top;
                    if (width > 4096 || height > 4096) reject(Domain::ErrorCodes::PayloadTooLarge, "Selected window exceeds capture dimension bounds.");
                    image = std::make_unique<Surface>(width, height);
                    HDC desktop = ::GetDC(nullptr);
                    if (!desktop) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Desktop capture context is unavailable.");
                    const BOOL captured = ::BitBlt(image->dc, 0, 0, width, height, desktop,
                        geometry.left, geometry.top, SRCCOPY | CAPTUREBLT);
                    ::ReleaseDC(nullptr, desktop);
                    if (!captured) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Capture the visible desktop window region failed.");
                    result["capture_source"] = "visible_desktop_window_region";
                    result["occlusion_possible"] = true;
                } else {
                    image = std::make_unique<Surface>(
                        static_cast<int>(integer(arguments, "width", 1024, 1, 4096)),
                        static_cast<int>(integer(arguments, "height", 768, 1, 4096)));
                    render(*image, arguments, context);
                }
                const auto bytes = png(*image);
                result.update(preview(*image, previewDimension, context));
                check(context);
                auto parents = Detail::ensureAuthorizedParentDirectories(authorized.value(), context);
                if (!parents) throw Failure{parents.error()};
                auto written = files_.replace(authorized.value(), bytes, false, context);
                if (!written) throw Failure{written.error()};
                result["bytes_written"] = bytes.size();
            }
            if (result.is_null()) result = Json::object();
            if (read) result.update(preview(*image, previewDimension, context, true));
            result["ok"] = true;
            result["path"] = authorized.value().canonicalPath().value();
            result["width"] = image->width;
            result["height"] = image->height;
            result["format"] = imageFormat;
            result["engine"] = "forge-native-wic-gdi";
        } else reject(Domain::ErrorCodes::InvalidRequest, "Unknown desktop/artifact tool.");
        check(context);
        return Domain::Result<std::string>::success(result.dump());
    } catch (const Failure& failure) { return Domain::Result<std::string>::failure(failure.error); }
    catch (const Json::exception& error) { return Domain::Result<std::string>::failure(
        Domain::makeError(Domain::ErrorCodes::InvalidRequest, std::string{"Invalid desktop/artifact arguments: "} + error.what())); }
    catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure, "Desktop/artifact operation failed.")); }
}
} // namespace ForgeConductor::NativeTools::Windows
