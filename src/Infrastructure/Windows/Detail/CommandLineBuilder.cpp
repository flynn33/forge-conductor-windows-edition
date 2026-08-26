#include "CommandLineBuilder.h"

#include "ForgeConductor/Domain/Error.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

class EnvironmentStrings final {
public:
    explicit EnvironmentStrings(wchar_t* value) noexcept : value_{value} {}
    ~EnvironmentStrings() noexcept
    {
        if (value_ != nullptr) {
            static_cast<void>(::FreeEnvironmentStringsW(value_));
        }
    }

    EnvironmentStrings(const EnvironmentStrings&) = delete;
    EnvironmentStrings& operator=(const EnvironmentStrings&) = delete;

    [[nodiscard]] const wchar_t* get() const noexcept { return value_; }

private:
    wchar_t* value_{};
};

struct EnvironmentNameLess final {
    [[nodiscard]] bool operator()(const std::wstring& left,
                                  const std::wstring& right) const noexcept
    {
        const auto comparison =
            ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                   static_cast<int>(right.size()), TRUE);
        if (comparison == CSTR_LESS_THAN) {
            return true;
        }
        if (comparison == CSTR_GREATER_THAN || comparison == CSTR_EQUAL) {
            return false;
        }
        return left < right;
    }
};

constexpr std::array<std::wstring_view, 4U> AllowedInheritedEnvironmentNames{
    L"SystemRoot", L"WINDIR", L"TEMP", L"TMP"};

constexpr std::array<std::wstring_view, 9U> CredentialEnvironmentMarkers{
    L"API_KEY", L"AUTH",        L"COOKIE", L"CREDENTIAL", L"PASSWORD",
    L"PASSWD",  L"PRIVATE_KEY", L"SECRET", L"TOKEN"};

[[nodiscard]] constexpr wchar_t asciiUpper(const wchar_t character) noexcept
{
    return character >= L'a' && character <= L'z' ? static_cast<wchar_t>(character - (L'a' - L'A'))
                                                  : character;
}

[[nodiscard]] bool containsAsciiMarker(const std::wstring_view value,
                                       const std::wstring_view marker) noexcept
{
    if (marker.empty() || marker.size() > value.size()) {
        return false;
    }
    for (std::size_t start = 0U; start <= value.size() - marker.size(); ++start) {
        bool matches = true;
        for (std::size_t index = 0U; index < marker.size(); ++index) {
            if (asciiUpper(value[start + index]) != marker[index]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool ordinalEnvironmentNameEqual(const std::wstring_view left,
                                               const std::wstring_view right) noexcept
{
    return left.size() <= static_cast<std::size_t>(INT_MAX) &&
           right.size() <= static_cast<std::size_t>(INT_MAX) &&
           ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool isSafeInheritedEnvironmentName(const std::wstring_view name) noexcept
{
    if (name.empty() || name.starts_with(L'=') || name.find(L'=') != std::wstring_view::npos) {
        return false;
    }
    for (const auto marker : CredentialEnvironmentMarkers) {
        if (containsAsciiMarker(name, marker)) {
            return false;
        }
    }
    return std::any_of(
        AllowedInheritedEnvironmentNames.begin(), AllowedInheritedEnvironmentNames.end(),
        [name](const auto allowed) noexcept { return ordinalEnvironmentNameEqual(name, allowed); });
}

[[nodiscard]] Domain::Error invalidUtf8()
{
    return Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                             "Process text must be well-formed UTF-8.");
}

[[nodiscard]] Domain::Result<std::wstring> convertUtf8(const std::string_view value)
{
    if (value.empty()) {
        return Domain::Result<std::wstring>::success({});
    }
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
        return Domain::Result<std::wstring>::failure(invalidUtf8());
    }

    const auto inputLength = static_cast<int>(value.size());
    const auto required =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength, nullptr, 0);
    if (required <= 0) {
        return Domain::Result<std::wstring>::failure(invalidUtf8());
    }

    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength,
                              converted.data(), required) != required) {
        return Domain::Result<std::wstring>::failure(invalidUtf8());
    }
    return Domain::Result<std::wstring>::success(std::move(converted));
}

[[nodiscard]] std::wstring quoteArgument(const std::wstring_view argument)
{
    const bool requiresQuotes =
        argument.empty() || argument.find_first_of(L" \t\"") != std::wstring_view::npos;
    if (!requiresQuotes) {
        return std::wstring{argument};
    }

    std::wstring quoted;
    quoted.reserve(argument.size() + 2U);
    quoted.push_back(L'"');
    std::size_t backslashes = 0U;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append((backslashes * 2U) + 1U, L'\\');
            quoted.push_back(L'"');
            backslashes = 0U;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0U;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

[[nodiscard]] Domain::Result<void> appendEnvironmentEntry(std::vector<wchar_t>& block,
                                                          const std::wstring& name,
                                                          const std::wstring& value)
{
    constexpr auto maximum =
        Domain::MaximumProcessEnvironmentBlockUtf16CodeUnitsIncludingTerminators;
    const auto entryUnits = name.size() + 1U + value.size() + 1U;
    if (entryUnits > maximum || block.size() > maximum - entryUnits) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "The final process environment block exceeds the Forge UTF-16 limit."));
    }
    block.insert(block.end(), name.begin(), name.end());
    block.push_back(L'=');
    block.insert(block.end(), value.begin(), value.end());
    block.push_back(L'\0');
    return Domain::Result<void>::success();
}

} // namespace

Domain::Result<std::wstring> CommandLineBuilder::utf8ToUtf16(const std::string_view value)
{
    return convertUtf8(value);
}

Domain::Result<std::vector<wchar_t>>
CommandLineBuilder::buildCommandLine(const std::wstring_view absoluteApplicationName,
                                     const std::vector<std::string>& arguments)
{
    if (absoluteApplicationName.empty()) {
        return Domain::Result<std::vector<wchar_t>>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "The process application name must not be empty."));
    }

    std::wstring commandLine = quoteArgument(absoluteApplicationName);
    for (const auto& argument : arguments) {
        auto converted = convertUtf8(argument);
        if (!converted) {
            return Domain::Result<std::vector<wchar_t>>::failure(std::move(converted).error());
        }
        commandLine.push_back(L' ');
        commandLine.append(quoteArgument(converted.value()));
        if (commandLine.size() + 1U >
            Domain::MaximumProcessCommandLineUtf16CodeUnitsIncludingTerminator) {
            return Domain::Result<std::vector<wchar_t>>::failure(
                Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                  "The final quoted process command line exceeds "
                                  "32767 UTF-16 code units."));
        }
    }

    if (commandLine.size() + 1U >
        Domain::MaximumProcessCommandLineUtf16CodeUnitsIncludingTerminator) {
        return Domain::Result<std::vector<wchar_t>>::failure(
            Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                              "The final quoted process command line exceeds 32767 "
                              "UTF-16 code units."));
    }

    std::vector<wchar_t> mutableCommandLine{commandLine.begin(), commandLine.end()};
    mutableCommandLine.push_back(L'\0');
    return Domain::Result<std::vector<wchar_t>>::success(std::move(mutableCommandLine));
}

Domain::Result<std::vector<EnvironmentEntry>> CommandLineBuilder::readCurrentEnvironment()
{
    EnvironmentStrings environment{::GetEnvironmentStringsW()};
    if (environment.get() == nullptr) {
        return Domain::Result<std::vector<EnvironmentEntry>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "GetEnvironmentStringsW failed with Win32 error " +
                                                     std::to_string(::GetLastError()) + "."));
    }

    constexpr auto maximum =
        Domain::MaximumProcessEnvironmentBlockUtf16CodeUnitsIncludingTerminators;
    std::vector<EnvironmentEntry> entries;
    std::size_t scannedUnits = 1U;
    for (const auto* cursor = environment.get(); *cursor != L'\0';) {
        std::size_t entryLength{};
        while (cursor[entryLength] != L'\0') {
            if (scannedUnits >= maximum) {
                return Domain::Result<std::vector<EnvironmentEntry>>::failure(
                    Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                      "The inherited process environment exceeds the "
                                      "Forge UTF-16 limit."));
            }
            ++entryLength;
            ++scannedUnits;
        }
        if (scannedUnits >= maximum) {
            return Domain::Result<std::vector<EnvironmentEntry>>::failure(
                Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                  "The inherited process environment exceeds the "
                                  "Forge UTF-16 limit."));
        }
        ++scannedUnits;

        const std::wstring entry{cursor, entryLength};
        cursor += entryLength + 1U;
        const auto separator = entry.find(L'=');
        if (separator == std::wstring::npos || separator == 0U) {
            continue;
        }
        const auto name = std::wstring_view{entry}.substr(0U, separator);
        if (!isSafeInheritedEnvironmentName(name)) {
            continue;
        }
        entries.push_back(EnvironmentEntry{std::wstring{name}, entry.substr(separator + 1U)});
    }
    return Domain::Result<std::vector<EnvironmentEntry>>::success(std::move(entries));
}

Domain::Result<std::vector<wchar_t>> CommandLineBuilder::buildEnvironmentBlock(
    const Domain::ProcessRequest& request,
    const std::span<const EnvironmentEntry> inheritedEnvironment)
{
    std::map<std::wstring, std::wstring, EnvironmentNameLess> entries;
    if (request.inheritEnvironment) {
        for (const auto& entry : inheritedEnvironment) {
            if (!isSafeInheritedEnvironmentName(entry.name)) {
                continue;
            }
            entries.insert_or_assign(entry.name, entry.value);
        }
    }

    for (const auto& variable : request.environment) {
        auto name = convertUtf8(variable.name);
        if (!name) {
            return Domain::Result<std::vector<wchar_t>>::failure(std::move(name).error());
        }
        auto value = convertUtf8(variable.value);
        if (!value) {
            return Domain::Result<std::vector<wchar_t>>::failure(std::move(value).error());
        }
        entries.insert_or_assign(std::move(name).value(), std::move(value).value());
    }

    if (entries.empty()) {
        return Domain::Result<std::vector<wchar_t>>::success(std::vector<wchar_t>{L'\0', L'\0'});
    }

    std::vector<wchar_t> block;
    for (const auto& [name, value] : entries) {
        auto appended = appendEnvironmentEntry(block, name, value);
        if (!appended) {
            return Domain::Result<std::vector<wchar_t>>::failure(std::move(appended).error());
        }
    }

    constexpr auto maximum =
        Domain::MaximumProcessEnvironmentBlockUtf16CodeUnitsIncludingTerminators;
    if (block.size() >= maximum) {
        return Domain::Result<std::vector<wchar_t>>::failure(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "The final process environment block exceeds the Forge UTF-16 limit."));
    }
    block.push_back(L'\0');
    return Domain::Result<std::vector<wchar_t>>::success(std::move(block));
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
