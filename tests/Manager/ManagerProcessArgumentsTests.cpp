#include "ManagerProcessArguments.h"

#include "ForgeConductor/Domain/Error.h"

#include <array>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Host = ForgeConductor::Hosts::Manager;

static_assert(std::is_final_v<Host::ManagerProcessArguments>);

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw TestFailure{message};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result, const std::string& action)
{
    if (!result) {
        throw TestFailure{
            action + " failed: " + result.error().code + ": " +
            result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
void requireError(
    Domain::Result<T> result,
    const std::string_view code,
    const std::string& action)
{
    require(!result.hasValue(), action + " unexpectedly succeeded");
    require(
        result.error().code == code,
        action + " returned " + result.error().code + " instead of " +
            std::string{code});
}

void emptyAndOpenArgumentsParse()
{
    const auto empty = take(
        Host::ManagerProcessArguments::parse({}),
        "parse empty arguments");
    require(!empty.expectedHome.has_value(), "empty input asserted a home");
    require(!empty.openBrowser, "empty input requested a browser");

    constexpr std::array<std::wstring_view, 1U> open{L"--open"};
    const auto parsedOpen = take(
        Host::ManagerProcessArguments::parse(open),
        "parse --open");
    require(
        !parsedOpen.expectedHome.has_value(),
        "--open unexpectedly asserted a home");
    require(parsedOpen.openBrowser, "--open did not request a browser");
}

void homeAndOrderParse()
{
    constexpr std::array<std::wstring_view, 3U> homeThenOpen{
        L"--home", L"C:\\Users\\J\u00e4mes\\Forge Conductor", L"--open"};
    const auto first = take(
        Host::ManagerProcessArguments::parse(homeThenOpen),
        "parse home then open");
    require(first.expectedHome.has_value(), "home assertion was not retained");
    require(
        first.expectedHome->value() ==
            "C:\\Users\\J\xc3\xa4mes\\Forge Conductor",
        "home assertion was not converted to strict UTF-8");
    require(first.openBrowser, "ordered arguments lost --open");

    constexpr std::array<std::wstring_view, 3U> openThenHome{
        L"--open", L"--home", L"D:\\Forge"};
    const auto second = take(
        Host::ManagerProcessArguments::parse(openThenHome),
        "parse open then home");
    require(
        second.expectedHome.has_value() &&
            second.expectedHome->value() == "D:\\Forge",
        "reordered home assertion was not retained");
    require(second.openBrowser, "reordered arguments lost --open");
}

void unknownDuplicateAndMissingArgumentsFail()
{
    constexpr std::array<std::wstring_view, 1U> legacyAlias{
        L"--open-browser"};
    requireError(
        Host::ManagerProcessArguments::parse(legacyAlias),
        Domain::ErrorCodes::InvalidRequest,
        "noncanonical browser alias");

    constexpr std::array<std::wstring_view, 2U> duplicateOpen{
        L"--open", L"--open"};
    requireError(
        Host::ManagerProcessArguments::parse(duplicateOpen),
        Domain::ErrorCodes::InvalidRequest,
        "duplicate --open");

    constexpr std::array<std::wstring_view, 4U> duplicateHome{
        L"--home", L"C:\\One", L"--home", L"C:\\Two"};
    requireError(
        Host::ManagerProcessArguments::parse(duplicateHome),
        Domain::ErrorCodes::InvalidRequest,
        "duplicate --home");

    constexpr std::array<std::wstring_view, 1U> missingHome{L"--home"};
    requireError(
        Host::ManagerProcessArguments::parse(missingHome),
        Domain::ErrorCodes::InvalidRequest,
        "missing --home value");

    constexpr std::array<std::wstring_view, 2U> flagAsHome{
        L"--home", L"--open"};
    requireError(
        Host::ManagerProcessArguments::parse(flagAsHome),
        Domain::ErrorCodes::InvalidRequest,
        "flag used as --home value");
}

void malformedAndRelativeHomesFail()
{
    constexpr std::array<std::wstring_view, 8U> invalidHomes{
        L"relative\\home",
        L"C:drive-relative",
        L"\\root-relative",
        L"\\\\server\\share",
        L"C:/forward/slashes",
        L"C:\\Forge\\..\\Other",
        L"C:\\Forge\\NUL",
        L"C:\\Forge\\trailing."};
    for (const auto invalidHome : invalidHomes) {
        const std::array arguments{std::wstring_view{L"--home"}, invalidHome};
        requireError(
            Host::ManagerProcessArguments::parse(arguments),
            Domain::ErrorCodes::InvalidRequest,
            "malformed or relative --home");
    }

    const std::wstring embeddedNull{L'C', L':', L'\\', L'A', L'\0', L'B'};
    const std::array embeddedNullArguments{
        std::wstring_view{L"--home"}, std::wstring_view{embeddedNull}};
    requireError(
        Host::ManagerProcessArguments::parse(embeddedNullArguments),
        Domain::ErrorCodes::InvalidRequest,
        "embedded-NUL --home");
}

void malformedUtf16AndBoundsFail()
{
    const std::wstring malformedFlag(1U, static_cast<wchar_t>(0xd800));
    const std::array malformedFlagArguments{std::wstring_view{malformedFlag}};
    requireError(
        Host::ManagerProcessArguments::parse(malformedFlagArguments),
        Domain::ErrorCodes::InvalidRequest,
        "malformed UTF-16 flag");

    std::wstring malformedHome{L"C:\\Forge\\"};
    malformedHome.push_back(static_cast<wchar_t>(0xdc00));
    const std::array malformedHomeArguments{
        std::wstring_view{L"--home"}, std::wstring_view{malformedHome}};
    requireError(
        Host::ManagerProcessArguments::parse(malformedHomeArguments),
        Domain::ErrorCodes::InvalidRequest,
        "malformed UTF-16 home");

    const std::wstring oversizedHome(
        Host::ManagerProcessArguments::MaximumHomeUtf16Units + 1U,
        L'x');
    const std::array oversizedHomeArguments{
        std::wstring_view{L"--home"}, std::wstring_view{oversizedHome}};
    requireError(
        Host::ManagerProcessArguments::parse(oversizedHomeArguments),
        Domain::ErrorCodes::LimitExceeded,
        "oversized UTF-16 home");

    std::wstring oversizedUtf8Home{L"C:\\"};
    oversizedUtf8Home.append(10'923U, L'\u0800');
    const std::array oversizedUtf8Arguments{
        std::wstring_view{L"--home"}, std::wstring_view{oversizedUtf8Home}};
    requireError(
        Host::ManagerProcessArguments::parse(oversizedUtf8Arguments),
        Domain::ErrorCodes::LimitExceeded,
        "oversized UTF-8 home");

    constexpr std::array<std::wstring_view, 5U> excessiveArguments{
        L"--open", L"--open", L"--open", L"--open", L"--open"};
    requireError(
        Host::ManagerProcessArguments::parse(excessiveArguments),
        Domain::ErrorCodes::LimitExceeded,
        "excessive process argument count");
}

} // namespace

int main()
{
    try {
        emptyAndOpenArgumentsParse();
        homeAndOrderParse();
        unknownDuplicateAndMissingArgumentsFail();
        malformedAndRelativeHomesFail();
        malformedUtf16AndBoundsFail();
        std::cout << "Manager process argument tests passed: 5 groups\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager process argument tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager process argument tests failed with an unknown "
                     "error.\n";
        return 1;
    }
}
