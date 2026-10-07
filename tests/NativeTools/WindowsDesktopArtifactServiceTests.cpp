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
#include <memory>
#include <mutex>
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
Decoded decode(std::vector<BYTE> bytes) {
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
    hr(converted->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom), "Generated PNG pixels could not convert.");
    decoded.pixels.resize(static_cast<std::size_t>(decoded.width) * decoded.height * 4U);
    hr(converted->CopyPixels(nullptr, decoded.width * 4U, static_cast<UINT>(decoded.pixels.size()), decoded.pixels.data()),
        "Generated PNG pixel decode was incomplete.");
    return decoded;
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
        std::wstring title = L"Forge Conductor owned desktop test")
        : worker_{[this, initialText = std::move(initialText), title = std::move(title)](std::stop_token stop) { run(stop, initialText, title); }} {
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
    void run(std::stop_token stop, const std::wstring& initialText, const std::wstring& title) {
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
        permissionsInvalidAndCancelledHaveNoWriteEffects(); std::cout << "PASS image-authority-invalid-cancel\n";
        ownedWindowAccessibilityReadWithoutInput(); std::cout << "PASS owned-window-accessibility-read-without-input\n";
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
