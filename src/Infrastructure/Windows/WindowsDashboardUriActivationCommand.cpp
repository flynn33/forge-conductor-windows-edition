#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardUriActivationCommand.h"

#include "Detail/IWindowsDashboardUriLaunchPlatform.h"
#include "Detail/UtfConversion.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::string_view Ipv4Prefix{"http://127.0.0.1:"};
constexpr std::string_view Ipv6Prefix{"http://[::1]:"};
constexpr std::string_view TokenMarker{"/#token="};

[[nodiscard]] Domain::Error commandError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] bool isLowerHex(const char value) noexcept
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f');
}

[[nodiscard]] Domain::Result<void> validateUri(
    const std::string_view uri) noexcept
{
    try {
        const std::size_t portStart = uri.starts_with(Ipv4Prefix)
            ? Ipv4Prefix.size()
            : uri.starts_with(Ipv6Prefix) ? Ipv6Prefix.size()
                                          : std::string_view::npos;
        if (portStart == std::string_view::npos) {
            return Domain::Result<void>::failure(commandError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard activation helper accepts only canonical loopback HTTP URIs."));
        }

        const std::size_t marker = uri.find(TokenMarker, portStart);
        if (marker == std::string_view::npos || marker == portStart) {
            return Domain::Result<void>::failure(commandError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard activation helper URI is missing its canonical endpoint."));
        }

        const std::string_view portText =
            uri.substr(portStart, marker - portStart);
        if (portText.size() > 5U ||
            (portText.size() > 1U && portText.front() == '0')) {
            return Domain::Result<void>::failure(commandError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard activation helper URI has an invalid port."));
        }
        std::uint32_t port{};
        const auto parsed = std::from_chars(
            portText.data(), portText.data() + portText.size(), port);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != portText.data() + portText.size() ||
            port == 0U || port > 65'535U) {
            return Domain::Result<void>::failure(commandError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard activation helper URI has an invalid port."));
        }

        const std::string_view token = uri.substr(marker + TokenMarker.size());
        if (token.size() != 64U ||
            uri.find(TokenMarker, marker + TokenMarker.size()) !=
                std::string_view::npos) {
            return Domain::Result<void>::failure(commandError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard activation helper URI has an invalid bearer fragment."));
        }
        for (const char character : token) {
            if (!isLowerHex(character)) {
                return Domain::Result<void>::failure(commandError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The dashboard activation helper URI has an invalid bearer fragment."));
            }
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(commandError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard activation helper URI could not be validated safely."));
    }
}

} // namespace

class WindowsDashboardUriActivationCommand::Impl final {
public:
    explicit Impl(
        std::unique_ptr<Detail::IWindowsDashboardUriLaunchPlatform> platform)
        : platform_{std::move(platform)}
    {
    }

    [[nodiscard]] Domain::Result<void> run(std::istream& input) noexcept
    {
        try {
            std::array<char,
                       WindowsDashboardUriActivationCommand::MaximumUriBytes +
                           1U>
                buffer{};
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count <= 0) {
                return Domain::Result<void>::failure(commandError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The dashboard activation helper received no URI."));
            }
            if (static_cast<std::size_t>(count) >
                WindowsDashboardUriActivationCommand::MaximumUriBytes) {
                return Domain::Result<void>::failure(commandError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The dashboard activation helper URI exceeded its bounded input."));
            }
            if (input.bad()) {
                return Domain::Result<void>::failure(commandError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard activation helper could not read standard input.",
                    true));
            }

            const std::string uri{
                buffer.data(), static_cast<std::size_t>(count)};
            auto valid = validateUri(uri);
            if (!valid) {
                return valid;
            }
            if (!platform_) {
                return Domain::Result<void>::failure(commandError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The dashboard activation helper has no Windows Shell boundary."));
            }
            auto wide = Detail::strictUtf8ToUtf16(uri);
            if (!wide) {
                return Domain::Result<void>::failure(std::move(wide).error());
            }
            auto opened = platform_->open(wide.value());
            if (!opened) {
                // The CLI emits only the code, but also sanitize this boundary
                // so a platform adapter can never reflect the URI through
                // either an unconstrained code or its error text.
                return Domain::Result<void>::failure(commandError(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "The Windows Shell could not open the registered Manager dashboard URI.",
                    opened.error().retryable));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(commandError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard activation helper failed safely."));
        }
    }

private:
    const std::unique_ptr<Detail::IWindowsDashboardUriLaunchPlatform> platform_;
};

WindowsDashboardUriActivationCommand::WindowsDashboardUriActivationCommand()
    : WindowsDashboardUriActivationCommand{
          Detail::createWindowsDashboardUriLaunchPlatform()}
{
}

WindowsDashboardUriActivationCommand::WindowsDashboardUriActivationCommand(
    std::unique_ptr<Detail::IWindowsDashboardUriLaunchPlatform> platform)
    : implementation_{std::make_unique<Impl>(std::move(platform))}
{
}

WindowsDashboardUriActivationCommand::~WindowsDashboardUriActivationCommand()
    noexcept = default;

Domain::Result<void> WindowsDashboardUriActivationCommand::run(
    std::istream& input) noexcept
{
    if (!implementation_) {
        return Domain::Result<void>::failure(commandError(
            Domain::ErrorCodes::TransportClosed,
            "The dashboard activation helper is unavailable."));
    }
    return implementation_->run(input);
}

} // namespace ForgeConductor::Infrastructure::Windows
