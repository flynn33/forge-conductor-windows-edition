#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioChatControl.h"

#include "Detail/UtfConversion.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioConversationReader.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <UIAutomation.h>

#include "Detail/UniqueHandle.h"
#include "Detail/LMStudioNativeChatFile.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Clock = std::chrono::steady_clock;
using Detail::readNativeChatFile;
using Detail::sameRevision;

[[nodiscard]] Domain::Error capabilityError(std::string message)
{
    return Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
                             std::move(message), true);
}

[[nodiscard]] Domain::Error comError(const std::string_view action, const HRESULT result)
{
    return capabilityError(std::string{action} + " failed (HRESULT " +
        std::to_string(static_cast<std::uint32_t>(result)) + ").");
}

[[nodiscard]] std::optional<Domain::Error> interrupted(
    const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) {
        return Domain::makeError(Domain::ErrorCodes::Cancelled,
                                 "LM Studio chat control was cancelled.");
    }
    if (context.isExpired(Clock::now())) {
        return Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,
                                 "LM Studio chat control deadline expired.");
    }
    return std::nullopt;
}

template <typename Interface>
class ComReference final {
public:
    ComReference() noexcept = default;
    ~ComReference() noexcept { reset(); }
    ComReference(const ComReference&) = delete;
    ComReference& operator=(const ComReference&) = delete;
    ComReference(ComReference&& other) noexcept
        : value_{std::exchange(other.value_, nullptr)} {}
    ComReference& operator=(ComReference&& other) noexcept
    {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] Interface* get() const noexcept { return value_; }
    [[nodiscard]] Interface* operator->() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
    [[nodiscard]] Interface** put() noexcept
    {
        reset();
        return &value_;
    }
private:
    void reset() noexcept
    {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
    }
    Interface* value_{};
};

class Automation final {
public:
    ~Automation() noexcept
    {
        client_ = {};
        if (initialized_) {
            ::CoUninitialize();
        }
    }
    [[nodiscard]] Domain::Result<void> initialize()
    {
        const HRESULT apartment = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
            return Domain::Result<void>::failure(comError("UI Automation initialization", apartment));
        }
        initialized_ = SUCCEEDED(apartment);
        const HRESULT result = ::CoCreateInstance(CLSID_CUIAutomation, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(client_.put()));
        if (FAILED(result)) {
            return Domain::Result<void>::failure(comError("Creating UI Automation", result));
        }
        ComReference<IUIAutomation2> bounded;
        if (SUCCEEDED(client_->QueryInterface(IID_PPV_ARGS(bounded.put()))) && bounded) {
            const HRESULT connection = bounded->put_ConnectionTimeout(2000U);
            const HRESULT transaction = bounded->put_TransactionTimeout(2000U);
            if (FAILED(connection) || FAILED(transaction)) {
                return Domain::Result<void>::failure(comError("Bounding UI Automation calls",
                    FAILED(connection) ? connection : transaction));
            }
        }
        return Domain::Result<void>::success();
    }
    [[nodiscard]] IUIAutomation* get() const noexcept { return client_.get(); }
private:
    bool initialized_{};
    ComReference<IUIAutomation> client_;
};

[[nodiscard]] Domain::Result<std::wstring> nativeExecutablePath(const Domain::PathText& executable)
{
    auto decoded = Detail::strictUtf8ToUtf16(executable.value());
    if (!decoded) { return decoded; }
    // QueryFullProcessImageNameW returns native separators; composition paths may
    // contain forward slashes. Normalize both identity and activation's boundary.
    return Domain::Result<std::wstring>::success(
        std::filesystem::path{decoded.value()}.lexically_normal().make_preferred().native());
}

struct WindowSearch final {
    std::wstring expectedExecutable;
    std::vector<HWND> windows;
    bool allocationFailed{};
};

BOOL CALLBACK collectWindows(HWND window, LPARAM parameter) noexcept
{
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    if (!::IsWindowVisible(window)) {
        return TRUE;
    }
    DWORD pid{};
    ::GetWindowThreadProcessId(window, &pid);
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return TRUE;
    }
    std::array<wchar_t, 32U * 1024U> executable{};
    DWORD size = static_cast<DWORD>(executable.size());
    const bool read = ::QueryFullProcessImageNameW(process, 0, executable.data(), &size) != FALSE;
    ::CloseHandle(process);
    if (read && ::CompareStringOrdinal(executable.data(), static_cast<int>(size),
        search.expectedExecutable.data(), static_cast<int>(search.expectedExecutable.size()),
        TRUE) == CSTR_EQUAL) {
        try {
            search.windows.push_back(window);
        } catch (...) {
            search.allocationFailed = true;
            return FALSE;
        }
    }
    return TRUE;
}

[[nodiscard]] Domain::Result<ComReference<IUIAutomationElement>> findControl(
    IUIAutomation& client,
    IUIAutomationElement& root,
    const wchar_t* name,
    const CONTROLTYPEID type)
{
    VARIANT label{};
    label.vt = VT_BSTR;
    label.bstrVal = ::SysAllocString(name);
    if (label.bstrVal == nullptr) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Allocating UI Automation label", E_OUTOFMEMORY));
    }
    ComReference<IUIAutomationCondition> nameCondition;
    HRESULT result = client.CreatePropertyCondition(UIA_NamePropertyId, label, nameCondition.put());
    ::VariantClear(&label);
    if (FAILED(result)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Finding LM Studio control by name", result));
    }
    VARIANT controlType{};
    controlType.vt = VT_I4;
    controlType.lVal = type;
    ComReference<IUIAutomationCondition> typeCondition;
    result = client.CreatePropertyCondition(UIA_ControlTypePropertyId, controlType, typeCondition.put());
    if (FAILED(result)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Finding LM Studio control by type", result));
    }
    ComReference<IUIAutomationCondition> condition;
    result = client.CreateAndCondition(nameCondition.get(), typeCondition.get(), condition.put());
    if (FAILED(result)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Combining LM Studio control conditions", result));
    }
    ComReference<IUIAutomationElement> element;
    result = root.FindFirst(TreeScope_Descendants, condition.get(), element.put());
    if (FAILED(result)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Reading LM Studio accessibility tree", result));
    }
    return Domain::Result<ComReference<IUIAutomationElement>>::success(std::move(element));
}

[[nodiscard]] std::string visibleControls(
    IUIAutomation& client, IUIAutomationElement& root,
    const Domain::OperationContext& context)
{
    const auto timeout = std::min(context.deadline, Clock::now() + std::chrono::seconds{3});
    const auto nameOf = [](IUIAutomationElement& element) {
        BSTR value{};
        const HRESULT result = element.get_CurrentName(&value);
        if (FAILED(result)) {
            return std::string{"<name HRESULT "} + std::to_string(static_cast<std::uint32_t>(result)) + ">";
        }
        const std::wstring text{value == nullptr ? L"" : value,
            value == nullptr ? 0U : std::min<std::size_t>(::SysStringLen(value), 120U)};
        ::SysFreeString(value);
        auto converted = Detail::strictUtf16ToUtf8(text);
        if (!converted) {
            return std::string{"<invalid UTF-16 name>"};
        }
        auto label = std::move(converted).value();
        for (auto& character : label) {
            if (character == '\r' || character == '\n' || character == '\t') {
                character = ' ';
            }
        }
        return label.empty() ? std::string{"<unnamed>"} : label;
    };
    std::string summary = " UIA window=[" + nameOf(root) + "]; visible buttons=";
    VARIANT type{};
    type.vt = VT_I4;
    type.lVal = UIA_ButtonControlTypeId;
    ComReference<IUIAutomationCondition> condition;
    HRESULT result = client.CreatePropertyCondition(UIA_ControlTypePropertyId, type, condition.put());
    if (FAILED(result)) {
        return summary + "<condition HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + ">";
    }
    ComReference<IUIAutomationElementArray> elements;
    result = root.FindAll(TreeScope_Descendants, condition.get(), elements.put());
    if (FAILED(result)) {
        return summary + "<enumeration HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + ">";
    }
    int count{};
    result = elements->get_Length(&count);
    if (FAILED(result)) {
        return summary + "<count HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + ">";
    }
    unsigned int reported{};
    for (int index = 0; index < count && reported < 40U; ++index) {
        if (interrupted(context) || Clock::now() >= timeout) {
            summary += " [diagnostic bound reached]";
            break;
        }
        ComReference<IUIAutomationElement> element;
        result = elements->GetElement(index, element.put());
        if (FAILED(result)) {
            summary += " [element HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + "]";
            break;
        }
        BOOL offscreen{};
        result = element->get_CurrentIsOffscreen(&offscreen);
        if (SUCCEEDED(result) && offscreen != FALSE) {
            continue;
        }
        summary += " [" + nameOf(*element.get()) + "]";
        ++reported;
    }
    if (reported == 0U) {
        summary += " []";
    }
    return summary;
}

struct ChatWindow final {
    HWND handle{};
    ComReference<IUIAutomationElement> root;
};

[[nodiscard]] Domain::Result<std::optional<ChatWindow>> findChatWindow(
    IUIAutomation& client,
    const std::wstring& executable,
    const Domain::OperationContext& context)
{
    WindowSearch search{executable, {}};
    if (!::EnumWindows(collectWindows, reinterpret_cast<LPARAM>(&search))) {
        if (search.allocationFailed) {
            return Domain::Result<std::optional<ChatWindow>>::failure(
                comError("Enumerating LM Studio windows", E_OUTOFMEMORY));
        }
        return Domain::Result<std::optional<ChatWindow>>::failure(capabilityError(
            "LM Studio window enumeration failed (Windows error " +
            std::to_string(::GetLastError()) + ")."));
    }
    for (HWND handle : search.windows) {
        if (auto error = interrupted(context)) {
            return Domain::Result<std::optional<ChatWindow>>::failure(std::move(*error));
        }
        ComReference<IUIAutomationElement> root;
        const HRESULT result = client.ElementFromHandle(handle, root.put());
        if (FAILED(result)) {
            continue;
        }
        for (const auto& [label, type] : std::array{
            std::pair{L"Chat input", UIA_EditControlTypeId},
            std::pair{L"New chat", UIA_ButtonControlTypeId},
            std::pair{L"Stop generating", UIA_ButtonControlTypeId},
            std::pair{L"Close model loader popover and clear filter term", UIA_ButtonControlTypeId}}) {
            auto control = findControl(client, *root.get(), label, type);
            if (!control) {
                return Domain::Result<std::optional<ChatWindow>>::failure(std::move(control).error());
            }
            if (control.value()) {
                return Domain::Result<std::optional<ChatWindow>>::success(
                    ChatWindow{handle, std::move(root)});
            }
        }
    }
    return Domain::Result<std::optional<ChatWindow>>::success(std::nullopt);
}

[[nodiscard]] Domain::Result<void> restoreWindow(
    const HWND window, const Domain::OperationContext& context,
    const Domain::MonotonicTimePoint timeout)
{
    if (auto error = interrupted(context)) {
        return Domain::Result<void>::failure(std::move(*error));
    }
    if (!::IsWindow(window)) {
        return Domain::Result<void>::failure(capabilityError(
            "The selected LM Studio native window no longer exists; no control was invoked."));
    }
    const bool wasIconic = ::IsIconic(window) != FALSE;
    if (wasIconic) {
        ::ShowWindow(window, SW_RESTORE);
    }
    while (::IsIconic(window) && Clock::now() < timeout) {
        if (auto error = interrupted(context)) {
            return Domain::Result<void>::failure(std::move(*error));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    if (!::IsWindow(window) || ::IsIconic(window)) {
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio's selected native window did not restore before control actions; IsIconic before=" +
            std::string{wasIconic ? "true" : "false"} + "; IsIconic after=" +
            (::IsIconic(window) ? "true" : "false") + "."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<bool> isIdle(IUIAutomation& client, IUIAutomationElement& root)
{
    auto stop = findControl(client, root, L"Stop generating", UIA_ButtonControlTypeId);
    if (!stop) {
        return Domain::Result<bool>::failure(std::move(stop).error());
    }
    // LM Studio displays this control for both predicting and stopping.
    return Domain::Result<bool>::success(!stop.value());
}

[[nodiscard]] Domain::Result<void> invoke(IUIAutomationElement& element, const std::string_view action)
{
    ComReference<IUIAutomationInvokePattern> pattern;
    HRESULT result = element.GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(pattern.put()));
    if (FAILED(result)) {
        return Domain::Result<void>::failure(comError(action, result));
    }
    if (!pattern) {
        return Domain::Result<void>::failure(capabilityError(std::string{action} +
            " returned HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) +
            " with no InvokePattern interface; no action was invoked."));
    }
    result = pattern->Invoke();
    return FAILED(result) ? Domain::Result<void>::failure(comError(action, result))
                         : Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<std::wstring> inputText(
    IUIAutomationElement& input, bool* fromTextPattern = nullptr)
{
    if (fromTextPattern != nullptr) {
        *fromTextPattern = false;
    }
    BSTR value{};
    ComReference<IUIAutomationValuePattern> valuePattern;
    HRESULT result = input.GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(valuePattern.put()));
    if (SUCCEEDED(result) && valuePattern) {
        result = valuePattern->get_CurrentValue(&value);
    } else {
        ComReference<IUIAutomationTextPattern> textPattern;
        result = input.GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(textPattern.put()));
        if (FAILED(result) || !textPattern) {
            return Domain::Result<std::wstring>::failure(FAILED(result)
                ? comError("Reading LM Studio Chat input TextPattern", result)
                : capabilityError("LM Studio Chat input pattern retrieval returned HRESULT " +
                    std::to_string(static_cast<std::uint32_t>(result)) +
                    " with no ValuePattern or TextPattern interface; the draft was not read or modified."));
        }
        ComReference<IUIAutomationTextRange> range;
        result = textPattern->get_DocumentRange(range.put());
        if (FAILED(result) || !range) {
            return Domain::Result<std::wstring>::failure(FAILED(result)
                ? comError("Reading LM Studio Chat input document range", result)
                : capabilityError("LM Studio Chat input document range returned HRESULT " +
                    std::to_string(static_cast<std::uint32_t>(result)) + " with no range interface."));
        }
        result = range->GetText(-1, &value);
        if (fromTextPattern != nullptr) {
            *fromTextPattern = true;
        }
    }
    if (FAILED(result)) {
        ::SysFreeString(value);
        return Domain::Result<std::wstring>::failure(comError("Reading LM Studio Chat input", result));
    }
    std::wstring text{value == nullptr ? L"" : value,
                      value == nullptr ? 0U : static_cast<std::size_t>(::SysStringLen(value))};
    ::SysFreeString(value);
    return Domain::Result<std::wstring>::success(std::move(text));
}

[[nodiscard]] bool emptyInput(const std::wstring& value)
{
    return std::all_of(value.begin(), value.end(), [](const wchar_t character) {
        return character == L'\r' || character == L'\n' || character == L'\t' || character == L' ';
    });
}

[[nodiscard]] std::wstring normalizedNewlines(const std::wstring_view text)
{
    std::wstring normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            if (index + 1U < text.size() && text[index + 1U] == L'\n') { ++index; }
            normalized.push_back(L'\n');
        } else {
            normalized.push_back(text[index]);
        }
    }
    return normalized;
}

[[nodiscard]] bool completeInputReadback(
    const std::wstring& actual, const std::wstring& wanted, const bool fromTextPattern)
{
    return actual == wanted || (fromTextPattern && actual.size() == wanted.size() + 1U &&
        std::equal(wanted.begin(), wanted.end(), actual.begin()) &&
        (actual.back() == L'\n' || actual.back() == L'\u2029'));
}

[[nodiscard]] Domain::Result<void> confirmInput(
    IUIAutomationElement& input, const std::wstring& expected,
    const Domain::OperationContext& context)
{
    const auto timeout = std::min(context.deadline, Clock::now() + std::chrono::seconds{5});
    const auto wanted = normalizedNewlines(expected);
    std::wstring actual;
    bool fromTextPattern = false;
    do {
        if (auto error = interrupted(context)) {
            return Domain::Result<void>::failure(std::move(*error));
        }
        auto readback = inputText(input, &fromTextPattern);
        if (!readback) {
            auto error = std::move(readback).error();
            error.message += " Full continuity input readback is unavailable; Send was not invoked.";
            return Domain::Result<void>::failure(std::move(error));
        }
        actual = normalizedNewlines(readback.value());
        // Text ranges can expose one final paragraph separator after the complete body.
        if (completeInputReadback(actual, wanted, fromTextPattern)) {
            return Domain::Result<void>::success();
        }
        if (Clock::now() >= timeout) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    } while (Clock::now() < timeout);
    const auto mismatch = std::mismatch(wanted.begin(), wanted.end(), actual.begin(), actual.end());
    return Domain::Result<void>::failure(capabilityError(
        "LM Studio continuity input readback did not match within five seconds; expected UTF-16 length=" +
        std::to_string(wanted.size()) + "; actual UTF-16 length=" + std::to_string(actual.size()) +
        "; first mismatch=" + std::to_string(static_cast<std::size_t>(mismatch.first - wanted.begin())) +
        "; readback pattern=" + (fromTextPattern ? "TextPattern" : "ValuePattern") +
        "; only CR/CRLF line endings and one terminal TextPattern paragraph separator are normalized. Send was not invoked."));
}

[[nodiscard]] Domain::Result<void> fillInput(
    IUIAutomationElement& input,
    const HWND window,
    const std::wstring& text,
    const Domain::OperationContext& context)
{
    bool existingTextPattern = false;
    auto existing = inputText(input, &existingTextPattern);
    if (!existing) {
        return Domain::Result<void>::failure(std::move(existing).error());
    }
    if (!emptyInput(existing.value())) {
        if (completeInputReadback(normalizedNewlines(existing.value()), normalizedNewlines(text), existingTextPattern)) {
            return confirmInput(input, text, context);
        }
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio Chat input already contains a draft; continuity did not overwrite it."));
    }
    ComReference<IUIAutomationValuePattern> valuePattern;
    if (SUCCEEDED(input.GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(valuePattern.put()))) && valuePattern) {
        BSTR value = ::SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        if (value == nullptr) {
            return Domain::Result<void>::failure(comError("Allocating LM Studio Chat input", E_OUTOFMEMORY));
        }
        const HRESULT result = valuePattern->SetValue(value);
        ::SysFreeString(value);
        if (SUCCEEDED(result)) {
            return confirmInput(input, text, context);
        }
        auto current = inputText(input);
        if (!current || !emptyInput(current.value())) {
            return Domain::Result<void>::failure(comError("Setting LM Studio Chat input", result));
        }
    }
    if (auto error = interrupted(context)) {
        return Domain::Result<void>::failure(std::move(*error));
    }
    if (::IsIconic(window)) {
        ::ShowWindow(window, SW_RESTORE);
    }
    ::SetForegroundWindow(window);
    const HRESULT focus = input.SetFocus();
    if (FAILED(focus)) {
        return Domain::Result<void>::failure(comError("Focusing LM Studio Chat input", focus));
    }
    if (::GetAncestor(::GetForegroundWindow(), GA_ROOT) != window) {
        return Domain::Result<void>::failure(capabilityError(
            "Windows did not place the LM Studio chat window in the foreground; no input was sent."));
    }
    constexpr std::size_t ChunkCharacters = 64U;
    for (std::size_t offset = 0U; offset < text.size(); offset += ChunkCharacters) {
        if (auto error = interrupted(context)) {
            return Domain::Result<void>::failure(std::move(*error));
        }
        if (::GetAncestor(::GetForegroundWindow(), GA_ROOT) != window) {
            return Domain::Result<void>::failure(capabilityError(
                "LM Studio lost foreground focus while receiving the continuity message."));
        }
        std::array<INPUT, ChunkCharacters * 2U> inputs{};
        const std::size_t count = std::min(ChunkCharacters, text.size() - offset);
        for (std::size_t index = 0U; index < count; ++index) {
            inputs[index * 2U].type = INPUT_KEYBOARD;
            inputs[index * 2U].ki.wScan = static_cast<WORD>(text[offset + index]);
            inputs[index * 2U].ki.dwFlags = KEYEVENTF_UNICODE;
            inputs[index * 2U + 1U] = inputs[index * 2U];
            inputs[index * 2U + 1U].ki.dwFlags |= KEYEVENTF_KEYUP;
        }
        const UINT expected = static_cast<UINT>(count * 2U);
        const UINT sent = ::SendInput(expected, inputs.data(), sizeof(INPUT));
        if (sent != expected) {
            return Domain::Result<void>::failure(capabilityError(
                "LM Studio Unicode input sent " + std::to_string(sent) + " of " +
                std::to_string(expected) + " input events (Windows error " +
                std::to_string(::GetLastError()) + ")."));
        }
    }
    return confirmInput(input, text, context);
}

[[nodiscard]] Domain::Result<Domain::PathText> lmStudioRoot()
{
    PWSTR profile{};
    const HRESULT result = ::SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile);
    if (FAILED(result)) {
        return Domain::Result<Domain::PathText>::failure(comError("Reading the Windows profile folder", result));
    }
    const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> profileOwner{profile, &::CoTaskMemFree};
    const std::filesystem::path root = std::filesystem::path{profileOwner.get()} / L".lmstudio";
    auto utf8 = Detail::strictUtf16ToUtf8(root.native());
    if (!utf8) {
        return Domain::Result<Domain::PathText>::failure(std::move(utf8).error());
    }
    return Domain::PathText::create(std::move(utf8).value());
}

[[nodiscard]] Domain::Result<ComReference<IUIAutomationElement>> findNamed(
    IUIAutomation& client, IUIAutomationElement& root, const wchar_t* name)
{
    VARIANT label{};
    label.vt = VT_BSTR;
    label.bstrVal = ::SysAllocString(name);
    if (label.bstrVal == nullptr) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Allocating LM Studio integration label", E_OUTOFMEMORY));
    }
    ComReference<IUIAutomationCondition> condition;
    HRESULT result = client.CreatePropertyCondition(UIA_NamePropertyId, label, condition.put());
    ::VariantClear(&label);
    if (FAILED(result)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Finding an LM Studio integration label", result));
    }
    ComReference<IUIAutomationElement> element;
    result = root.FindFirst(TreeScope_Descendants, condition.get(), element.put());
    if (FAILED(result)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Reading LM Studio integration labels", result));
    }
    return Domain::Result<ComReference<IUIAutomationElement>>::success(std::move(element));
}

[[nodiscard]] std::string elementFacts(IUIAutomationElement& element)
{
    BSTR name{};
    const HRESULT named = element.get_CurrentName(&name);
    std::string label;
    if (SUCCEEDED(named)) {
        const std::wstring text{name == nullptr ? L"" : name,
            name == nullptr ? 0U : std::min<std::size_t>(::SysStringLen(name), 100U)};
        auto converted = Detail::strictUtf16ToUtf8(text);
        label = converted ? std::move(converted).value() : "<invalid UTF-16>";
    } else {
        label = "<name HRESULT " + std::to_string(static_cast<std::uint32_t>(named)) + ">";
    }
    ::SysFreeString(name);
    CONTROLTYPEID type{};
    const HRESULT typed = element.get_CurrentControlType(&type);
    RECT rectangle{};
    const HRESULT bounded = element.get_CurrentBoundingRectangle(&rectangle);
    const auto pattern = [&element](const PROPERTYID id) {
        VARIANT value{};
        const HRESULT result = element.GetCurrentPropertyValue(id, &value);
        std::string supported = SUCCEEDED(result) && value.vt == VT_BOOL
            ? value.boolVal != VARIANT_FALSE ? "true" : "false"
            : "<HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) +
                " type " + std::to_string(value.vt) + ">";
        ::VariantClear(&value);
        return supported;
    };
    return "name=[" + label + "], type=" + (SUCCEEDED(typed) ? std::to_string(type)
        : "<HRESULT " + std::to_string(static_cast<std::uint32_t>(typed)) + ">") +
        ", Toggle=" + pattern(UIA_IsTogglePatternAvailablePropertyId) +
        ", Invoke=" + pattern(UIA_IsInvokePatternAvailablePropertyId) +
        ", rect=" + (SUCCEEDED(bounded) ? "(" + std::to_string(rectangle.left) + "," +
            std::to_string(rectangle.top) + "," + std::to_string(rectangle.right) + "," +
            std::to_string(rectangle.bottom) + ")" : "<HRESULT " +
                std::to_string(static_cast<std::uint32_t>(bounded)) + ">");
}

[[nodiscard]] std::string patternCount(
    IUIAutomation& client, IUIAutomationElement& root, const PROPERTYID id)
{
    VARIANT available{};
    available.vt = VT_BOOL;
    available.boolVal = VARIANT_TRUE;
    ComReference<IUIAutomationCondition> condition;
    HRESULT result = client.CreatePropertyCondition(id, available, condition.put());
    if (FAILED(result)) {
        return "<condition HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + ">";
    }
    ComReference<IUIAutomationElementArray> elements;
    result = root.FindAll(TreeScope_Descendants, condition.get(), elements.put());
    if (FAILED(result)) {
        return "<enumeration HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + ">";
    }
    int count{};
    result = elements->get_Length(&count);
    return SUCCEEDED(result) ? std::to_string(count)
        : "<count HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + ">";
}

[[nodiscard]] std::string descendantFacts(
    IUIAutomation& client, IUIAutomationElement& root,
    const Domain::OperationContext& context)
{
    const auto timeout = std::min(context.deadline, Clock::now() + std::chrono::seconds{3});
    ComReference<IUIAutomationCondition> condition;
    HRESULT result = client.CreateTrueCondition(condition.put());
    if (FAILED(result)) {
        return "<true condition HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + ">";
    }
    ComReference<IUIAutomationElementArray> elements;
    result = root.FindAll(TreeScope_Descendants, condition.get(), elements.put());
    if (FAILED(result)) {
        return "<descendants HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + ">";
    }
    int count{};
    result = elements->get_Length(&count);
    if (FAILED(result)) {
        return "<descendant count HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + ">";
    }
    std::string evidence;
    for (int index = 0; index < std::min(count, 32); ++index) {
        if (interrupted(context) || Clock::now() >= timeout) {
            evidence += " [diagnostic bound reached]";
            break;
        }
        ComReference<IUIAutomationElement> element;
        result = elements->GetElement(index, element.put());
        if (FAILED(result)) {
            evidence += " [element HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + "]";
            break;
        }
        evidence += " {" + std::to_string(index) + ": " + elementFacts(*element.get()) + "}";
    }
    return evidence;
}

[[nodiscard]] Domain::Result<ComReference<IUIAutomationElement>> rowToggle(
    IUIAutomation& client, IUIAutomationElement& label, const wchar_t* requestedName,
    const bool development, const Domain::OperationContext& context)
{
    if (auto error = interrupted(context)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(std::move(*error));
    }
    ComReference<IUIAutomationTreeWalker> walker;
    HRESULT result = client.get_RawViewWalker(walker.put());
    if (FAILED(result)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Reading the LM Studio integration row", result));
    }
    ComReference<IUIAutomationElement> group;
    result = walker->GetParentElement(&label, group.put());
    if (FAILED(result) || !group) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(FAILED(result)
            ? comError("Reading the LM Studio integration label group", result)
            : capabilityError("LM Studio did not expose the integration label's group."));
    }
    auto badge = findNamed(client, *group.get(), L"DEV");
    if (!badge) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(std::move(badge).error());
    }
    if (static_cast<bool>(badge.value()) != development) {
        return Domain::Result<ComReference<IUIAutomationElement>>::success({});
    }
    VARIANT available{};
    available.vt = VT_BOOL;
    available.boolVal = VARIANT_TRUE;
    ComReference<IUIAutomationCondition> condition;
    result = client.CreatePropertyCondition(UIA_IsTogglePatternAvailablePropertyId, available, condition.put());
    if (FAILED(result)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Finding the integration's switch", result));
    }
    ComReference<IUIAutomationElementArray> contained;
    result = group->FindAll(TreeScope_Descendants, condition.get(), contained.put());
    if (FAILED(result)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Reading the integration group switches", result));
    }
    int count{};
    result = contained->get_Length(&count);
    if (FAILED(result)) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
            comError("Counting the integration group switches", result));
    }
    ComReference<IUIAutomationElement> selected;
    if (count == 1) {
        result = contained->GetElement(0, selected.put());
        if (FAILED(result) || !selected) {
            return Domain::Result<ComReference<IUIAutomationElement>>::failure(FAILED(result)
                ? comError("Reading the integration's contained switch", result)
                : capabilityError("The integration group returned a null unique switch."));
        }
        return Domain::Result<ComReference<IUIAutomationElement>>::success(std::move(selected));
    }
    ComReference<IUIAutomationElement> list;
    int labelIndex = -1;
    int switchIndex = -1;
    int interveningLabelIndex = -1;
    if (count == 0) {
        result = walker->GetParentElement(group.get(), list.put());
        if (FAILED(result) || !list) {
            return Domain::Result<ComReference<IUIAutomationElement>>::failure(FAILED(result)
                ? comError("Reading the integration's owning list", result)
                : capabilityError("LM Studio did not expose the integration group's owning list."));
        }
        CONTROLTYPEID type{};
        result = list->get_CurrentControlType(&type);
        if (FAILED(result)) {
            return Domain::Result<ComReference<IUIAutomationElement>>::failure(
                comError("Reading the integration's owning list type", result));
        }
        if (type != UIA_ListControlTypeId) {
            // Tool-result badges render the same owner/name label in a Group without
            // a switch. They are not entries in the observed integrations List.
            return Domain::Result<ComReference<IUIAutomationElement>>::success({});
        }
        if (type == UIA_ListControlTypeId) {
            ComReference<IUIAutomationCondition> all;
            result = client.CreateTrueCondition(all.put());
            if (FAILED(result) || !all) {
                return Domain::Result<ComReference<IUIAutomationElement>>::failure(FAILED(result)
                    ? comError("Creating the owning-list view condition", result)
                    : capabilityError("UI Automation returned no owning-list view condition."));
            }
            ComReference<IUIAutomationElementArray> elements;
            result = list->FindAll(TreeScope_Descendants, all.get(), elements.put());
            if (FAILED(result) || !elements) {
                return Domain::Result<ComReference<IUIAutomationElement>>::failure(FAILED(result)
                    ? comError("Reading the owning-list encounter order", result)
                    : capabilityError("LM Studio returned no owning-list encounter order."));
            }
            int length{};
            result = elements->get_Length(&length);
            if (FAILED(result)) {
                return Domain::Result<ComReference<IUIAutomationElement>>::failure(
                    comError("Counting the owning-list elements", result));
            }
            const auto selectionTimeout = std::min(context.deadline, Clock::now() + std::chrono::seconds{5});
            // The observed FindAll view includes each switch immediately before its
            // plugin label; the RawViewWalker sibling view omits those switches.
            for (int index = 0; index < std::min(length, 256); ++index) {
                if (auto error = interrupted(context)) {
                    return Domain::Result<ComReference<IUIAutomationElement>>::failure(std::move(*error));
                }
                if (Clock::now() >= selectionTimeout) {
                    return Domain::Result<ComReference<IUIAutomationElement>>::failure(capabilityError(
                        "LM Studio owning-list switch selection exceeded its five-second bound."));
                }
                ComReference<IUIAutomationElement> element;
                result = elements->GetElement(index, element.put());
                if (FAILED(result) || !element) {
                    return Domain::Result<ComReference<IUIAutomationElement>>::failure(FAILED(result)
                        ? comError("Reading an owning-list element", result)
                        : capabilityError("The owning-list view returned a null element."));
                }
                BOOL matches{};
                result = client.CompareElements(element.get(), &label, &matches);
                if (FAILED(result)) {
                    return Domain::Result<ComReference<IUIAutomationElement>>::failure(
                        comError("Matching the exact integration label in encounter order", result));
                }
                if (matches) {
                    labelIndex = index;
                    if (selected && interveningLabelIndex < 0) {
                        return Domain::Result<ComReference<IUIAutomationElement>>::success(std::move(selected));
                    }
                    break;
                }
                VARIANT toggle{};
                result = element->GetCurrentPropertyValue(UIA_IsTogglePatternAvailablePropertyId, &toggle);
                const bool isSwitch = SUCCEEDED(result) && toggle.vt == VT_BOOL && toggle.boolVal != VARIANT_FALSE;
                ::VariantClear(&toggle);
                if (FAILED(result)) {
                    return Domain::Result<ComReference<IUIAutomationElement>>::failure(
                        comError("Reading an owning-list element's switch availability", result));
                }
                if (isSwitch) {
                    selected = std::move(element);
                    switchIndex = index;
                    interveningLabelIndex = -1;
                    continue;
                }
                CONTROLTYPEID elementType{};
                result = element->get_CurrentControlType(&elementType);
                if (FAILED(result)) {
                    return Domain::Result<ComReference<IUIAutomationElement>>::failure(
                        comError("Reading an owning-list element's type", result));
                }
                if (selected && elementType == UIA_TextControlTypeId) {
                    BSTR name{};
                    result = element->get_CurrentName(&name);
                    const std::wstring text{name == nullptr ? L"" : name,
                        name == nullptr ? 0U : ::SysStringLen(name)};
                    ::SysFreeString(name);
                    if (FAILED(result)) {
                        return Domain::Result<ComReference<IUIAutomationElement>>::failure(
                            comError("Reading an intervening integration label", result));
                    }
                    // Plugin labels render owner/name, while DEV and source badges
                    // remain plain text. A different plugin label invalidates this switch.
                    const auto slash = text.find(L'/');
                    if (slash != std::wstring::npos && slash != 0U && slash + 1U < text.size()) {
                        interveningLabelIndex = index;
                    }
                }
            }
        }
    }
    auto requested = Detail::strictUtf16ToUtf8(requestedName);
    const std::string evidence = " target=[" + (requested ? std::move(requested).value() : "<invalid UTF-16>") +
        "], DEV requested=" + (development ? "true" : "false") + "; label={" + elementFacts(label) +
        "}; group={" + elementFacts(*group.get()) + ", Toggle descendants=" + std::to_string(count) +
        ", Invoke descendants=" + patternCount(client, *group.get(), UIA_IsInvokePatternAvailablePropertyId) +
        "}; owning list={" + (list ? elementFacts(*list.get()) : "absent") +
        "}; exact label index=" + std::to_string(labelIndex) + "; preceding switch index=" +
        std::to_string(switchIndex) + "; intervening plugin-label index=" + std::to_string(interveningLabelIndex) +
        "; owning-list encounter order=" + (list ? descendantFacts(client, *list.get(), context) : "absent");
    return Domain::Result<ComReference<IUIAutomationElement>>::failure(capabilityError(
        "LM Studio did not expose an owned switch immediately before the exact integration label." + evidence));
}

[[nodiscard]] Domain::Result<void> setIntegration(
    IUIAutomation& client, IUIAutomationElement& root, const wchar_t* name,
    const bool development, const bool desired, const bool required,
    const Domain::OperationContext& context, const Domain::MonotonicTimePoint timeout)
{
    VARIANT label{};
    label.vt = VT_BSTR;
    label.bstrVal = ::SysAllocString(name);
    if (label.bstrVal == nullptr) {
        return Domain::Result<void>::failure(comError("Allocating integration selection", E_OUTOFMEMORY));
    }
    ComReference<IUIAutomationCondition> condition;
    HRESULT result = client.CreatePropertyCondition(UIA_NamePropertyId, label, condition.put());
    ::VariantClear(&label);
    if (FAILED(result)) {
        return Domain::Result<void>::failure(comError("Finding the named integration rows", result));
    }
    ComReference<IUIAutomationElementArray> labels;
    result = root.FindAll(TreeScope_Descendants, condition.get(), labels.put());
    if (FAILED(result)) {
        return Domain::Result<void>::failure(comError("Reading the named integration rows", result));
    }
    int count{};
    result = labels->get_Length(&count);
    if (FAILED(result)) {
        return Domain::Result<void>::failure(comError("Counting the named integration rows", result));
    }
    ComReference<IUIAutomationElement> selected;
    for (int index = 0; index < count; ++index) {
        ComReference<IUIAutomationElement> element;
        result = labels->GetElement(index, element.put());
        if (FAILED(result)) {
            return Domain::Result<void>::failure(comError("Reading a named integration row", result));
        }
        auto found = rowToggle(client, *element.get(), name, development, context);
        if (!found) {
            return Domain::Result<void>::failure(std::move(found).error());
        }
        if (found.value()) {
            selected = std::move(found).value();
            break;
        }
    }
    if (!selected) {
        return required ? Domain::Result<void>::failure(capabilityError(
            "LM Studio did not expose a unique switch for the required installed Forge integration."))
            : Domain::Result<void>::success();
    }
    ComReference<IUIAutomationTogglePattern> pattern;
    result = selected->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(pattern.put()));
    if (FAILED(result)) {
        return Domain::Result<void>::failure(comError("Reading an LM Studio integration switch", result));
    }
    if (!pattern) {
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio integration pattern retrieval returned HRESULT " +
            std::to_string(static_cast<std::uint32_t>(result)) +
            " with no TogglePattern interface; the switch was not modified."));
    }
    ToggleState state{};
    result = pattern->get_CurrentToggleState(&state);
    if (FAILED(result)) {
        return Domain::Result<void>::failure(comError("Reading an LM Studio integration state", result));
    }
    const auto expected = desired ? ToggleState_On : ToggleState_Off;
    if (state == expected) {
        return Domain::Result<void>::success();
    }
    if (auto error = interrupted(context)) {
        return Domain::Result<void>::failure(std::move(*error));
    }
    BOOL enabled{};
    result = selected->get_CurrentIsEnabled(&enabled);
    if (FAILED(result) || enabled == FALSE) {
        return Domain::Result<void>::failure(FAILED(result) ? comError("Reading integration availability", result)
            : capabilityError("The required LM Studio integration switch is disabled."));
    }
    result = pattern->Toggle();
    if (FAILED(result)) {
        return Domain::Result<void>::failure(comError("Toggling the selected LM Studio integration", result));
    }
    while (Clock::now() < timeout) {
        if (auto error = interrupted(context)) {
            return Domain::Result<void>::failure(std::move(*error));
        }
        result = pattern->get_CurrentToggleState(&state);
        if (FAILED(result)) {
            return Domain::Result<void>::failure(comError("Verifying the LM Studio integration switch", result));
        }
        if (state == expected) {
            return Domain::Result<void>::success();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return Domain::Result<void>::failure(capabilityError("LM Studio did not apply the requested integration switch state."));
}

[[nodiscard]] Domain::Result<bool> developmentRowPresent(
    IUIAutomation& client, IUIAutomationElement& root, const wchar_t* name,
    const Domain::OperationContext& context)
{
    VARIANT named{};
    named.vt = VT_BSTR;
    named.bstrVal = ::SysAllocString(name);
    if (named.bstrVal == nullptr) {
        return Domain::Result<bool>::failure(comError("Allocating the development integration label", E_OUTOFMEMORY));
    }
    ComReference<IUIAutomationCondition> condition;
    HRESULT result = client.CreatePropertyCondition(UIA_NamePropertyId, named, condition.put());
    ::VariantClear(&named);
    if (FAILED(result) || !condition) {
        return Domain::Result<bool>::failure(FAILED(result)
            ? comError("Finding development integration rows", result)
            : capabilityError("UI Automation returned no development-row condition."));
    }
    ComReference<IUIAutomationElementArray> labels;
    result = root.FindAll(TreeScope_Descendants, condition.get(), labels.put());
    if (FAILED(result) || !labels) {
        return Domain::Result<bool>::failure(FAILED(result)
            ? comError("Reading development integration rows", result)
            : capabilityError("LM Studio returned no development-row collection."));
    }
    int count{};
    result = labels->get_Length(&count);
    if (FAILED(result)) {
        return Domain::Result<bool>::failure(comError("Counting development integration rows", result));
    }
    ComReference<IUIAutomationTreeWalker> walker;
    result = client.get_RawViewWalker(walker.put());
    if (FAILED(result) || !walker) {
        return Domain::Result<bool>::failure(FAILED(result)
            ? comError("Reading development integration groups", result)
            : capabilityError("UI Automation returned no development-row walker."));
    }
    for (int index = 0; index < count; ++index) {
        if (auto error = interrupted(context)) {
            return Domain::Result<bool>::failure(std::move(*error));
        }
        ComReference<IUIAutomationElement> label;
        result = labels->GetElement(index, label.put());
        if (FAILED(result) || !label) {
            return Domain::Result<bool>::failure(FAILED(result)
                ? comError("Reading a development integration label", result)
                : capabilityError("LM Studio returned a null development integration label."));
        }
        ComReference<IUIAutomationElement> group;
        result = walker->GetParentElement(label.get(), group.put());
        if (FAILED(result) || !group) {
            return Domain::Result<bool>::failure(FAILED(result)
                ? comError("Reading the development label's own group", result)
                : capabilityError("LM Studio returned no development label group."));
        }
        auto badge = findNamed(client, *group.get(), L"DEV");
        if (!badge) {
            return Domain::Result<bool>::failure(std::move(badge).error());
        }
        if (badge.value()) {
            return Domain::Result<bool>::success(true);
        }
    }
    return Domain::Result<bool>::success(false);
}

} // namespace

namespace Detail {

[[nodiscard]] bool sameRevision(
    const BY_HANDLE_FILE_INFORMATION& left, const BY_HANDLE_FILE_INFORMATION& right)
{
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
        left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow &&
        left.nFileSizeHigh == right.nFileSizeHigh && left.nFileSizeLow == right.nFileSizeLow &&
        left.ftLastWriteTime.dwHighDateTime == right.ftLastWriteTime.dwHighDateTime &&
        left.ftLastWriteTime.dwLowDateTime == right.ftLastWriteTime.dwLowDateTime;
}

[[nodiscard]] Domain::Result<NativeChatFile> readNativeChatFile(
    const std::wstring& path, const Domain::OperationContext& context)
{
    Detail::UniqueHandle file{::CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) {
        return Domain::Result<NativeChatFile>::failure(capabilityError(
            "Reading the selected LM Studio chat failed (Windows error " + std::to_string(::GetLastError()) + ")."));
    }
    NativeChatFile snapshot;
    if (!::GetFileInformationByHandle(file.get(), &snapshot.revision)) {
        return Domain::Result<NativeChatFile>::failure(capabilityError(
            "Reading the selected LM Studio chat revision failed (Windows error " + std::to_string(::GetLastError()) + ")."));
    }
    constexpr std::size_t MaximumChatBytes = 64U * 1024U * 1024U;
    if (snapshot.revision.nFileSizeHigh != 0U || snapshot.revision.nFileSizeLow > MaximumChatBytes) {
        return Domain::Result<NativeChatFile>::failure(capabilityError(
            "The selected LM Studio chat exceeds the 64 MiB native snapshot bound."));
    }
    snapshot.bytes.resize(snapshot.revision.nFileSizeLow);
    std::size_t offset{};
    while (offset < snapshot.bytes.size()) {
        if (auto error = interrupted(context)) {
            return Domain::Result<NativeChatFile>::failure(std::move(*error));
        }
        DWORD count{};
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(64U * 1024U, snapshot.bytes.size() - offset));
        if (!::ReadFile(file.get(), snapshot.bytes.data() + offset, wanted, &count, nullptr) || count == 0U) {
            return Domain::Result<NativeChatFile>::failure(capabilityError(
                "Reading the selected LM Studio chat bytes failed or ended early (Windows error " +
                std::to_string(::GetLastError()) + ")."));
        }
        offset += count;
    }
    BY_HANDLE_FILE_INFORMATION after{};
    if (!::GetFileInformationByHandle(file.get(), &after)) {
        return Domain::Result<NativeChatFile>::failure(capabilityError(
            "Rechecking the selected LM Studio chat revision failed (Windows error " + std::to_string(::GetLastError()) + ")."));
    }
    if (!sameRevision(snapshot.revision, after)) {
        return Domain::Result<NativeChatFile>::failure(Domain::makeError(Domain::ErrorCodes::Conflict,
            "The selected LM Studio chat changed while its native snapshot was read; retry.", true));
    }
    return Domain::Result<NativeChatFile>::success(std::move(snapshot));
}

} // namespace Detail

namespace {

[[nodiscard]] Domain::Result<std::pair<std::size_t, std::size_t>> pluginArrayRange(
    const std::string& bytes)
{
    int depth{};
    const auto space = [](const char value) {
        return value == ' ' || value == '\r' || value == '\n' || value == '\t';
    };
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        const char value = bytes[index];
        if (value == '"') {
            const auto begin = index;
            bool escaped = false;
            for (++index; index < bytes.size(); ++index) {
                if (!escaped && bytes[index] == '"') {
                    break;
                }
                if (!escaped && bytes[index] == '\\') {
                    escaped = true;
                } else {
                    escaped = false;
                }
            }
            if (index == bytes.size()) {
                break;
            }
            if (depth == 1) {
                const auto key = nlohmann::json::parse(bytes.substr(begin, index - begin + 1U), nullptr, false);
                std::size_t cursor = index + 1U;
                while (cursor < bytes.size() && space(bytes[cursor])) { ++cursor; }
                if (key.is_string() && key.get<std::string>() == "plugins" && cursor < bytes.size() && bytes[cursor] == ':') {
                    do { ++cursor; } while (cursor < bytes.size() && space(bytes[cursor]));
                    if (cursor == bytes.size() || bytes[cursor] != '[') { break; }
                    const auto arrayBegin = cursor;
                    int brackets = 1;
                    bool quoted = false;
                    escaped = false;
                    for (++cursor; cursor < bytes.size(); ++cursor) {
                        const char character = bytes[cursor];
                        if (quoted) {
                            if (!escaped && character == '"') { quoted = false; }
                            if (!escaped && character == '\\') { escaped = true; } else { escaped = false; }
                        } else if (character == '"') { quoted = true; }
                        else if (character == '[') { ++brackets; }
                        else if (character == ']' && --brackets == 0) {
                            return Domain::Result<std::pair<std::size_t, std::size_t>>::success({arrayBegin, cursor + 1U});
                        }
                    }
                    break;
                }
            }
        } else if (value == '{' || value == '[') { ++depth; }
        else if (value == '}' || value == ']') { --depth; }
    }
    return Domain::Result<std::pair<std::size_t, std::size_t>>::failure(capabilityError(
        "The selected LM Studio chat has no unambiguous top-level plugins array; no field was changed."));
}

[[nodiscard]] Domain::Result<void> waitWatcherBoundary(
    const Domain::OperationContext& context, const Domain::MonotonicTimePoint timeout)
{
    const auto until = Clock::now() + std::chrono::milliseconds{1200};
    if (until > timeout) {
        return Domain::Result<void>::failure(capabilityError(
            "The LM Studio file watcher refresh needs 1200 ms beyond its recent-write filter; no message was sent."));
    }
    while (Clock::now() < until) {
        if (auto error = interrupted(context)) {
            return Domain::Result<void>::failure(std::move(*error));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<bool> removeUnknownDevelopmentSelections(
    IUIAutomation& client, IUIAutomationElement& root, const Domain::PathText& lmRoot,
    const std::string_view expectedId, const Domain::OperationContext& context,
    const Domain::MonotonicTimePoint timeout)
{
    auto observation = WindowsLMStudioConversationReader::read(lmRoot, context);
    auto paused = isIdle(client, root);
    if (!observation || !paused || !observation.value() || !paused.value() ||
        observation.value()->toolsActive || observation.value()->conversationId != expectedId) {
        return Domain::Result<bool>::failure(!observation ? std::move(observation).error()
            : !paused ? std::move(paused).error()
            : capabilityError("LM Studio left the expected selected-chat/tool pause before integration cleanup; no field was changed."));
    }
    std::vector<std::string> remove;
    for (const auto& [identifier, rendered] : std::array{
        std::pair{"dev/forge-conductor/continuity", L"forge-conductor/continuity"},
        std::pair{"dev/mcp/forge-conductor-clu", L"mcp/forge-conductor-clu"}}) {
        if (std::find(observation.value()->plugins.begin(), observation.value()->plugins.end(), identifier) ==
            observation.value()->plugins.end()) { continue; }
        auto present = developmentRowPresent(client, root, rendered, context);
        if (!present) { return Domain::Result<bool>::failure(std::move(present).error()); }
        if (!present.value()) { remove.emplace_back(identifier); }
    }
    if (remove.empty()) { return Domain::Result<bool>::success(false); }
    auto quiet = waitWatcherBoundary(context, timeout);
    if (!quiet) { return Domain::Result<bool>::failure(std::move(quiet).error()); }
    auto path = Detail::strictUtf8ToUtf16(observation.value()->conversationPath);
    if (!path) { return Domain::Result<bool>::failure(std::move(path).error()); }
    auto original = readNativeChatFile(path.value(), context);
    if (!original) { return Domain::Result<bool>::failure(std::move(original).error()); }
    auto range = pluginArrayRange(original.value().bytes);
    if (!range) { return Domain::Result<bool>::failure(std::move(range).error()); }
    const auto [begin, end] = range.value();
    auto plugins = nlohmann::json::parse(original.value().bytes.substr(begin, end - begin), nullptr, false);
    if (!plugins.is_array() || !std::all_of(plugins.begin(), plugins.end(), [](const auto& id) { return id.is_string(); })) {
        return Domain::Result<bool>::failure(capabilityError("The native plugins field is not an array of identifiers; no field was changed."));
    }
    auto filtered = nlohmann::json::array();
    for (const auto& identifier : plugins) {
        if (std::find(remove.begin(), remove.end(), identifier.get<std::string>()) == remove.end()) {
            filtered.push_back(identifier);
        }
    }
    if (filtered == plugins) { return Domain::Result<bool>::success(false); }
    const std::string replacement = original.value().bytes.substr(0U, begin) + filtered.dump() + original.value().bytes.substr(end);
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) {
        return Domain::Result<bool>::failure(capabilityError("Allocating the native chat update name failed; no field was changed."));
    }
    std::array<wchar_t, 40U> suffix{};
    if (::StringFromGUID2(guid, suffix.data(), static_cast<int>(suffix.size())) == 0) {
        return Domain::Result<bool>::failure(capabilityError("Formatting the native chat update name failed; no field was changed."));
    }
    const std::wstring temporary = path.value() + L".forge-plugins-" + suffix.data() + L".tmp";
    struct TemporaryCleanup final {
        const std::wstring& path;
        ~TemporaryCleanup() noexcept { static_cast<void>(::DeleteFileW(path.c_str())); }
    } cleanup{temporary};
    Detail::UniqueHandle output{::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!output) {
        return Domain::Result<bool>::failure(capabilityError("Creating the native chat field update failed (Windows error " +
            std::to_string(::GetLastError()) + "); no field was changed."));
    }
    std::size_t offset{};
    while (offset < replacement.size()) {
        if (auto error = interrupted(context)) { return Domain::Result<bool>::failure(std::move(*error)); }
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(64U * 1024U, replacement.size() - offset));
        DWORD written{};
        if (!::WriteFile(output.get(), replacement.data() + offset, wanted, &written, nullptr) || written == 0U) {
            return Domain::Result<bool>::failure(capabilityError("Writing the native chat field update failed (Windows error " +
                std::to_string(::GetLastError()) + "); no field was changed."));
        }
        offset += written;
    }
    if (!::FlushFileBuffers(output.get())) {
        return Domain::Result<bool>::failure(capabilityError("Flushing the native chat field update failed (Windows error " +
            std::to_string(::GetLastError()) + "); no field was changed."));
    }
    output.reset();
    observation = WindowsLMStudioConversationReader::read(lmRoot, context);
    paused = isIdle(client, root);
    auto current = readNativeChatFile(path.value(), context);
    if (!current || !observation || !paused || !observation.value() || !paused.value() ||
        observation.value()->toolsActive || observation.value()->conversationId != expectedId ||
        !sameRevision(original.value().revision, current.value().revision) || current.value().bytes != original.value().bytes) {
        return Domain::Result<bool>::failure(!current ? std::move(current).error()
            : !observation ? std::move(observation).error()
            : !paused ? std::move(paused).error()
            : Domain::makeError(Domain::ErrorCodes::Conflict,
                "The selected native chat or its file revision changed immediately before integration cleanup; no field was changed.", true));
    }
    if (auto error = interrupted(context)) { return Domain::Result<bool>::failure(std::move(*error)); }
    if (!::ReplaceFileW(path.value().c_str(), temporary.c_str(), nullptr, 0U, nullptr, nullptr)) {
        return Domain::Result<bool>::failure(capabilityError("Replacing only the native chat plugins field failed (Windows error " +
            std::to_string(::GetLastError()) + "); no message was sent."));
    }
    auto refreshed = waitWatcherBoundary(context, timeout);
    if (!refreshed) { return Domain::Result<bool>::failure(std::move(refreshed).error()); }
    auto readback = readNativeChatFile(path.value(), context);
    if (!readback || readback.value().bytes != replacement) {
        return Domain::Result<bool>::failure(!readback ? std::move(readback).error()
            : capabilityError("LM Studio's native chat bytes did not retain the exact plugins-only update; no message was sent."));
    }
    return Domain::Result<bool>::success(true);
}

[[nodiscard]] Domain::Result<bool> integrationRowsAvailable(
    IUIAutomation& client, IUIAutomationElement& root,
    const Domain::OperationContext& context)
{
    VARIANT name{};
    name.vt = VT_BSTR;
    name.bstrVal = ::SysAllocString(L"mcp/forge-conductor");
    if (name.bstrVal == nullptr) { return Domain::Result<bool>::failure(comError("Allocating the installed integration label", E_OUTOFMEMORY)); }
    ComReference<IUIAutomationCondition> condition;
    HRESULT result = client.CreatePropertyCondition(UIA_NamePropertyId, name, condition.put());
    ::VariantClear(&name);
    if (FAILED(result) || !condition) {
        return Domain::Result<bool>::failure(FAILED(result) ? comError("Finding installed integration label candidates", result)
            : capabilityError("UI Automation returned no installed-integration label condition."));
    }
    ComReference<IUIAutomationElementArray> labels;
    result = root.FindAll(TreeScope_Descendants, condition.get(), labels.put());
    if (FAILED(result) || !labels) {
        return Domain::Result<bool>::failure(FAILED(result) ? comError("Reading installed integration label candidates", result)
            : capabilityError("LM Studio returned no installed-integration label collection."));
    }
    int count{};
    result = labels->get_Length(&count);
    if (FAILED(result)) { return Domain::Result<bool>::failure(comError("Counting installed integration label candidates", result)); }
    for (int index = 0; index < count; ++index) {
        if (auto error = interrupted(context)) { return Domain::Result<bool>::failure(std::move(*error)); }
        ComReference<IUIAutomationElement> label;
        result = labels->GetElement(index, label.put());
        if (FAILED(result) || !label) {
            return Domain::Result<bool>::failure(FAILED(result) ? comError("Reading an installed integration label candidate", result)
                : capabilityError("LM Studio returned a null installed-integration label candidate."));
        }
        auto toggle = rowToggle(client, *label.get(), L"mcp/forge-conductor", false, context);
        if (!toggle) { return Domain::Result<bool>::failure(std::move(toggle).error()); }
        if (toggle.value()) { return Domain::Result<bool>::success(true); }
    }
    return Domain::Result<bool>::success(false);
}

[[nodiscard]] Domain::Result<void> prepareIntegrations(
    IUIAutomation& client, IUIAutomationElement& root, const Domain::PathText& lmRoot,
    const std::string_view expectedId, const Domain::OperationContext& context,
    const Domain::MonotonicTimePoint timeout)
{
    auto selected = WindowsLMStudioConversationReader::read(lmRoot, context);
    auto paused = isIdle(client, root);
    if (!selected || !paused) {
        return Domain::Result<void>::failure(!selected ? std::move(selected).error() : std::move(paused).error());
    }
    if (!selected.value() || selected.value()->conversationId != expectedId || selected.value()->toolsActive || !paused.value()) {
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio changed selected conversation or left its tool/prediction pause before integration preparation; no message was sent."));
    }
    const auto& enabled = selected.value()->plugins;
    const auto has = [&enabled](const std::string_view id) {
        return std::find(enabled.begin(), enabled.end(), id) != enabled.end();
    };
    // useChatEnabledPluginsWritableSignal stores these enabled IDs; useResolvePlugins
    // resolves that exact saved list. Preserve an already configured native session.
    if (has("mcp/forge-conductor") && has("mcp/forge-conductor-fallback") && has("mcp/forge-conductor-clu") &&
        !has("dev/forge-conductor/continuity") && !has("dev/mcp/forge-conductor-clu")) {
        return Domain::Result<void>::success();
    }

    auto popup = findControl(client, root, L"Integrations select", UIA_ButtonControlTypeId);
    if (!popup || !popup.value()) {
        return Domain::Result<void>::failure(!popup ? std::move(popup).error()
            : capabilityError("LM Studio's Integrations select control is absent." + visibleControls(client, root, context)));
    }
    auto available = integrationRowsAvailable(client, root, context);
    if (!available) {
        return Domain::Result<void>::failure(std::move(available).error());
    }
    const bool openedHere = !available.value();
    if (openedHere) {
        auto opened = invoke(*popup.value().get(), "Opening LM Studio integrations");
        if (!opened) {
            return opened;
        }
    }
    while (!available.value() && Clock::now() < timeout) {
        if (auto error = interrupted(context)) {
            return Domain::Result<void>::failure(std::move(*error));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        available = integrationRowsAvailable(client, root, context);
        if (!available) {
            return Domain::Result<void>::failure(std::move(available).error());
        }
    }
    if (!available.value()) {
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio integrations did not expose the installed forge-conductor label and its owned switch." +
            visibleControls(client, root, context)));
    }
    auto cleaned = removeUnknownDevelopmentSelections(client, root, lmRoot, expectedId, context, timeout);
    if (!cleaned) { return Domain::Result<void>::failure(std::move(cleaned).error()); }
    if (cleaned.value()) {
        auto saved = setIntegration(client, root, L"mcp/forge-conductor", false, false, true, context, timeout);
        if (!saved) { return saved; }
        saved = setIntegration(client, root, L"mcp/forge-conductor", false, true, true, context, timeout);
        if (!saved) { return saved; }
    }
    for (const auto* name : {L"forge-conductor/continuity", L"mcp/forge-conductor-clu"}) {
        auto changed = setIntegration(client, root, name, true, false, false, context, timeout);
        if (!changed) {
            return changed;
        }
    }
    for (const auto* name : {L"mcp/forge-conductor", L"mcp/forge-conductor-fallback", L"mcp/forge-conductor-clu"}) {
        auto changed = setIntegration(client, root, name, false, true, true, context, timeout);
        if (!changed) {
            return changed;
        }
    }
    return openedHere ? invoke(*popup.value().get(), "Closing LM Studio integrations")
        : Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<ComReference<IUIAutomationElement>> classicFilterInput(
    IUIAutomation& client, IUIAutomationElement& root,
    const Domain::OperationContext& context)
{
    auto named = findControl(client, root, L"Type to filter models...", UIA_EditControlTypeId);
    if (!named || named.value()) { return named; }
    auto close = findControl(client, root, L"Close model loader popover and clear filter term", UIA_ButtonControlTypeId);
    if (!close) { return Domain::Result<ComReference<IUIAutomationElement>>::failure(std::move(close).error()); }
    if (!close.value()) { return Domain::Result<ComReference<IUIAutomationElement>>::success({}); }
    VARIANT type{};
    type.vt = VT_I4;
    type.lVal = UIA_EditControlTypeId;
    ComReference<IUIAutomationCondition> condition;
    HRESULT result = client.CreatePropertyCondition(UIA_ControlTypePropertyId, type, condition.put());
    if (FAILED(result) || !condition) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(FAILED(result)
            ? comError("Finding the model loader's filter", result)
            : capabilityError("UI Automation returned no model-filter condition."));
    }
    ComReference<IUIAutomationElementArray> edits;
    result = root.FindAll(TreeScope_Descendants, condition.get(), edits.put());
    if (FAILED(result) || !edits) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(FAILED(result)
            ? comError("Reading the model loader's filter", result)
            : capabilityError("LM Studio returned no model-filter collection."));
    }
    int count{};
    result = edits->get_Length(&count);
    if (FAILED(result)) { return Domain::Result<ComReference<IUIAutomationElement>>::failure(comError("Counting model-loader edit controls", result)); }
    ComReference<IUIAutomationElement> filter;
    std::string evidence;
    for (int index = 0; index < count && index < 32; ++index) {
        if (auto error = interrupted(context)) { return Domain::Result<ComReference<IUIAutomationElement>>::failure(std::move(*error)); }
        ComReference<IUIAutomationElement> edit;
        result = edits->GetElement(index, edit.put());
        if (FAILED(result) || !edit) {
            return Domain::Result<ComReference<IUIAutomationElement>>::failure(FAILED(result)
                ? comError("Reading a model-loader edit control", result)
                : capabilityError("LM Studio returned a null model-loader edit control."));
        }
        BSTR rawName{};
        result = edit->get_CurrentName(&rawName);
        const std::wstring name{rawName == nullptr ? L"" : rawName, rawName == nullptr ? 0U : ::SysStringLen(rawName)};
        ::SysFreeString(rawName);
        if (FAILED(result)) { return Domain::Result<ComReference<IUIAutomationElement>>::failure(comError("Reading a model-loader edit label", result)); }
        if (name == L"Chat input") { continue; }
        BOOL offscreen{};
        result = edit->get_CurrentIsOffscreen(&offscreen);
        if (SUCCEEDED(result) && offscreen != FALSE) { continue; }
        evidence += " {" + elementFacts(*edit.get()) + "}";
        if (filter) {
            return Domain::Result<ComReference<IUIAutomationElement>>::failure(capabilityError(
                "The exact LM Studio model-loader popover exposed multiple filter candidates; no input was sent." + evidence));
        }
        filter = std::move(edit);
    }
    if (count > 32) {
        return Domain::Result<ComReference<IUIAutomationElement>>::failure(capabilityError(
            "LM Studio's model-loader filter enumeration exceeded its bound; no input was sent."));
    }
    return Domain::Result<ComReference<IUIAutomationElement>>::success(std::move(filter));
}

[[nodiscard]] Domain::Result<void> closeClassicModelPopover(
    IUIAutomation& client, IUIAutomationElement& root,
    const Domain::OperationContext& context,
    const std::optional<std::string_view> expectedConversationId = std::nullopt)
{
    auto close = findControl(client, root, L"Close model loader popover and clear filter term", UIA_ButtonControlTypeId);
    if (!close) { return Domain::Result<void>::failure(std::move(close).error()); }
    if (!close.value()) { return Domain::Result<void>::success(); }
    auto filter = classicFilterInput(client, root, context);
    auto eject = findControl(client, root, L"Eject", UIA_ButtonControlTypeId);
    if (!filter || !eject) {
        return Domain::Result<void>::failure(!filter ? std::move(filter).error() : std::move(eject).error());
    }
    if (!filter.value() && !eject.value()) {
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio's exact close-loader control lacks its owned filter or loaded-model evidence; it was not invoked."));
    }
    auto lmRoot = lmStudioRoot();
    if (!lmRoot) { return Domain::Result<void>::failure(std::move(lmRoot).error()); }
    auto selected = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
    auto paused = isIdle(client, root);
    if (!selected || !paused) {
        return Domain::Result<void>::failure(!selected ? std::move(selected).error() : std::move(paused).error());
    }
    if (!selected.value() || selected.value()->toolsActive || !paused.value()) {
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio left its selected tool/prediction pause before closing its model picker; no chat input was sent."));
    }
    const std::string conversation = selected.value()->conversationId;
    if (expectedConversationId && conversation != *expectedConversationId) {
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio selection drift before closing its model picker: expected [" + std::string{*expectedConversationId} +
            "] but selected [" + conversation + "]; no chat input was sent."));
    }
    auto closed = invoke(*close.value().get(), "Closing the exact LM Studio model-loader popover");
    if (!closed) { return closed; }
    const auto timeout = std::min(context.deadline, Clock::now() + std::chrono::seconds{5});
    while (Clock::now() < timeout) {
        if (auto error = interrupted(context)) { return Domain::Result<void>::failure(std::move(*error)); }
        close = findControl(client, root, L"Close model loader popover and clear filter term", UIA_ButtonControlTypeId);
        if (!close) { return Domain::Result<void>::failure(std::move(close).error()); }
        auto input = findControl(client, root, L"Chat input", UIA_EditControlTypeId);
        if (!input) { return Domain::Result<void>::failure(std::move(input).error()); }
        if (!close.value() && input.value()) {
            selected = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
            paused = isIdle(client, root);
            if (!selected || !paused) {
                return Domain::Result<void>::failure(!selected ? std::move(selected).error() : std::move(paused).error());
            }
            if (!selected.value() || selected.value()->conversationId != conversation || selected.value()->toolsActive || !paused.value()) {
                return Domain::Result<void>::failure(capabilityError(
                    "LM Studio changed selection or left its tool/prediction pause after its model picker closed; no chat input was sent."));
            }
            return Domain::Result<void>::success();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return Domain::Result<void>::failure(capabilityError(
        "LM Studio's model picker did not return to its main Chat input within five seconds; no chat input was sent." + visibleControls(client, root, context)));
}

[[nodiscard]] Domain::Result<bool> classicSelectedModel(
    IUIAutomation& client, IUIAutomationElement& root,
    const Domain::OperationContext& context, std::string* detail = nullptr,
    const std::optional<std::string_view> expectedConversationId = std::nullopt)
{
    auto lmRoot = lmStudioRoot();
    if (!lmRoot) { return Domain::Result<bool>::failure(std::move(lmRoot).error()); }
    auto selected = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
    if (!selected || !selected.value()) {
        return Domain::Result<bool>::failure(!selected ? std::move(selected).error()
            : capabilityError("LM Studio has no selected native conversation for loaded-model acknowledgment."));
    }
    if (expectedConversationId && selected.value()->conversationId != *expectedConversationId) {
        return Domain::Result<bool>::failure(capabilityError(
            "LM Studio selection drift during loaded-model acknowledgment: expected [" + std::string{*expectedConversationId} +
            "] but selected [" + selected.value()->conversationId + "]; no chat input was sent."));
    }
    auto path = Detail::strictUtf8ToUtf16(selected.value()->conversationPath);
    if (!path) { return Domain::Result<bool>::failure(std::move(path).error()); }
    auto raw = readNativeChatFile(path.value(), context);
    if (!raw) { return Domain::Result<bool>::failure(std::move(raw).error()); }
    const auto state = nlohmann::json::parse(raw.value().bytes, nullptr, false);
    if (!state.is_object()) {
        return Domain::Result<bool>::failure(capabilityError("LM Studio's selected native conversation is not valid JSON during model acknowledgment."));
    }
    std::string identifier;
    const auto used = state.find("lastUsedModel");
    if (used != state.end() && used->is_object()) {
        const auto stored = used->find("identifier");
        if (stored != used->end() && stored->is_string()) { identifier = stored->get<std::string>(); }
    }
    auto close = findControl(client, root, L"Close model loader popover and clear filter term", UIA_ButtonControlTypeId);
    auto input = findControl(client, root, L"Chat input", UIA_EditControlTypeId);
    if (!close || !input) {
        return Domain::Result<bool>::failure(!close ? std::move(close).error() : std::move(input).error());
    }
    bool matchingTrigger = false;
    if (!identifier.empty()) {
        auto label = Detail::strictUtf8ToUtf16(identifier);
        if (!label) { return Domain::Result<bool>::failure(std::move(label).error()); }
        auto trigger = findControl(client, root, label.value().c_str(), UIA_ButtonControlTypeId);
        if (!trigger) { return Domain::Result<bool>::failure(std::move(trigger).error()); }
        matchingTrigger = static_cast<bool>(trigger.value());
    }
    if (detail != nullptr) {
        *detail = "lastUsedModel.identifier=[" + identifier + "], matching native trigger=" +
            (matchingTrigger ? "true" : "false") + ", loader closed=" + (!close.value() ? "true" : "false") +
            ", Chat input=" + (input.value() ? "true" : "false");
    }
    // ModelLoaderTrigger renders B.identifier only when useChatLastUsedModelInstance
    // resolves the saved identifier to a currently loaded, non-unloading instance.
    return Domain::Result<bool>::success(!close.value() && input.value() && matchingTrigger);
}

[[nodiscard]] Domain::Result<void> acknowledgeClassicModel(
    IUIAutomation& client, IUIAutomationElement& root,
    const Domain::OperationContext& context, const std::string_view action)
{
    auto lmRoot = lmStudioRoot();
    if (!lmRoot) { return Domain::Result<void>::failure(std::move(lmRoot).error()); }
    auto initial = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
    if (!initial || !initial.value()) {
        return Domain::Result<void>::failure(!initial ? std::move(initial).error()
            : capabilityError("LM Studio's native selected conversation disappeared before model acknowledgment."));
    }
    const auto expected = initial.value()->conversationId;
    const auto timeout = std::min(context.deadline, Clock::now() + std::chrono::seconds{4});
    std::string detail;
    while (Clock::now() < timeout) {
        if (auto error = interrupted(context)) { return Domain::Result<void>::failure(std::move(*error)); }
        auto confirmed = classicSelectedModel(client, root, context, &detail, expected);
        if (!confirmed) { return Domain::Result<void>::failure(std::move(confirmed).error()); }
        if (confirmed.value()) { return Domain::Result<void>::success(); }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return Domain::Result<void>::failure(capabilityError(
        "LM Studio did not acknowledge " + std::string{action} + " within four seconds: " + detail +
        "; no chat input was sent." + visibleControls(client, root, context)));
}

[[nodiscard]] Domain::Result<bool> selectClassicLoadedWithKeyboard(
    IUIAutomation& client, IUIAutomationElement& root, const HWND window,
    const Domain::OperationContext& context)
{
    auto filter = classicFilterInput(client, root, context);
    if (!filter) { return Domain::Result<bool>::failure(std::move(filter).error()); }
    if (!filter.value()) { return Domain::Result<bool>::success(false); }
    auto text = inputText(*filter.value().get());
    if (!text) { return Domain::Result<bool>::failure(std::move(text).error()); }
    if (!emptyInput(text.value())) {
        return Domain::Result<bool>::failure(capabilityError(
            "LM Studio's model filter has existing text; its draft was preserved and no keyboard model selection was sent."));
    }
    ComReference<IUIAutomationCondition> all;
    HRESULT result = client.CreateTrueCondition(all.put());
    if (FAILED(result) || !all) {
        return Domain::Result<bool>::failure(FAILED(result) ? comError("Bounding native model navigation", result)
            : capabilityError("UI Automation returned no native model-navigation condition."));
    }
    ComReference<IUIAutomationElementArray> descendants;
    result = root.FindAll(TreeScope_Descendants, all.get(), descendants.put());
    if (FAILED(result) || !descendants) {
        return Domain::Result<bool>::failure(FAILED(result) ? comError("Bounding native model navigation", result)
            : capabilityError("LM Studio returned no native model-navigation collection."));
    }
    int count{};
    result = descendants->get_Length(&count);
    if (FAILED(result) || count < 1 || count > 512) {
        return Domain::Result<bool>::failure(FAILED(result) ? comError("Counting native model navigation elements", result)
            : capabilityError("LM Studio's model-navigation element count is outside the bounded native view; no keys were sent."));
    }
    ::SetForegroundWindow(window);
    result = filter.value()->SetFocus();
    if (FAILED(result)) { return Domain::Result<bool>::failure(comError("Focusing the exact model-loader filter", result)); }
    const auto focused = [&]() -> Domain::Result<void> {
        if (auto error = interrupted(context)) { return Domain::Result<void>::failure(std::move(*error)); }
        ComReference<IUIAutomationElement> actual;
        HRESULT read = client.GetFocusedElement(actual.put());
        BOOL same{};
        if (SUCCEEDED(read) && actual) { read = client.CompareElements(actual.get(), filter.value().get(), &same); }
        if (FAILED(read) || !actual || same == FALSE || ::GetAncestor(::GetForegroundWindow(), GA_ROOT) != window) {
            return Domain::Result<void>::failure(capabilityError(
                "Native focus is not the exact owned LM Studio model filter; no further selection keys were sent."));
        }
        return Domain::Result<void>::success();
    };
    for (int step = 0; step <= count; ++step) {
        if (step % 32 == 0) {
            auto owned = focused();
            if (!owned) { return Domain::Result<bool>::failure(std::move(owned).error()); }
        }
        std::array<INPUT, 2> keys{};
        keys[0].type = INPUT_KEYBOARD;
        keys[0].ki.wVk = VK_UP;
        keys[1] = keys[0];
        keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
        const UINT sent = ::SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
        if (sent != keys.size()) {
            return Domain::Result<bool>::failure(capabilityError(
                "Navigating the verified loaded-model picker accepted " + std::to_string(sent) + " of 2 native key events."));
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    auto owned = focused();
    if (!owned) { return Domain::Result<bool>::failure(std::move(owned).error()); }
    // The installed handler clamps ArrowUp at option zero, where LoadedModelsSection
    // renders its already-loaded instances before generators and unloaded models.
    std::array<INPUT, 2> enter{};
    enter[0].type = INPUT_KEYBOARD;
    enter[0].ki.wVk = VK_RETURN;
    enter[1] = enter[0];
    enter[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT sent = ::SendInput(static_cast<UINT>(enter.size()), enter.data(), sizeof(INPUT));
    return sent == enter.size() ? Domain::Result<bool>::success(true)
        : Domain::Result<bool>::failure(capabilityError(
            "Selecting the verified first loaded-model option accepted " + std::to_string(sent) + " of 2 native key events."));
}

[[nodiscard]] Domain::Result<bool> selectClassicLoadedRow(
    IUIAutomation& client, IUIAutomationElement& root,
    const Domain::OperationContext& context)
{
    auto eject = findControl(client, root, L"Eject", UIA_ButtonControlTypeId);
    if (!eject) { return Domain::Result<bool>::failure(std::move(eject).error()); }
    if (!eject.value()) { return Domain::Result<bool>::success(false); }
    ComReference<IUIAutomationTreeWalker> walker;
    HRESULT result = client.get_RawViewWalker(walker.put());
    if (FAILED(result) || !walker) {
        return Domain::Result<bool>::failure(FAILED(result)
            ? comError("Reading the loaded-model row's parent", result)
            : capabilityError("UI Automation returned no loaded-model tree walker."));
    }
    ComReference<IUIAutomationCondition> all;
    result = client.CreateTrueCondition(all.put());
    if (FAILED(result) || !all) {
        return Domain::Result<bool>::failure(FAILED(result)
            ? comError("Finding the loaded-model row's descendants", result)
            : capabilityError("UI Automation returned no loaded-row condition."));
    }
    ComReference<IUIAutomationElement> row;
    result = walker->GetParentElement(eject.value().get(), row.put());
    std::string evidence;
    const auto bound = std::min(context.deadline, Clock::now() + std::chrono::seconds{5});
    for (unsigned int depth = 0U; SUCCEEDED(result) && row && depth < 7U && Clock::now() < bound; ++depth) {
        if (auto error = interrupted(context)) { return Domain::Result<bool>::failure(std::move(*error)); }
        ComReference<IUIAutomationElementArray> descendants;
        result = row->FindAll(TreeScope_Descendants, all.get(), descendants.put());
        if (FAILED(result) || !descendants) {
            return Domain::Result<bool>::failure(FAILED(result)
                ? comError("Reading the loaded-model row", result)
                : capabilityError("LM Studio returned no loaded-model row collection."));
        }
        int count{};
        result = descendants->get_Length(&count);
        if (FAILED(result)) { return Domain::Result<bool>::failure(comError("Counting loaded-model row descendants", result)); }
        unsigned int ejectCount{}, contextCount{};
        bool ownedEject = false, loading = false, section = false;
        ComReference<IUIAutomationElement> previousText, modelName;
        std::wstring previousLabel;
        for (int index = 0; index < count && index < 128 && Clock::now() < bound; ++index) {
            if (auto error = interrupted(context)) { return Domain::Result<bool>::failure(std::move(*error)); }
            ComReference<IUIAutomationElement> element;
            result = descendants->GetElement(index, element.put());
            if (FAILED(result) || !element) {
                return Domain::Result<bool>::failure(FAILED(result)
                    ? comError("Reading a loaded-model row element", result)
                    : capabilityError("LM Studio returned a null loaded-model row element."));
            }
            CONTROLTYPEID type{};
            result = element->get_CurrentControlType(&type);
            if (FAILED(result)) { return Domain::Result<bool>::failure(comError("Reading a loaded-model row element type", result)); }
            BSTR rawName{};
            result = element->get_CurrentName(&rawName);
            const std::wstring name{rawName == nullptr ? L"" : rawName,
                rawName == nullptr ? 0U : ::SysStringLen(rawName)};
            ::SysFreeString(rawName);
            if (FAILED(result)) { return Domain::Result<bool>::failure(comError("Reading a loaded-model row label", result)); }
            if (type == UIA_ButtonControlTypeId && name == L"Eject") {
                ++ejectCount;
                BOOL same{};
                result = client.CompareElements(element.get(), eject.value().get(), &same);
                if (FAILED(result)) { return Domain::Result<bool>::failure(comError("Verifying the owned loaded-model Eject control", result)); }
                ownedEject = ownedEject || same != FALSE;
            }
            if (type == UIA_TextControlTypeId) {
                loading = loading || name.starts_with(L"Loading...");
                section = section || name.starts_with(L"Currently Loaded") || name == L"Your Generators";
                if (name.starts_with(L"Context: ")) {
                    ++contextCount;
                    if (previousText && !previousLabel.empty() && !previousLabel.starts_with(L"Context: ") &&
                        !previousLabel.starts_with(L"Currently Loaded") && !previousLabel.starts_with(L"Loading...")) {
                        modelName = std::move(previousText);
                    }
                }
                previousLabel = name;
                previousText = std::move(element);
            }
        }
        evidence += " {depth=" + std::to_string(depth) + ", " + elementFacts(*row.get()) +
            ", ejects=" + std::to_string(ejectCount) + ", contexts=" + std::to_string(contextCount) + "}";
        if (count < 128 && ownedEject && ejectCount == 1U && contextCount == 1U && modelName && !loading && !section) {
            // SelectLoadedModelOption owns model-name text, Context badge and Eject.
            // Its row action selects the existing instance; Eject is never invoked.
            UIA_HWND nativeWindow{};
            result = root.get_CurrentNativeWindowHandle(&nativeWindow);
            const HWND window = static_cast<HWND>(nativeWindow);
            if (FAILED(result) || window == nullptr || !::IsWindow(window)) {
                return Domain::Result<bool>::failure(capabilityError(
                    "The owned loaded-model row has no native action and its LM Studio HWND is unavailable; no model was selected. " + evidence));
            }
            auto keyboard = selectClassicLoadedWithKeyboard(client, root, window, context);
            if (!keyboard) { return keyboard; }
            if (keyboard.value()) {
                auto confirmed = acknowledgeClassicModel(client, root, context, "verified model-filter ArrowUp/Enter selection");
                return confirmed ? Domain::Result<bool>::success(true)
                    : Domain::Result<bool>::failure(std::move(confirmed).error());
            }
            ::SetForegroundWindow(window);
            if (::GetAncestor(::GetForegroundWindow(), GA_ROOT) != window) {
                return Domain::Result<bool>::failure(capabilityError(
                    "LM Studio did not own the foreground window before loaded-model selection; no model was selected."));
            }
            RECT rectangle{};
            result = modelName->get_CurrentBoundingRectangle(&rectangle);
            if (FAILED(result) || rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top) {
                return Domain::Result<bool>::failure(capabilityError(
                    "The exact owned loaded-model name has no native action or usable rectangle; no model was selected. " +
                    elementFacts(*modelName.get()) + "; row=" + evidence + "; descendants=" + descendantFacts(client, *row.get(), context)));
            }
            const POINT point{rectangle.left + (rectangle.right - rectangle.left) / 2,
                rectangle.top + (rectangle.bottom - rectangle.top) / 2};
            ComReference<IUIAutomationElement> hit;
            result = client.ElementFromPoint(point, hit.put());
            BOOL exact{};
            if (SUCCEEDED(result) && hit) { result = client.CompareElements(hit.get(), modelName.get(), &exact); }
            if (FAILED(result) || !hit || exact == FALSE ||
                ::GetAncestor(::WindowFromPoint(point), GA_ROOT) != window) {
                return Domain::Result<bool>::failure(capabilityError(
                    "Native hit testing did not identify the exact owned loaded-model name in LM Studio; no model was selected. " +
                    elementFacts(*modelName.get()) + "; hit=" + (hit ? elementFacts(*hit.get()) : "<none>")));
            }
            if (auto error = interrupted(context)) { return Domain::Result<bool>::failure(std::move(*error)); }
            const int left = ::GetSystemMetrics(SM_XVIRTUALSCREEN), top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN), height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (width <= 1 || height <= 1 || point.x < left || point.y < top ||
                point.x >= left + width || point.y >= top + height ||
                ::GetAncestor(::GetForegroundWindow(), GA_ROOT) != window) {
                return Domain::Result<bool>::failure(capabilityError("LM Studio's verified model-name point is unavailable; no model was selected."));
            }
            std::array<INPUT, 3> inputs{};
            inputs[0].type = INPUT_MOUSE;
            inputs[0].mi.dx = static_cast<LONG>((static_cast<std::int64_t>(point.x) - left) * 65535 / (width - 1));
            inputs[0].mi.dy = static_cast<LONG>((static_cast<std::int64_t>(point.y) - top) * 65535 / (height - 1));
            inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            inputs[2].type = INPUT_MOUSE;
            inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            const UINT sent = ::SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
            if (sent != inputs.size()) {
                return Domain::Result<bool>::failure(capabilityError("Selecting the verified loaded-model name accepted " +
                    std::to_string(sent) + " of 3 native mouse events."));
            }
            auto confirmed = acknowledgeClassicModel(client, root, context, "exact native model-name selection");
            return confirmed ? Domain::Result<bool>::success(true)
                : Domain::Result<bool>::failure(std::move(confirmed).error());
        }
        ComReference<IUIAutomationElement> parent;
        result = walker->GetParentElement(row.get(), parent.put());
        row = std::move(parent);
    }
    return Domain::Result<bool>::failure(capabilityError(
        "The native Eject control did not identify an owned already-loaded model row with one model name and Context badge; no model was selected." +
        evidence + "; descendants=" + (row ? descendantFacts(client, *row.get(), context) : "<no further parent>")));
}

[[nodiscard]] Domain::Result<void> selectClassicLoadedModel(
    IUIAutomation& client, IUIAutomationElement& root, const Domain::OperationContext& context,
    const Domain::MonotonicTimePoint timeout)
{
    VARIANT identifier{};
    identifier.vt = VT_BSTR;
    identifier.bstrVal = ::SysAllocString(L"model-loader-option-0");
    if (identifier.bstrVal == nullptr) {
        return Domain::Result<void>::failure(comError("Allocating the loaded-model option ID", E_OUTOFMEMORY));
    }
    ComReference<IUIAutomationCondition> condition;
    HRESULT result = client.CreatePropertyCondition(UIA_AutomationIdPropertyId, identifier, condition.put());
    ::VariantClear(&identifier);
    if (FAILED(result) || !condition) {
        return Domain::Result<void>::failure(FAILED(result)
            ? comError("Finding the existing loaded-model option", result)
            : capabilityError("UI Automation returned no loaded-model option condition."));
    }
    while (Clock::now() < timeout) {
        if (auto error = interrupted(context)) { return Domain::Result<void>::failure(std::move(*error)); }
        ComReference<IUIAutomationElementArray> options;
        result = root.FindAll(TreeScope_Descendants, condition.get(), options.put());
        if (FAILED(result) || !options) {
            return Domain::Result<void>::failure(FAILED(result)
                ? comError("Reading the existing loaded-model option", result)
                : capabilityError("LM Studio returned no loaded-model option collection."));
        }
        int count{};
        result = options->get_Length(&count);
        if (FAILED(result)) { return Domain::Result<void>::failure(comError("Counting existing loaded-model options", result)); }
        if (count > 1) {
            return Domain::Result<void>::failure(capabilityError(
                "LM Studio exposed multiple model-loader-option-0 elements; no model was selected."));
        }
        if (count == 0) {
            auto row = selectClassicLoadedRow(client, root, context);
            if (!row) { return Domain::Result<void>::failure(std::move(row).error()); }
            if (row.value()) { return Domain::Result<void>::success(); }
        }
        if (count == 1) {
            ComReference<IUIAutomationElement> option;
            result = options->GetElement(0, option.put());
            if (FAILED(result) || !option) {
                return Domain::Result<void>::failure(FAILED(result)
                    ? comError("Reading the unique existing model option", result)
                    : capabilityError("LM Studio returned a null existing model option."));
            }
            auto eject = findControl(client, *option.get(), L"Eject", UIA_ButtonControlTypeId);
            if (!eject) { return Domain::Result<void>::failure(std::move(eject).error()); }
            VARIANT textType{};
            textType.vt = VT_I4;
            textType.lVal = UIA_TextControlTypeId;
            ComReference<IUIAutomationCondition> textCondition;
            result = client.CreatePropertyCondition(UIA_ControlTypePropertyId, textType, textCondition.put());
            if (FAILED(result) || !textCondition) {
                return Domain::Result<void>::failure(FAILED(result)
                    ? comError("Finding the existing model context badge", result)
                    : capabilityError("UI Automation returned no context-badge condition."));
            }
            ComReference<IUIAutomationElementArray> texts;
            result = option->FindAll(TreeScope_Descendants, textCondition.get(), texts.put());
            if (FAILED(result) || !texts) {
                return Domain::Result<void>::failure(FAILED(result)
                    ? comError("Reading the existing model context badge", result)
                    : capabilityError("LM Studio returned no context-badge collection."));
            }
            int textCount{};
            result = texts->get_Length(&textCount);
            if (FAILED(result)) { return Domain::Result<void>::failure(comError("Counting existing model badges", result)); }
            bool hasContext = false;
            bool loading = false;
            for (int index = 0; index < textCount; ++index) {
                if (auto error = interrupted(context)) { return Domain::Result<void>::failure(std::move(*error)); }
                ComReference<IUIAutomationElement> text;
                result = texts->GetElement(index, text.put());
                if (FAILED(result) || !text) {
                    return Domain::Result<void>::failure(FAILED(result)
                        ? comError("Reading an existing model badge", result)
                        : capabilityError("LM Studio returned a null existing model badge."));
                }
                BSTR name{};
                result = text->get_CurrentName(&name);
                const std::wstring value{name == nullptr ? L"" : name, name == nullptr ? 0U : ::SysStringLen(name)};
                ::SysFreeString(name);
                if (FAILED(result)) { return Domain::Result<void>::failure(comError("Reading an existing model badge label", result)); }
                hasContext = hasContext || value.starts_with(L"Context: ");
                loading = loading || value.starts_with(L"Loading...");
            }
            if (eject.value() && hasContext && !loading) {
                // SelectLoadedModelOption marks the chat's existing instance reference;
                // its separate Eject button is evidence only and is never invoked.
                return invoke(*option.get(), "Selecting the classic already-loaded model option");
            }
            return Domain::Result<void>::failure(capabilityError(
                "LM Studio's first classic model option lacks its existing-instance Eject/Context markers or is still loading; no model was selected. " +
                elementFacts(*option.get()) + "; descendants=" + descendantFacts(client, *option.get(), context)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return Domain::Result<void>::failure(capabilityError(
        "LM Studio's classic loader did not expose model-loader-option-0 for an existing loaded instance; no model was selected." +
        visibleControls(client, root, context)));
}

[[nodiscard]] Domain::Result<void> selectLoadedModel(
    IUIAutomation& client, IUIAutomationElement& root, const Domain::OperationContext& context,
    const Domain::MonotonicTimePoint timeout)
{
    // The closed picker exposes this action only for a selected loaded instance.
    auto selected = findControl(client, root, L"Eject the model from memory", UIA_ButtonControlTypeId);
    if (!selected) {
        return Domain::Result<void>::failure(std::move(selected).error());
    }
    if (selected.value()) {
        return Domain::Result<void>::success();
    }
    auto classicSelected = classicSelectedModel(client, root, context);
    if (!classicSelected) { return Domain::Result<void>::failure(std::move(classicSelected).error()); }
    if (classicSelected.value()) { return Domain::Result<void>::success(); }
    auto existingPopover = selectClassicLoadedRow(client, root, context);
    if (!existingPopover) { return Domain::Result<void>::failure(std::move(existingPopover).error()); }
    if (existingPopover.value()) { return Domain::Result<void>::success(); }
    auto picker = findControl(client, root, L"Model picker", UIA_ButtonControlTypeId);
    bool classic = false;
    if (picker && !picker.value()) {
        picker = findControl(client, root, L"Select a model to load (Ctrl +L)", UIA_ButtonControlTypeId);
        classic = picker && static_cast<bool>(picker.value());
    }
    if (!picker || !picker.value()) {
        return Domain::Result<void>::failure(!picker ? std::move(picker).error()
            : capabilityError("LM Studio's Model picker control is absent." + visibleControls(client, root, context)));
    }
    auto opened = invoke(*picker.value().get(), "Opening LM Studio's loaded model picker");
    if (!opened) {
        return opened;
    }
    if (classic) {
        auto chosen = selectClassicLoadedModel(client, root, context, timeout);
        if (!chosen) { return chosen; }
        return acknowledgeClassicModel(client, root, context, "classic already-loaded model selection");
    }
    VARIANT type{};
    type.vt = VT_I4;
    type.lVal = UIA_MenuItemControlTypeId;
    ComReference<IUIAutomationCondition> condition;
    HRESULT result = client.CreatePropertyCondition(UIA_ControlTypePropertyId, type, condition.put());
    if (FAILED(result)) {
        return Domain::Result<void>::failure(comError("Finding loaded model menu items", result));
    }
    while (Clock::now() < timeout) {
        if (auto error = interrupted(context)) {
            return Domain::Result<void>::failure(std::move(*error));
        }
        ComReference<IUIAutomationElementArray> items;
        result = root.FindAll(TreeScope_Descendants, condition.get(), items.put());
        if (FAILED(result)) {
            return Domain::Result<void>::failure(comError("Reading the loaded model picker", result));
        }
        int count{};
        result = items->get_Length(&count);
        if (FAILED(result)) {
            return Domain::Result<void>::failure(comError("Counting model picker entries", result));
        }
        for (int index = 0; index < count; ++index) {
            ComReference<IUIAutomationElement> item;
            result = items->GetElement(index, item.put());
            if (FAILED(result)) {
                return Domain::Result<void>::failure(comError("Reading a loaded model entry", result));
            }
            auto loaded = findControl(client, *item.get(), L"Eject the model from memory", UIA_ButtonControlTypeId);
            if (!loaded) {
                return Domain::Result<void>::failure(std::move(loaded).error());
            }
            if (loaded.value()) {
                return invoke(*item.get(), "Selecting an already loaded LM Studio model");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return Domain::Result<void>::failure(capabilityError(
        "LM Studio's model picker did not expose an already loaded model; continuity did not load or unload a model."));
}

[[nodiscard]] Domain::Result<void> activateSingleton(const std::wstring& executable)
{
    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    request.lpVerb = L"open";
    request.lpFile = executable.c_str();
    request.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&request)) {
        return Domain::Result<void>::failure(capabilityError(
            "Activating the running LM Studio app failed (Windows error " +
            std::to_string(::GetLastError()) + ")."));
    }
    if (request.hProcess != nullptr) {
        ::CloseHandle(request.hProcess);
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<bool> completedNativeToolBoundary(
    const LMStudioConversationObservation& observed,
    const Domain::OperationContext& context)
{
    using Json = nlohmann::json;
    if (observed.toolsActive || observed.stopReason != "toolCalls" ||
        observed.generationEvidence.empty() || observed.nativeToolResults.empty()) {
        return Domain::Result<bool>::success(false);
    }
    const auto& last = observed.nativeToolResults.back();
    if (last.requestId.empty() || (last.pluginIdentifier != "mcp/forge-conductor" &&
        last.pluginIdentifier != "mcp/forge-conductor-fallback" && last.pluginIdentifier != "mcp/forge-conductor-clu")) {
        return Domain::Result<bool>::success(false);
    }
    auto path = Detail::strictUtf8ToUtf16(observed.conversationPath);
    if (!path) { return Domain::Result<bool>::failure(std::move(path).error()); }
    auto snapshot = readNativeChatFile(path.value(), context);
    if (!snapshot) { return Domain::Result<bool>::failure(std::move(snapshot).error()); }
    const auto document = Json::parse(snapshot.value().bytes, nullptr, false);
    const auto generation = Json::parse(observed.generationEvidence, nullptr, false);
    if (!document.is_object() || !generation.is_object() || !document.contains("messages") ||
        !document["messages"].is_array() || !generation.contains("message_index") ||
        !generation.contains("selected_version") || !generation.contains("step_index") ||
        !generation.contains("genInfo")) { return Domain::Result<bool>::success(false); }
    const auto messageIndex = generation["message_index"].get<std::size_t>();
    const auto selectedVersion = generation["selected_version"].get<std::size_t>();
    const auto stepIndex = generation["step_index"].get<std::size_t>();
    const auto& messages = document["messages"];
    if (messages.empty() || messageIndex != messages.size() - 1U) { return Domain::Result<bool>::success(false); }
    const auto& message = messages[messageIndex];
    if (!message.is_object() || !message.contains("currentlySelected") ||
        message["currentlySelected"] != selectedVersion || !message.contains("versions") ||
        !message["versions"].is_array() || selectedVersion >= message["versions"].size()) {
        return Domain::Result<bool>::success(false);
    }
    const auto& version = message["versions"][selectedVersion];
    if (!version.contains("steps") || !version["steps"].is_array() || stepIndex >= version["steps"].size()) {
        return Domain::Result<bool>::success(false);
    }
    const auto& steps = version["steps"];
    if (!steps[stepIndex].contains("genInfo") || steps[stepIndex]["genInfo"] != generation["genInfo"]) {
        return Domain::Result<bool>::success(false);
    }
    bool request = false, result = false;
    Json requestCallId;
    for (std::size_t index = stepIndex; index < steps.size(); ++index) {
        if (auto error = interrupted(context)) { return Domain::Result<bool>::failure(std::move(*error)); }
        const auto& step = steps[index];
        if (!step.is_object() || step.value("type", std::string{}) != "contentBlock" ||
            !step.contains("content") || !step["content"].is_array()) { continue; }
        for (const auto& part : step["content"]) {
            if (!part.is_object()) { continue; }
            const auto type = part.value("type", std::string{});
            if (type == "toolCallRequest") {
                if (result) { return Domain::Result<bool>::success(false); }
                request = part.contains("callId") && part["callId"].is_number() &&
                    part.value("toolCallRequestId", std::string{}) == last.requestId &&
                    part.value("pluginIdentifier", std::string{}) == last.pluginIdentifier &&
                    part.value("name", std::string{}) == last.name;
                if (request) { requestCallId = part["callId"]; }
            } else if (type == "toolCallResult" && request && part.contains("callId") && part["callId"] == requestCallId &&
                part.value("content", std::string{}) == last.content && part.value("name", last.name) == last.name &&
                part.value("toolCallRequestId", last.requestId) == last.requestId) {
                result = true;
            }
        }
    }
    return Domain::Result<bool>::success(request && result);
}

} // namespace

Domain::Result<void> WindowsLMStudioChatControl::activate(
    const Domain::PathText& executable,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (auto error = interrupted(context)) {
            return Domain::Result<void>::failure(std::move(*error));
        }
        auto path = nativeExecutablePath(executable);
        if (!path) {
            return Domain::Result<void>::failure(std::move(path).error());
        }
        Automation automation;
        auto initialized = automation.initialize();
        if (!initialized) {
            return Domain::Result<void>::failure(std::move(initialized).error());
        }
        const auto timeout = std::min(context.deadline, Clock::now() + std::chrono::seconds{25});
        auto window = findChatWindow(*automation.get(), path.value(), context);
        if (!window) {
            return Domain::Result<void>::failure(std::move(window).error());
        }
        if (window.value()) {
            auto restored = restoreWindow(window.value()->handle, context, timeout);
            if (!restored) { return restored; }
            return closeClassicModelPopover(*automation.get(), *window.value()->root.get(), context);
        }
        auto activated = activateSingleton(path.value());
        if (!activated) {
            return activated;
        }
        do {
            if (auto error = interrupted(context)) {
                return Domain::Result<void>::failure(std::move(*error));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            window = findChatWindow(*automation.get(), path.value(), context);
            if (!window) {
                return Domain::Result<void>::failure(std::move(window).error());
            }
        } while (!window.value() && Clock::now() < timeout);
        if (!window.value()) {
            return Domain::Result<void>::failure(capabilityError(
                "LM Studio activation did not expose a Chat input or New chat control within 25 seconds."));
        }
        auto restored = restoreWindow(window.value()->handle, context, timeout);
        if (!restored) { return restored; }
        return closeClassicModelPopover(*automation.get(), *window.value()->root.get(), context);
    } catch (...) {
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio could not expose its chat window through Windows UI Automation."));
    }
}

Domain::Result<bool> WindowsLMStudioChatControl::idle(
    const Domain::PathText& executable,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (auto error = interrupted(context)) {
            return Domain::Result<bool>::failure(std::move(*error));
        }
        auto path = nativeExecutablePath(executable);
        if (!path) {
            return Domain::Result<bool>::failure(std::move(path).error());
        }
        Automation automation;
        auto initialized = automation.initialize();
        if (!initialized) {
            return Domain::Result<bool>::failure(std::move(initialized).error());
        }
        auto window = findChatWindow(*automation.get(), path.value(), context);
        if (!window) {
            return Domain::Result<bool>::failure(std::move(window).error());
        }
        if (!window.value()) {
            return Domain::Result<bool>::failure(capabilityError(
                "LM Studio has no visible accessible chat window."));
        }
        return isIdle(*automation.get(), *window.value()->root.get());
    } catch (...) {
        return Domain::Result<bool>::failure(capabilityError(
            "LM Studio chat pause could not be inspected."));
    }
}

Domain::Result<bool> WindowsLMStudioChatControl::pauseAtToolBoundary(
    const Domain::PathText& executable,
    const std::string_view expectedConversationId,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (auto error = interrupted(context)) { return Domain::Result<bool>::failure(std::move(*error)); }
        if (expectedConversationId.empty()) {
            return Domain::Result<bool>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                "A native LM Studio conversation ID is required before pausing."));
        }
        auto path = nativeExecutablePath(executable);
        auto studio = lmStudioRoot();
        if (!path || !studio) { return Domain::Result<bool>::failure(!path ? std::move(path).error() : std::move(studio).error()); }
        Automation automation;
        auto initialized = automation.initialize();
        if (!initialized) { return Domain::Result<bool>::failure(std::move(initialized).error()); }
        auto window = findChatWindow(*automation.get(), path.value(), context);
        if (!window) { return Domain::Result<bool>::failure(std::move(window).error()); }
        if (!window.value()) { return Domain::Result<bool>::success(false); }
        auto selected = WindowsLMStudioConversationReader::read(studio.value(), context);
        if (!selected) { return Domain::Result<bool>::failure(std::move(selected).error()); }
        if (!selected.value() || selected.value()->conversationId != expectedConversationId || selected.value()->toolsActive) {
            return Domain::Result<bool>::success(false);
        }
        auto paused = isIdle(*automation.get(), *window.value()->root.get());
        if (!paused) { return Domain::Result<bool>::failure(std::move(paused).error()); }
        if (paused.value()) { return Domain::Result<bool>::success(true); }
        auto boundary = completedNativeToolBoundary(*selected.value(), context);
        if (!boundary || !boundary.value()) { return boundary; }
        auto stop = findControl(*automation.get(), *window.value()->root.get(), L"Stop generating", UIA_ButtonControlTypeId);
        if (!stop) { return Domain::Result<bool>::failure(std::move(stop).error()); }
        auto current = WindowsLMStudioConversationReader::read(studio.value(), context);
        if (!current) { return Domain::Result<bool>::failure(std::move(current).error()); }
        if (!current.value() || current.value()->conversationId != expectedConversationId || current.value()->toolsActive ||
            current.value()->generationEvidence != selected.value()->generationEvidence || current.value()->nativeToolResults.empty() ||
            current.value()->nativeToolResults.back().requestId != selected.value()->nativeToolResults.back().requestId ||
            current.value()->nativeToolResults.back().content != selected.value()->nativeToolResults.back().content) {
            return Domain::Result<bool>::success(false);
        }
        boundary = completedNativeToolBoundary(*current.value(), context);
        if (!boundary || !boundary.value()) { return boundary; }
        if (auto error = interrupted(context)) { return Domain::Result<bool>::failure(std::move(*error)); }
        if (!stop.value()) { return isIdle(*automation.get(), *window.value()->root.get()); }
        auto stopped = invoke(*stop.value().get(), "Pausing LM Studio at its completed native tool boundary");
        if (!stopped) { return Domain::Result<bool>::failure(std::move(stopped).error()); }
        const auto timeout = std::min(context.deadline, Clock::now() + std::chrono::seconds{10});
        while (Clock::now() < timeout) {
            if (auto error = interrupted(context)) { return Domain::Result<bool>::failure(std::move(*error)); }
            current = WindowsLMStudioConversationReader::read(studio.value(), context);
            if (!current) {
                if (!current.error().retryable) { return Domain::Result<bool>::failure(std::move(current).error()); }
            } else if (!current.value() || current.value()->conversationId != expectedConversationId || current.value()->toolsActive) {
                return Domain::Result<bool>::success(false);
            } else {
                paused = isIdle(*automation.get(), *window.value()->root.get());
                if (!paused) { return Domain::Result<bool>::failure(std::move(paused).error()); }
                if (paused.value()) { return Domain::Result<bool>::success(true); }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        return Domain::Result<bool>::failure(capabilityError(
            "LM Studio Stop generating was invoked once at a completed tool boundary, but a native idle pause was not acknowledged within ten seconds."));
    } catch (...) {
        return Domain::Result<bool>::failure(capabilityError("LM Studio's completed native tool pause could not be inspected."));
    }
}

Domain::Result<void> WindowsLMStudioChatControl::closeWindow(
    const Domain::PathText& executable,
    const Domain::OperationContext& context,
    const std::optional<std::string_view> expectedConversationId) noexcept
{
    try {
        if (auto error = interrupted(context)) { return Domain::Result<void>::failure(std::move(*error)); }
        auto path = nativeExecutablePath(executable);
        if (!path) { return Domain::Result<void>::failure(std::move(path).error()); }
        Automation automation;
        auto initialized = automation.initialize();
        if (!initialized) { return initialized; }
        auto window = findChatWindow(*automation.get(), path.value(), context);
        if (!window) { return Domain::Result<void>::failure(std::move(window).error()); }
        if (!window.value()) {
            WindowSearch visible{path.value(), {}};
            if (!::EnumWindows(collectWindows, reinterpret_cast<LPARAM>(&visible))) {
                return Domain::Result<void>::failure(visible.allocationFailed
                    ? comError("Inspecting owned LM Studio windows during cleanup", E_OUTOFMEMORY)
                    : capabilityError("LM Studio cleanup window enumeration failed (Windows error " +
                        std::to_string(::GetLastError()) + "); closure is unverified."));
            }
            if (!visible.windows.empty()) {
                return Domain::Result<void>::failure(capabilityError(
                    "LM Studio has " + std::to_string(visible.windows.size()) +
                    " visible owned HWNDs but no recognized chat or model-picker window; closure is unverified."));
            }
            return Domain::Result<void>::success();
        }
        auto lmRoot = lmStudioRoot();
        if (!lmRoot) { return Domain::Result<void>::failure(std::move(lmRoot).error()); }
        const auto pausedSelection = [&]() -> Domain::Result<void> {
            if (auto error = interrupted(context)) { return Domain::Result<void>::failure(std::move(*error)); }
            auto paused = isIdle(*automation.get(), *window.value()->root.get());
            auto selected = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
            if (!paused || !selected) {
                return Domain::Result<void>::failure(!paused ? std::move(paused).error() : std::move(selected).error());
            }
            if (!paused.value() || !selected.value() || selected.value()->toolsActive) {
                return Domain::Result<void>::failure(capabilityError(
                    "LM Studio is not at an observed tool/prediction pause; its window was not closed."));
            }
            if (expectedConversationId && selected.value()->conversationId != *expectedConversationId) {
                return Domain::Result<void>::failure(capabilityError(
                    "LM Studio selection drift before window cleanup: expected [" + std::string{*expectedConversationId} +
                    "] but selected [" + selected.value()->conversationId + "]; its window was not closed."));
            }
            return Domain::Result<void>::success();
        };
        auto paused = pausedSelection();
        if (!paused) { return paused; }
        ComReference<IUIAutomationWindowPattern> pattern;
        const HRESULT retrieved = window.value()->root->GetCurrentPatternAs(
            UIA_WindowPatternId, IID_PPV_ARGS(pattern.put()));
        paused = pausedSelection();
        if (!paused) { return paused; }
        const HWND handle = window.value()->handle;
        if (SUCCEEDED(retrieved) && pattern) {
            const HRESULT closed = pattern->Close();
            if (FAILED(closed)) {
                return Domain::Result<void>::failure(comError("Closing the owned LM Studio chat window", closed));
            }
        } else if (!::PostMessageW(handle, WM_CLOSE, 0, 0)) {
            return Domain::Result<void>::failure(capabilityError(
                "The normal LM Studio window-close message failed (Windows error " +
                std::to_string(::GetLastError()) + "); no process was terminated."));
        }
        const auto timeout = std::min(context.deadline, Clock::now() + std::chrono::seconds{5});
        while (::IsWindow(handle) && Clock::now() < timeout) {
            if (auto error = interrupted(context)) { return Domain::Result<void>::failure(std::move(*error)); }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        return !::IsWindow(handle) ? Domain::Result<void>::success()
            : Domain::Result<void>::failure(capabilityError(
                "LM Studio's owned chat HWND still exists after its normal close request; closure is unverified. "
                "No process was terminated."));
    } catch (...) {
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio's owned chat window could not be closed normally; no process was terminated."));
    }
}

Domain::Result<void> WindowsLMStudioChatControl::send(
    const Domain::PathText& executable,
    const std::string_view text,
    const bool newChat,
    const Domain::OperationContext& context,
    const std::optional<std::string_view> expectedConversationId,
    const std::function<void(std::string_view)> successorCreated,
    const LMStudioChatEffectObserver effectObserver) noexcept
{
    try {
        if (auto error = interrupted(context)) {
            return Domain::Result<void>::failure(std::move(*error));
        }
        if (text.empty()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest, "LM Studio continuity message is empty."));
        }
        auto path = nativeExecutablePath(executable);
        auto message = Detail::strictUtf8ToUtf16(text);
        if (!path || !message) {
            return Domain::Result<void>::failure(!path ? std::move(path).error() : std::move(message).error());
        }
        Automation automation;
        auto initialized = automation.initialize();
        if (!initialized) {
            return Domain::Result<void>::failure(std::move(initialized).error());
        }
        const auto timeout = std::min(context.deadline, Clock::now() + std::chrono::seconds{25});
        auto window = findChatWindow(*automation.get(), path.value(), context);
        if (!window) {
            return Domain::Result<void>::failure(std::move(window).error());
        }
        if (!window.value()) {
            auto activated = activateSingleton(path.value());
            if (!activated) {
                return activated;
            }
            do {
                if (auto error = interrupted(context)) {
                    return Domain::Result<void>::failure(std::move(*error));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                window = findChatWindow(*automation.get(), path.value(), context);
                if (!window) {
                    return Domain::Result<void>::failure(std::move(window).error());
                }
            } while (!window.value() && Clock::now() < timeout);
        }
        if (!window.value()) {
            return Domain::Result<void>::failure(capabilityError(
                "LM Studio activation did not expose a Chat input or New chat control within 25 seconds."));
        }
        const bool wasIconic = ::IsIconic(window.value()->handle) != FALSE;
        auto restored = restoreWindow(window.value()->handle, context, timeout);
        if (!restored) {
            return restored;
        }
        auto& root = *window.value()->root.get();
        auto pickerClosed = closeClassicModelPopover(*automation.get(), root, context, expectedConversationId);
        if (!pickerClosed) { return pickerClosed; }
        while (Clock::now() < timeout) {
            if (auto error = interrupted(context)) {
                return Domain::Result<void>::failure(std::move(*error));
            }
            auto paused = isIdle(*automation.get(), root);
            if (!paused) {
                return Domain::Result<void>::failure(std::move(paused).error());
            }
            if (paused.value()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        auto paused = isIdle(*automation.get(), root);
        if (!paused || !paused.value()) {
            return Domain::Result<void>::failure(!paused ? std::move(paused).error()
                : capabilityError("LM Studio did not reach a prediction pause within 25 seconds; no chat was started."));
        }
        auto lmRoot = lmStudioRoot();
        if (!lmRoot) {
            return Domain::Result<void>::failure(std::move(lmRoot).error());
        }
        auto initial = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
        if (!initial || !initial.value()) {
            return Domain::Result<void>::failure(!initial ? std::move(initial).error()
                : capabilityError("LM Studio has no selected conversation for continuity input."));
        }
        if (expectedConversationId && initial.value()->conversationId != *expectedConversationId) {
            return Domain::Result<void>::failure(capabilityError(
                "LM Studio selection drift: expected predecessor [" + std::string{*expectedConversationId} +
                "] but selected [" + initial.value()->conversationId + "]; no message was sent."));
        }
        std::string targetConversationId = initial.value()->conversationId;
        if (newChat) {
            auto previous = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
            if (!previous || !previous.value()) {
                return Domain::Result<void>::failure(!previous ? std::move(previous).error()
                    : capabilityError("LM Studio's predecessor conversation could not be identified before New chat."));
            }
            if (previous.value()->conversationId != targetConversationId) {
                return Domain::Result<void>::failure(capabilityError(
                    "LM Studio selection drift before New chat: expected [" + targetConversationId +
                    "] but selected [" + previous.value()->conversationId + "]; no chat was created."));
            }
            const auto predecessor = previous.value()->conversationId;
            auto button = findControl(*automation.get(), root, L"New chat", UIA_ButtonControlTypeId);
            if (button && !button.value()) {
                // LM Studio's expanded sidebar uses the English chat:newChatButton label.
                button = findControl(*automation.get(), root, L"New", UIA_ButtonControlTypeId);
            }
            if (!button) {
                return Domain::Result<void>::failure(std::move(button).error());
            }
            if (!button.value()) {
                return Domain::Result<void>::failure(capabilityError(
                    "LM Studio's New chat/New control is absent." + visibleControls(*automation.get(), root, context)));
            }
            auto beforeCreate = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
            if (!beforeCreate || !beforeCreate.value() || beforeCreate.value()->conversationId != predecessor) {
                return Domain::Result<void>::failure(!beforeCreate ? std::move(beforeCreate).error()
                    : capabilityError("LM Studio selection drift immediately before New chat; no chat was created."));
            }
            auto creationPause = isIdle(*automation.get(), root);
            if (!creationPause || !creationPause.value() || beforeCreate.value()->toolsActive) {
                return Domain::Result<void>::failure(!creationPause ? std::move(creationPause).error()
                    : capabilityError("LM Studio left its tool/prediction pause immediately before New chat; no chat was created."));
            }
            if (auto error = interrupted(context)) {
                return Domain::Result<void>::failure(std::move(*error));
            }
            if (effectObserver) {
                auto recorded = effectObserver({LMStudioChatEffect::NewChat, LMStudioChatEffectStage::BeforeDispatch, predecessor, 0U});
                if (!recorded) return recorded;
            }
            auto created = invoke(*button.value().get(), "Invoking LM Studio New chat");
            if (!created) {
                return created;
            }
            bool acknowledged = false;
            std::optional<std::string> candidateSuccessor;
            std::optional<Domain::Error> lastReadError;
            while (Clock::now() < timeout) {
                if (auto error = interrupted(context)) {
                    return Domain::Result<void>::failure(std::move(*error));
                }
                auto selected = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
                if (!selected) {
                    if (!selected.error().retryable) {
                        return Domain::Result<void>::failure(std::move(selected).error());
                    }
                    lastReadError = std::move(selected).error();
                    std::this_thread::sleep_for(std::chrono::milliseconds{100});
                    continue;
                }
                if (candidateSuccessor && selected.value() && selected.value()->conversationId != *candidateSuccessor) {
                    return Domain::Result<void>::failure(capabilityError(
                        "LM Studio selection changed while New chat was being acknowledged; no successor was adopted and no message was sent."));
                }
                if (selected.value() && selected.value()->conversationId != predecessor) {
                    if (!selected.value()->userMessages.empty() || !selected.value()->nativeToolResults.empty() || selected.value()->toolsActive) {
                        return Domain::Result<void>::failure(capabilityError(
                            "LM Studio New chat selected a conversation with existing native user/tool history or active tools; no successor was adopted and no message was sent."));
                    }
                    if (!candidateSuccessor) {
                        candidateSuccessor = selected.value()->conversationId;
                        std::this_thread::sleep_for(std::chrono::milliseconds{100});
                        continue;
                    }
                    targetConversationId = selected.value()->conversationId;
                    if (effectObserver) {
                        auto recorded = effectObserver({LMStudioChatEffect::NewChat, LMStudioChatEffectStage::Confirmed, targetConversationId, 0U});
                        if (!recorded) return recorded;
                    }
                    if (successorCreated) {
                        successorCreated(targetConversationId);
                    }
                    acknowledged = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
            if (!acknowledged) {
                return Domain::Result<void>::failure(capabilityError(
                    "LM Studio New chat was invoked, but no stable successor ID was acknowledged before the deadline. Automatic rollover is deferred; no selected chat was adopted and no continuity message was sent." +
                    (lastReadError ? " Last retryable native read: " + lastReadError->message : std::string{})));
            }
        }
        auto integrations = prepareIntegrations(*automation.get(), root, lmRoot.value(), targetConversationId, context, timeout);
        if (!integrations) {
            auto error = std::move(integrations).error();
            error.message += " Native window IsIconic before restore=" + std::string{wasIconic ? "true" : "false"} +
                "; IsIconic after=" + (::IsIconic(window.value()->handle) ? "true" : "false") + ".";
            return Domain::Result<void>::failure(std::move(error));
        }
        bool integrationsConfirmed = false;
        while (Clock::now() < timeout) {
            if (auto error = interrupted(context)) {
                return Domain::Result<void>::failure(std::move(*error));
            }
            auto observed = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
            if (!observed) {
                return Domain::Result<void>::failure(std::move(observed).error());
            }
            if (observed.value()) {
                const auto& plugins = observed.value()->plugins;
                const auto has = [&plugins](const std::string_view name) {
                    return std::find(plugins.begin(), plugins.end(), name) != plugins.end();
                };
                if (has("mcp/forge-conductor") && has("mcp/forge-conductor-fallback") &&
                    has("mcp/forge-conductor-clu") && !has("dev/forge-conductor/continuity") &&
                    !has("dev/mcp/forge-conductor-clu")) {
                    integrationsConfirmed = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if (!integrationsConfirmed) {
            return Domain::Result<void>::failure(capabilityError(
                "LM Studio did not persist the three existing Forge integrations without the old development continuity integration; no message was sent."));
        }
        auto model = selectLoadedModel(*automation.get(), root, context, timeout);
        if (!model) {
            return model;
        }
        auto stillPaused = isIdle(*automation.get(), root);
        auto selected = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
        if (selected && selected.value() && selected.value()->conversationId != targetConversationId) {
            return Domain::Result<void>::failure(capabilityError(
                "LM Studio selection drift before continuity input: expected [" + targetConversationId +
                "] but selected [" + selected.value()->conversationId + "]; no message was sent."));
        }
        if (!stillPaused || !selected || !stillPaused.value() || !selected.value() || selected.value()->toolsActive) {
            return Domain::Result<void>::failure(!stillPaused ? std::move(stillPaused).error()
                : !selected ? std::move(selected).error()
                : capabilityError("LM Studio left its tool/prediction pause before continuity input; no message was sent."));
        }
        auto input = findControl(*automation.get(), root, L"Chat input", UIA_EditControlTypeId);
        while (input && !input.value() && Clock::now() < timeout) {
            if (auto error = interrupted(context)) {
                return Domain::Result<void>::failure(std::move(*error));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            input = findControl(*automation.get(), root, L"Chat input", UIA_EditControlTypeId);
        }
        if (!input || !input.value()) {
            return Domain::Result<void>::failure(!input ? std::move(input).error()
                : capabilityError("LM Studio did not expose the Chat input after selecting its already-loaded model." +
                    visibleControls(*automation.get(), root, context)));
        }
        auto filled = fillInput(*input.value().get(), window.value()->handle, message.value(), context);
        if (!filled) {
            return filled;
        }
        while (Clock::now() < timeout) {
            if (auto error = interrupted(context)) {
                return Domain::Result<void>::failure(std::move(*error));
            }
            auto button = findControl(*automation.get(), root, L"Send", UIA_ButtonControlTypeId);
            if (!button) {
                return Domain::Result<void>::failure(std::move(button).error());
            }
            if (button.value()) {
                BOOL enabled{};
                const HRESULT result = button.value()->get_CurrentIsEnabled(&enabled);
                if (FAILED(result)) {
                    return Domain::Result<void>::failure(comError("Reading LM Studio Send state", result));
                }
                if (enabled != FALSE) {
                    auto pause = isIdle(*automation.get(), root);
                    auto current = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
                    if (current && current.value() && current.value()->conversationId != targetConversationId) {
                        return Domain::Result<void>::failure(capabilityError(
                            "LM Studio selection drift immediately before Send: expected [" + targetConversationId +
                            "] but selected [" + current.value()->conversationId + "]; no message was sent."));
                    }
                    if (!pause || !current || !pause.value() || !current.value() || current.value()->toolsActive) {
                        return Domain::Result<void>::failure(!pause ? std::move(pause).error()
                            : !current ? std::move(current).error()
                            : capabilityError("LM Studio left its tool/prediction pause before Send; the continuity message remains a draft."));
                    }
                    const auto previousUserMessages = current.value()->userMessages.size();
                    const auto expectedMessage = normalizedNewlines(message.value());
                    if (effectObserver) {
                        auto recorded = effectObserver({LMStudioChatEffect::Send, LMStudioChatEffectStage::BeforeDispatch, targetConversationId, previousUserMessages});
                        if (!recorded) return recorded;
                    }
                    auto dispatched = invoke(*button.value().get(), "Invoking LM Studio Send");
                    if (!dispatched) { return dispatched; }
                    std::optional<Domain::Error> lastReadError;
                    while (Clock::now() < timeout) {
                        if (auto error = interrupted(context)) {
                            error->message += " Send was dispatched once; its persisted submission remains unverified.";
                            return Domain::Result<void>::failure(std::move(*error));
                        }
                        auto submitted = WindowsLMStudioConversationReader::read(lmRoot.value(), context);
                        if (!submitted) {
                            if (!submitted.error().retryable) {
                                auto error = std::move(submitted).error();
                                error.message += " Send was dispatched once; its persisted submission remains unverified.";
                                return Domain::Result<void>::failure(std::move(error));
                            }
                            lastReadError = std::move(submitted).error();
                        } else if (submitted.value()) {
                            if (submitted.value()->conversationId != targetConversationId) {
                                return Domain::Result<void>::failure(Domain::makeError(
                                    Domain::ErrorCodes::AcknowledgementTimeout,
                                    "LM Studio Send was dispatched once, but selection changed from [" + targetConversationId +
                                    "] to [" + submitted.value()->conversationId +
                                    "] before its exact new user message was acknowledged; submission is ambiguous.", true));
                            }
                            const auto& messages = submitted.value()->userMessages;
                            for (std::size_t index = previousUserMessages; index < messages.size(); ++index) {
                                auto persisted = Detail::strictUtf8ToUtf16(messages[index]);
                                if (!persisted) {
                                    auto error = std::move(persisted).error();
                                    error.message += " Send was dispatched once; its persisted submission remains unverified.";
                                    return Domain::Result<void>::failure(std::move(error));
                                }
                                if (normalizedNewlines(persisted.value()) == expectedMessage) {
                                    if (effectObserver) {
                                        auto recorded = effectObserver({LMStudioChatEffect::Send, LMStudioChatEffectStage::Confirmed, targetConversationId, previousUserMessages});
                                        if (!recorded) return recorded;
                                    }
                                    return Domain::Result<void>::success();
                                }
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds{100});
                    }
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::AcknowledgementTimeout,
                        "LM Studio Send was dispatched once for [" + targetConversationId +
                        "] but its exact new user message was not persisted before the deadline; submission is ambiguous. "
                        "The controller did not repeat Send." +
                        (lastReadError ? " Last retryable native read: " + lastReadError->message : std::string{}), true));
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio Send did not become enabled within 25 seconds; the message remains in Chat input."));
    } catch (...) {
        return Domain::Result<void>::failure(capabilityError(
            "LM Studio could not receive the continuity message through Windows UI Automation."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
