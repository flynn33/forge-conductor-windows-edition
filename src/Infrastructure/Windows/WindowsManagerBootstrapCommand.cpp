#include "ForgeConductor/Infrastructure/Windows/WindowsManagerBootstrapCommand.h"

#include "Detail/IWindowsManagerBootstrapPlatform.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {
[[nodiscard]] Domain::Result<void> invalid(const char* message)
{
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::InvalidRequest, message));
}
}

WindowsManagerBootstrapCommand::WindowsManagerBootstrapCommand()
    : WindowsManagerBootstrapCommand{Detail::createWindowsManagerBootstrapPlatform()}
{
}

WindowsManagerBootstrapCommand::WindowsManagerBootstrapCommand(
    std::unique_ptr<Detail::IWindowsManagerBootstrapPlatform> platform)
    : platform_{std::move(platform)}
{
}

WindowsManagerBootstrapCommand::~WindowsManagerBootstrapCommand() noexcept = default;

Domain::Result<void> WindowsManagerBootstrapCommand::run(std::istream& input) noexcept
{
    try {
        std::array<char, MaximumRequestBytes + 1U> buffer{};
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (input.bad()) return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::TransportClosed, "The Manager bootstrap could not read its request."));
        if (count <= 0) return invalid("The Manager bootstrap received no request.");
        if (static_cast<std::size_t>(count) > MaximumRequestBytes)
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "The Manager bootstrap request exceeded its bound."));
        const auto request = nlohmann::json::parse(buffer.data(), buffer.data() + count, nullptr, false);
        if (!request.is_object() || request.size() != 2U ||
            !request.contains("home") || !request.at("home").is_string() ||
            !request.contains("isolated_profile") || !request.at("isolated_profile").is_boolean())
            return invalid("The Manager bootstrap requires only home and isolated_profile.");
        const auto text = request.at("home").get<std::string>();
        auto home = Domain::PathText::create(text);
        if (!home || !Domain::isValidUtf8(text))
            return invalid("The Manager bootstrap home is not bounded UTF-8 path text.");
        if (!platform_) return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure, "The Manager bootstrap has no Windows Shell boundary."));
        auto started = platform_->start(home.value(), request.at("isolated_profile").get<bool>());
        if (!started) return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "The Windows desktop Shell could not start the matching Manager.", started.error().retryable));
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "The Manager bootstrap failed safely."));
    }
}

Domain::Result<void> WindowsManagerBootstrapCommand::launch(
    const std::span<const std::string_view> arguments) noexcept
{
    try {
        if (arguments.size() != 3U || arguments[0] != "--internal-launch-manager" ||
            (arguments[1] != "--home" && arguments[1] != "--alpha-root"))
            return invalid("The detached Manager launch requires its fixed command, profile mode, and home.");
        auto home = Domain::PathText::create(arguments[2]);
        if (!home || !Domain::isValidUtf8(arguments[2]))
            return invalid("The Manager bootstrap home is not bounded UTF-8 path text.");
        if (!platform_) return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure, "The Manager bootstrap has no Windows launch boundary."));
        auto started = platform_->start(home.value(), arguments[1] == "--alpha-root",
            Detail::ManagerBootstrapLaunchMode::DetachedManager);
        if (!started) return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "The detached CLI could not start the matching Manager.", started.error().retryable));
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "The detached Manager launch failed safely."));
    }
}
}
