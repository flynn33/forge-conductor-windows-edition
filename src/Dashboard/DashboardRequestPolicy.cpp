#include "ForgeConductor/Dashboard/DashboardRequestPolicy.h"

#include "ForgeConductor/Contracts/IManagerAuthentication.h"
#include "ForgeConductor/Domain/Error.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Dashboard {
namespace {

struct HeaderLookup final {
    std::optional<std::string_view> value;
    bool duplicate{};
};

[[nodiscard]] HeaderLookup findHeader(
    const DashboardHttpRequest& request,
    const std::string_view lowercaseName) noexcept
{
    HeaderLookup result;
    for (const auto& header : request.headers()) {
        if (header.name != lowercaseName) {
            continue;
        }
        if (result.value.has_value()) {
            result.duplicate = true;
            continue;
        }
        result.value = header.value;
    }
    return result;
}

[[nodiscard]] constexpr char lowerAscii(const char value) noexcept
{
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A'))
        : value;
}

[[nodiscard]] bool equalsAsciiCaseInsensitive(
    const std::string_view left,
    const std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (lowerAscii(left[index]) != lowerAscii(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool isOptionalWhitespace(const char value) noexcept
{
    return value == ' ' || value == '\t';
}

[[nodiscard]] std::string_view trimOptionalWhitespace(
    std::string_view value) noexcept
{
    while (!value.empty() && isOptionalWhitespace(value.front())) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && isOptionalWhitespace(value.back())) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] bool isJsonContentType(std::string_view value) noexcept
{
    value = trimOptionalWhitespace(value);
    const auto separator = value.find(';');
    const auto mediaType = trimOptionalWhitespace(value.substr(0U, separator));
    if (!equalsAsciiCaseInsensitive(mediaType, "application/json")) {
        return false;
    }
    if (separator == std::string_view::npos) {
        return true;
    }

    auto parameter = trimOptionalWhitespace(value.substr(separator + 1U));
    if (parameter.empty() || parameter.find(';') != std::string_view::npos) {
        return false;
    }
    const auto equals = parameter.find('=');
    if (equals == std::string_view::npos ||
        parameter.find('=', equals + 1U) != std::string_view::npos) {
        return false;
    }

    const auto name = trimOptionalWhitespace(parameter.substr(0U, equals));
    const auto parameterValue =
        trimOptionalWhitespace(parameter.substr(equals + 1U));
    return equalsAsciiCaseInsensitive(name, "charset") &&
        equalsAsciiCaseInsensitive(parameterValue, "utf-8");
}

[[nodiscard]] constexpr bool isLowercaseHexadecimal(const char value) noexcept
{
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f');
}

[[nodiscard]] bool bearerMatches(
    const std::string_view authorization,
    const Domain::Sha256Digest& expected)
{
    constexpr std::string_view Scheme = "Bearer ";
    constexpr std::size_t TokenCharacters =
        Contracts::ManagerAuthenticationSecret::SizeBytes * 2U;
    if (!authorization.starts_with(Scheme)) {
        return false;
    }

    const auto candidate = authorization.substr(Scheme.size());
    std::array<char, TokenCharacters> normalized{};
    bool syntaxValid = candidate.size() == TokenCharacters;
    for (std::size_t index = 0U; index < normalized.size(); ++index) {
        const bool hasCharacter = index < candidate.size();
        const char character = hasCharacter ? candidate[index] : '0';
        const bool characterValid =
            hasCharacter && isLowercaseHexadecimal(character);
        syntaxValid = syntaxValid && characterValid;
        normalized[index] = characterValid ? character : '0';
    }

    auto parsed = Domain::Sha256Digest::parse(
        std::string_view{normalized.data(), normalized.size()});
    if (!parsed) {
        return false;
    }
    const bool equal = Contracts::constantTimeManagerAuthenticationTokenEquals(
        parsed.value(), expected);
    return syntaxValid && equal;
}

[[nodiscard]] bool isPublicBootstrapPath(const std::string_view path) noexcept
{
    return path == "/" || path == "/index.html" || path == "/control" ||
        path == "/manager" || path == "/ping" || path.starts_with("/static/");
}

[[nodiscard]] DashboardHttpRejection rejection(
    const std::uint16_t status,
    std::string code,
    std::string message,
    std::vector<DashboardHttpHeader> responseHeaders = {})
{
    return DashboardHttpRejection{
        status,
        std::move(code),
        std::move(message),
        std::move(responseHeaders)};
}

[[nodiscard]] DashboardHttpRejection forbidden(std::string message)
{
    return rejection(403U, "forbidden", std::move(message));
}

[[nodiscard]] DashboardHttpRejection unauthorized()
{
    return rejection(
        401U,
        "unauthorized",
        "Dashboard authentication is required.",
        {{"www-authenticate", "Bearer"}});
}

[[nodiscard]] DashboardHttpRejection internalFailure()
{
    return rejection(
        500U,
        "internal_failure",
        "Dashboard request policy evaluation failed.");
}

} // namespace

Domain::Result<DashboardRequestPolicy> DashboardRequestPolicy::create(
    std::string bindHost,
    const std::uint16_t bindPort,
    Domain::Sha256Digest bearerToken) noexcept
{
    try {
        if (!isConfiguredLoopbackHost(bindHost)) {
            return Domain::Result<DashboardRequestPolicy>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Dashboard bind host must be the literal 127.0.0.1 or ::1."));
        }
        if (bindPort == 0U) {
            return Domain::Result<DashboardRequestPolicy>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Dashboard bind port must be nonzero."));
        }
        return Domain::Result<DashboardRequestPolicy>::success(
            DashboardRequestPolicy{
                std::move(bindHost), bindPort, std::move(bearerToken)});
    } catch (...) {
        return Domain::Result<DashboardRequestPolicy>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard request policy construction failed."));
    }
}

std::optional<DashboardHttpRejection> DashboardRequestPolicy::rejectionFor(
    const DashboardHttpRequest& request) const noexcept
{
    try {
        const auto host = findHeader(request, "host");
        const std::string authority = bindHost_ == "::1"
            ? "[::1]:" + std::to_string(bindPort_)
            : "127.0.0.1:" + std::to_string(bindPort_);
        if (host.duplicate || !host.value.has_value() || *host.value != authority) {
            return forbidden(
                "The Host header does not match the active dashboard endpoint.");
        }

        if (!isPublicBootstrapPath(request.path())) {
            const auto authorization = findHeader(request, "authorization");
            if (authorization.duplicate || !authorization.value.has_value() ||
                !bearerMatches(*authorization.value, bearerToken_)) {
                return unauthorized();
            }
        }

        if (!request.isMutation()) {
            return std::nullopt;
        }

        const auto contentType = findHeader(request, "content-type");
        if (contentType.duplicate || !contentType.value.has_value() ||
            !isJsonContentType(*contentType.value)) {
            return rejection(
                415U,
                "unsupported_media_type",
                "State-changing dashboard requests require application/json.");
        }

        const std::string origin = "http://" + authority;
        const auto requestOrigin = findHeader(request, "origin");
        if (requestOrigin.duplicate ||
            (requestOrigin.value.has_value() && *requestOrigin.value != origin)) {
            return forbidden("Cross-origin dashboard requests are not allowed.");
        }

        const auto fetchSite = findHeader(request, "sec-fetch-site");
        if (fetchSite.duplicate ||
            (fetchSite.value.has_value() && *fetchSite.value != "same-origin" &&
             *fetchSite.value != "none")) {
            return forbidden("Cross-origin dashboard requests are not allowed.");
        }
        return std::nullopt;
    } catch (...) {
        return internalFailure();
    }
}

bool DashboardRequestPolicy::isConfiguredLoopbackHost(
    const std::string_view host) noexcept
{
    return host == "127.0.0.1" || host == "::1";
}

} // namespace ForgeConductor::Dashboard
