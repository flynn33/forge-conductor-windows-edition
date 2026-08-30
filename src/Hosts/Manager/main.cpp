#include "ManagerCompositionRoot.h"
#include "ManagerProcessArguments.h"

#include "ForgeConductor/Domain/Error.h"

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Host = ForgeConductor::Hosts::Manager;

constexpr int InvalidArgumentsExitCode = 2;

void writeError(
    const std::string_view prefix,
    const Domain::Error& error)
{
    std::cerr << prefix << ": " << error.code << ": " << error.message
              << '\n';
}

[[nodiscard]] Domain::Result<Host::ManagerProcessArguments> parseArguments(
    const int argumentCount,
    wchar_t** const argumentValues)
{
    if (argumentCount < 1 || argumentValues == nullptr) {
        return Domain::Result<Host::ManagerProcessArguments>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The Manager process received an invalid native argument "
                "vector."));
    }

    const auto processArgumentCount =
        static_cast<std::size_t>(argumentCount - 1);
    if (processArgumentCount >
        Host::ManagerProcessArguments::MaximumInspectedArgumentCount) {
        return Domain::Result<Host::ManagerProcessArguments>::failure(
            Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The Manager process argument count exceeds its bound."));
    }

    std::array<
        std::wstring_view,
        Host::ManagerProcessArguments::MaximumInspectedArgumentCount>
        arguments{};
    for (std::size_t index = 0U; index < processArgumentCount; ++index) {
        const auto* value = argumentValues[index + 1U];
        if (value == nullptr) {
            return Domain::Result<Host::ManagerProcessArguments>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The Manager process received a null native argument."));
        }
        arguments[index] = std::wstring_view{value};
    }

    return Host::ManagerProcessArguments::parse(
        std::span<const std::wstring_view>{
            arguments.data(), processArgumentCount});
}

} // namespace

int wmain(const int argc, wchar_t** const argv)
{
    try {
        auto parsed = parseArguments(argc, argv);
        if (!parsed) {
            writeError("Forge Conductor Manager argument error", parsed.error());
            return InvalidArgumentsExitCode;
        }

        auto processArguments = std::move(parsed).value();
        Host::ManagerCompositionRootOptions options;
        options.expectedHome = std::move(processArguments.expectedHome);
        options.openBrowserOverride = processArguments.openBrowser;

        auto created = Host::ManagerCompositionRoot::create(
            std::move(options));
        if (!created) {
            writeError(
                "Forge Conductor Manager startup failed", created.error());
            return EXIT_FAILURE;
        }

        auto root = std::move(created).value();
        auto outcome = root->run();
        if (!outcome) {
            writeError(
                "Forge Conductor Manager runtime failed", outcome.error());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception&) {
        std::cerr << "Forge Conductor Manager failed safely.\n";
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Forge Conductor Manager failed safely.\n";
        return EXIT_FAILURE;
    }
}
