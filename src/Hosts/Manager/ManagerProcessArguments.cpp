#include "ManagerProcessArguments.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "ForgeConductor/Domain/Error.h"

#include <limits>
#include <string>
#include <utility>

namespace ForgeConductor::Hosts::Manager {
namespace {

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    std::string message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, std::move(message)));
}

[[nodiscard]] Domain::Result<std::string> strictUtf16ToUtf8(
    const std::wstring_view value) noexcept
{
    try {
        if (value.find(L'\0') != std::wstring_view::npos) {
            return failure<std::string>(
                Domain::ErrorCodes::InvalidRequest,
                "A Manager process argument contains an embedded NUL.");
        }
        if (value.size() > ManagerProcessArguments::MaximumHomeUtf16Units ||
            value.size() >
                static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return failure<std::string>(
                Domain::ErrorCodes::LimitExceeded,
                "A Manager process argument exceeds the UTF-16 bound.");
        }
        if (value.empty()) {
            return Domain::Result<std::string>::success({});
        }

        const int inputLength = static_cast<int>(value.size());
        const int required = ::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            inputLength,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required == 0) {
            return failure<std::string>(
                Domain::ErrorCodes::InvalidRequest,
                "A Manager process argument is not valid UTF-16.");
        }

        std::string converted(static_cast<std::size_t>(required), '\0');
        const int written = ::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            inputLength,
            converted.data(),
            required,
            nullptr,
            nullptr);
        if (written != required) {
            return failure<std::string>(
                Domain::ErrorCodes::InvalidRequest,
                "A Manager process argument could not be converted from "
                "UTF-16.");
        }
        return Domain::Result<std::string>::success(std::move(converted));
    } catch (...) {
        return failure<std::string>(
            Domain::ErrorCodes::InternalFailure,
            "A Manager process argument could not be converted within its "
            "resource bound.");
    }
}

[[nodiscard]] wchar_t asciiUpper(const wchar_t value) noexcept
{
    if (value >= L'a' && value <= L'z') {
        return static_cast<wchar_t>(value - L'a' + L'A');
    }
    return value;
}

[[nodiscard]] bool equalAsciiIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (asciiUpper(left[index]) != asciiUpper(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isReservedDeviceComponent(
    const std::wstring_view component) noexcept
{
    const std::size_t dot = component.find(L'.');
    const std::wstring_view base = component.substr(0U, dot);
    if (equalAsciiIgnoreCase(base, L"CON") ||
        equalAsciiIgnoreCase(base, L"PRN") ||
        equalAsciiIgnoreCase(base, L"AUX") ||
        equalAsciiIgnoreCase(base, L"NUL") ||
        equalAsciiIgnoreCase(base, L"CONIN$") ||
        equalAsciiIgnoreCase(base, L"CONOUT$")) {
        return true;
    }
    return base.size() == 4U && base[3U] >= L'1' && base[3U] <= L'9' &&
        (equalAsciiIgnoreCase(base.substr(0U, 3U), L"COM") ||
         equalAsciiIgnoreCase(base.substr(0U, 3U), L"LPT"));
}

[[nodiscard]] bool isForbiddenPathCharacter(const wchar_t value) noexcept
{
    return value < 0x20 || value == L'<' || value == L'>' ||
        value == L'"' || value == L'|' || value == L'?' ||
        value == L'*' || value == L':';
}

[[nodiscard]] bool isValidPathComponent(
    const std::wstring_view component) noexcept
{
    if (component.empty() || component == L"." || component == L".." ||
        component.back() == L' ' || component.back() == L'.' ||
        isReservedDeviceComponent(component)) {
        return false;
    }
    for (const wchar_t character : component) {
        if (isForbiddenPathCharacter(character)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isAsciiDriveLetter(const wchar_t value) noexcept
{
    return (value >= L'A' && value <= L'Z') ||
        (value >= L'a' && value <= L'z');
}

[[nodiscard]] Domain::Result<void> validateAbsoluteLocalHome(
    const std::wstring_view value) noexcept
{
    if (value.size() < 3U || !isAsciiDriveLetter(value[0U]) ||
        value[1U] != L':' || value[2U] != L'\\' ||
        value.find(L'/') != std::wstring_view::npos ||
        (value.size() > 3U && value.back() == L'\\')) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "--home must be an absolute local Windows drive path."));
    }

    std::size_t start = 3U;
    while (start < value.size()) {
        const std::size_t separator = value.find(L'\\', start);
        const std::size_t end = separator == std::wstring_view::npos
            ? value.size()
            : separator;
        if (!isValidPathComponent(value.substr(start, end - start))) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "--home contains an empty, relative, reserved, or forbidden "
                "Windows path component."));
        }
        if (separator == std::wstring_view::npos) {
            break;
        }
        start = separator + 1U;
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<Domain::PathText> parseHome(
    const std::wstring_view value) noexcept
{
    auto converted = strictUtf16ToUtf8(value);
    if (!converted) {
        return Domain::Result<Domain::PathText>::failure(
            std::move(converted).error());
    }
    auto validated = validateAbsoluteLocalHome(value);
    if (!validated) {
        return Domain::Result<Domain::PathText>::failure(
            std::move(validated).error());
    }
    if (converted.value().size() > Domain::PathText::MaximumBytes) {
        return failure<Domain::PathText>(
            Domain::ErrorCodes::LimitExceeded,
            "--home exceeds the UTF-8 path bound.");
    }
    return Domain::PathText::create(converted.value());
}

} // namespace

Domain::Result<ManagerProcessArguments> ManagerProcessArguments::parse(
    const std::span<const std::wstring_view> arguments) noexcept
{
    try {
        if (arguments.size() > MaximumInspectedArgumentCount) {
            return failure<ManagerProcessArguments>(
                Domain::ErrorCodes::LimitExceeded,
                "The Manager process argument count exceeds its bound.");
        }

        ManagerProcessArguments parsed;
        for (std::size_t index = 0U; index < arguments.size(); ++index) {
            const auto& argument = arguments[index];
            auto strictArgument = strictUtf16ToUtf8(argument);
            if (!strictArgument) {
                return Domain::Result<ManagerProcessArguments>::failure(
                    std::move(strictArgument).error());
            }

            if (argument == L"--open") {
                if (parsed.openBrowser) {
                    return failure<ManagerProcessArguments>(
                        Domain::ErrorCodes::InvalidRequest,
                        "--open may be specified only once.");
                }
                parsed.openBrowser = true;
                continue;
            }

            if (argument == L"--home") {
                if (parsed.expectedHome.has_value()) {
                    return failure<ManagerProcessArguments>(
                        Domain::ErrorCodes::InvalidRequest,
                        "--home may be specified only once.");
                }
                if (index + 1U >= arguments.size() ||
                    arguments[index + 1U].starts_with(L"--")) {
                    return failure<ManagerProcessArguments>(
                        Domain::ErrorCodes::InvalidRequest,
                        "--home requires one absolute Windows path value.");
                }
                auto home = parseHome(arguments[++index]);
                if (!home) {
                    return Domain::Result<ManagerProcessArguments>::failure(
                        std::move(home).error());
                }
                parsed.expectedHome = std::move(home).value();
                continue;
            }

            return failure<ManagerProcessArguments>(
                Domain::ErrorCodes::InvalidRequest,
                "Unknown Manager process argument.");
        }
        return Domain::Result<ManagerProcessArguments>::success(
            std::move(parsed));
    } catch (...) {
        return failure<ManagerProcessArguments>(
            Domain::ErrorCodes::InternalFailure,
            "Manager process arguments could not be parsed within their "
            "resource bound.");
    }
}

} // namespace ForgeConductor::Hosts::Manager
