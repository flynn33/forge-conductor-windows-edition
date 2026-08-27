#include "ForgeConductor/Dashboard/DashboardStaticResourcePath.h"

#include "ForgeConductor/Domain/Error.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Dashboard {
namespace {

constexpr std::string_view StaticPrefix = "/static/";

[[nodiscard]] Domain::Result<DashboardStaticResourcePath> invalid(
    std::string message)
{
    return Domain::Result<DashboardStaticResourcePath>::failure(
        Domain::makeError(Domain::ErrorCodes::InvalidRequest, std::move(message)));
}

[[nodiscard]] Domain::Result<DashboardStaticResourcePath> outsideAuthority(
    std::string message)
{
    return Domain::Result<DashboardStaticResourcePath>::failure(
        Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority, std::move(message)));
}

[[nodiscard]] Domain::Result<DashboardStaticResourcePath> tooLarge(
    std::string message)
{
    return Domain::Result<DashboardStaticResourcePath>::failure(
        Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge, std::move(message)));
}

[[nodiscard]] Domain::Result<DashboardStaticResourcePath> internalFailure()
{
    return Domain::Result<DashboardStaticResourcePath>::failure(
        Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard static-resource path decoding failed."));
}

[[nodiscard]] constexpr int hexadecimalValue(const char value) noexcept
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] constexpr bool isContinuationByte(
    const unsigned char value) noexcept
{
    return value >= 0x80U && value <= 0xbfU;
}

// Validates shortest-form UTF-8 scalar encodings. C0/C1 controls are rejected
// in addition to malformed sequences so no control can be hidden in UTF-8.
[[nodiscard]] bool isStrictControlFreeUtf8(const std::string_view value) noexcept
{
    std::size_t offset = 0U;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7fU) {
            if (first <= 0x1fU || first == 0x7fU) {
                return false;
            }
            ++offset;
            continue;
        }

        if (first >= 0xc2U && first <= 0xdfU) {
            if (offset + 1U >= value.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[offset + 1U]);
            if (!isContinuationByte(second)) {
                return false;
            }
            const std::uint32_t codePoint =
                (static_cast<std::uint32_t>(first & 0x1fU) << 6U) |
                static_cast<std::uint32_t>(second & 0x3fU);
            if (codePoint >= 0x80U && codePoint <= 0x9fU) {
                return false;
            }
            offset += 2U;
            continue;
        }

        if (first >= 0xe0U && first <= 0xefU) {
            if (offset + 2U >= value.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[offset + 1U]);
            const auto third = static_cast<unsigned char>(value[offset + 2U]);
            if (!isContinuationByte(second) || !isContinuationByte(third) ||
                (first == 0xe0U && second < 0xa0U) ||
                (first == 0xedU && second > 0x9fU)) {
                return false;
            }
            offset += 3U;
            continue;
        }

        if (first >= 0xf0U && first <= 0xf4U) {
            if (offset + 3U >= value.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[offset + 1U]);
            const auto third = static_cast<unsigned char>(value[offset + 2U]);
            const auto fourth = static_cast<unsigned char>(value[offset + 3U]);
            if (!isContinuationByte(second) || !isContinuationByte(third) ||
                !isContinuationByte(fourth) ||
                (first == 0xf0U && second < 0x90U) ||
                (first == 0xf4U && second > 0x8fU)) {
                return false;
            }
            offset += 4U;
            continue;
        }

        return false;
    }
    return true;
}

[[nodiscard]] constexpr bool isCanonicalFirstCharacter(
    const char value) noexcept
{
    return (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9');
}

[[nodiscard]] constexpr bool isCanonicalPathCharacter(
    const char value) noexcept
{
    return isCanonicalFirstCharacter(value) || value == '-' || value == '_' ||
        value == '.';
}

[[nodiscard]] bool isReservedWindowsDeviceStem(
    const std::string_view segment) noexcept
{
    const auto dot = segment.find('.');
    const auto stem = segment.substr(0U, dot);
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
        return true;
    }
    if (stem.size() != 4U) {
        return false;
    }
    const auto prefix = stem.substr(0U, 3U);
    const char number = stem[3U];
    return (prefix == "com" || prefix == "lpt") &&
        number >= '0' && number <= '9';
}

[[nodiscard]] Domain::Result<void> validateCanonicalRelativePath(
    const std::string_view value)
{
    std::size_t segmentStart = 0U;
    while (segmentStart <= value.size()) {
        const auto separator = value.find('/', segmentStart);
        const auto segmentEnd = separator == std::string_view::npos
            ? value.size()
            : separator;
        const auto segment = value.substr(segmentStart, segmentEnd - segmentStart);

        if (segment.empty() || segment == "." || segment == "..") {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "Dashboard static-resource paths may not contain empty, dot, or dot-dot segments."));
        }
        if (segment.size() > DashboardStaticResourcePath::MaximumSegmentBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A dashboard static-resource path segment exceeds the 255-byte limit."));
        }
        const bool allCharactersCanonical = std::all_of(
            segment.begin(), segment.end(), isCanonicalPathCharacter);
        if (!isCanonicalFirstCharacter(segment.front()) ||
            segment.back() == '.' ||
            !allCharactersCanonical) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "Dashboard static-resource paths must use canonical lowercase ASCII segments."));
        }
        if (isReservedWindowsDeviceStem(segment)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "Dashboard static-resource paths may not use Windows device names."));
        }

        if (separator == std::string_view::npos) {
            break;
        }
        segmentStart = separator + 1U;
    }
    return Domain::Result<void>::success();
}

} // namespace

Domain::Result<DashboardStaticResourcePath> DashboardStaticResourcePath::decode(
    const std::string_view classifiedRawTarget) noexcept
{
    try {
        if (classifiedRawTarget.size() > MaximumRawTargetBytes) {
            return tooLarge(
                "The dashboard static-resource target exceeds the 8192-byte limit.");
        }
        if (!classifiedRawTarget.starts_with(StaticPrefix)) {
            return invalid(
                "The dashboard static-resource target must begin with /static/.");
        }

        const auto rawSuffix = classifiedRawTarget.substr(StaticPrefix.size());
        if (rawSuffix.empty()) {
            return invalid("The dashboard static-resource suffix is empty.");
        }
        if (rawSuffix.find('?') != std::string_view::npos ||
            rawSuffix.find('#') != std::string_view::npos) {
            return invalid(
                "Dashboard static-resource paths may not contain a query or fragment delimiter.");
        }

        std::string decoded;
        decoded.reserve((std::min)(rawSuffix.size(), MaximumDecodedBytes));
        for (std::size_t offset = 0U; offset < rawSuffix.size(); ++offset) {
            unsigned char byte = static_cast<unsigned char>(rawSuffix[offset]);
            bool percentEncoded = false;
            if (byte == static_cast<unsigned char>('%')) {
                if (offset + 2U >= rawSuffix.size()) {
                    return invalid(
                        "Dashboard static-resource paths contain a malformed percent escape.");
                }
                const int high = hexadecimalValue(rawSuffix[offset + 1U]);
                const int low = hexadecimalValue(rawSuffix[offset + 2U]);
                if (high < 0 || low < 0) {
                    return invalid(
                        "Dashboard static-resource paths contain a malformed percent escape.");
                }
                byte = static_cast<unsigned char>((high << 4) | low);
                offset += 2U;
                percentEncoded = true;
            }

            if (byte == static_cast<unsigned char>('\\') ||
                (byte == static_cast<unsigned char>('/') && percentEncoded)) {
                return outsideAuthority(
                    "Dashboard static-resource paths may not contain encoded separators or backslashes.");
            }
            if (byte == static_cast<unsigned char>('?') ||
                byte == static_cast<unsigned char>('#')) {
                return invalid(
                    "Dashboard static-resource paths may not contain encoded query or fragment delimiters.");
            }
            if (decoded.size() == MaximumDecodedBytes) {
                return tooLarge(
                    "The decoded dashboard static-resource path exceeds the 1024-byte limit.");
            }
            decoded.push_back(static_cast<char>(byte));
        }

        if (!isStrictControlFreeUtf8(decoded)) {
            return invalid(
                "The decoded dashboard static-resource path must be strict control-free UTF-8.");
        }

        auto canonical = validateCanonicalRelativePath(decoded);
        if (!canonical) {
            return Domain::Result<DashboardStaticResourcePath>::failure(
                std::move(canonical).error());
        }

        const auto extensionDelimiter = decoded.rfind('.');
        if (extensionDelimiter == std::string::npos) {
            return invalid(
                "Dashboard static resources require an allowed file extension.");
        }
        const auto extension = std::string_view{decoded}.substr(
            extensionDelimiter + 1U);
        MimeKind mimeKind{};
        if (extension == "html") {
            mimeKind = MimeKind::Html;
        } else if (extension == "css") {
            mimeKind = MimeKind::Css;
        } else if (extension == "js") {
            mimeKind = MimeKind::JavaScript;
        } else if (extension == "json") {
            mimeKind = MimeKind::Json;
        } else {
            return invalid(
                "The dashboard static-resource file extension is not allowed.");
        }

        return Domain::Result<DashboardStaticResourcePath>::success(
            DashboardStaticResourcePath{std::move(decoded), mimeKind});
    } catch (...) {
        return internalFailure();
    }
}

std::string_view DashboardStaticResourcePath::mimeType() const noexcept
{
    switch (mimeKind_) {
    case MimeKind::Html:
        return "text/html; charset=utf-8";
    case MimeKind::Css:
        return "text/css; charset=utf-8";
    case MimeKind::JavaScript:
        return "application/javascript; charset=utf-8";
    case MimeKind::Json:
        return "application/json; charset=utf-8";
    }
    return "application/octet-stream";
}

} // namespace ForgeConductor::Dashboard
