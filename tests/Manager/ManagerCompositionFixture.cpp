#include "ManagerCompositionRoot.h"

#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/FileSystemModels.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Host = ForgeConductor::Hosts::Manager;

constexpr int InvalidArgumentsExitCode = 2;
constexpr std::size_t MaximumFixtureArgumentCharacters = 32U * 1024U;

template <typename Value>
[[nodiscard]] Domain::Result<Value> failure(
    const std::string_view code,
    std::string message)
{
    return Domain::Result<Value>::failure(
        Domain::makeError(code, std::move(message)));
}

[[nodiscard]] Domain::Result<std::string> strictUtf16ToUtf8(
    const std::wstring_view value) noexcept
{
    try {
        if (value.empty() ||
            value.size() > MaximumFixtureArgumentCharacters ||
            value.size() >
                static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
            value.find(L'\0') != std::wstring_view::npos) {
            return failure<std::string>(
                Domain::ErrorCodes::InvalidRequest,
                "A Manager composition fixture argument is empty or invalid.");
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
                "A Manager composition fixture argument is not valid UTF-16.");
        }

        std::string converted(static_cast<std::size_t>(required), '\0');
        if (::WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.data(),
                inputLength,
                converted.data(),
                required,
                nullptr,
                nullptr) != required) {
            return failure<std::string>(
                Domain::ErrorCodes::InvalidRequest,
                "A Manager composition fixture argument could not be converted.");
        }
        return Domain::Result<std::string>::success(std::move(converted));
    } catch (...) {
        return failure<std::string>(
            Domain::ErrorCodes::InternalFailure,
            "A Manager composition fixture argument could not be converted within its bound.");
    }
}

void writeError(
    const std::string_view prefix,
    const Domain::Error& error)
{
    std::cerr << prefix << ": " << error.code << ": " << error.message
              << '\n';
}

} // namespace

int wmain(const int argc, wchar_t** const argv)
{
    if (argc != 4 || argv == nullptr || argv[1] == nullptr ||
        argv[2] == nullptr || argv[3] == nullptr) {
        std::cerr
            << "Usage: ForgeConductor.Manager.exe <isolated-data-root> "
               "<instance-purpose-suffix> <isolated-registry-subkey>\n";
        return InvalidArgumentsExitCode;
    }

    try {
        auto rootText = strictUtf16ToUtf8(argv[1]);
        auto purposeSuffix = strictUtf16ToUtf8(argv[2]);
        const std::wstring_view registrySubkey{argv[3]};
        if (!rootText || !purposeSuffix || registrySubkey.empty() ||
            registrySubkey.size() > MaximumFixtureArgumentCharacters ||
            registrySubkey.find(L'\0') != std::wstring_view::npos) {
            const auto error = !rootText
                ? rootText.error()
                : (!purposeSuffix
                       ? purposeSuffix.error()
                       : Domain::makeError(
                             Domain::ErrorCodes::InvalidRequest,
                             "The isolated registry subkey is empty or invalid."));
            writeError("Manager composition fixture argument error", error);
            return InvalidArgumentsExitCode;
        }

        auto dataRoot = Domain::PathText::create(rootText.value());
        if (!dataRoot) {
            writeError(
                "Manager composition fixture argument error",
                dataRoot.error());
            return InvalidArgumentsExitCode;
        }

        Host::ManagerCompositionRootOptions options;
        options.environment.explicitDataRoot = std::move(dataRoot).value();
        options.environment.allowEnvironmentOverride = false;
        options.instanceLease.purposeSuffix =
            std::move(purposeSuffix).value();
        options.secureStorageRegistrySubkey =
            std::wstring{registrySubkey};
        options.enableExternalHostMaintenance = false;
        options.openBrowserOverride = false;
        options.enableEtw = false;

        auto created = Host::ManagerCompositionRoot::create(
            std::move(options));
        if (!created) {
            writeError(
                "Manager composition fixture startup failed",
                created.error());
            return EXIT_FAILURE;
        }

        auto root = std::move(created).value();
        auto outcome = root->run();
        if (!outcome) {
            writeError(
                "Manager composition fixture runtime failed",
                outcome.error());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception&) {
        std::cerr << "Manager composition fixture failed safely.\n";
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Manager composition fixture failed safely.\n";
        return EXIT_FAILURE;
    }
}
