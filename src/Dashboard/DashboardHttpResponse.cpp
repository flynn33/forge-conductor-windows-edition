#include "ForgeConductor/Dashboard/DashboardHttpResponse.h"

#include <array>
#include <charconv>
#include <limits>
#include <span>
#include <string_view>

namespace ForgeConductor::Dashboard {
namespace {

constexpr std::string_view HttpPrefix = "HTTP/1.1 ";
constexpr std::string_view ContentTypePrefix = "Content-Type: ";
constexpr std::string_view ContentLengthPrefix = "Content-Length: ";
constexpr std::string_view CompleteConnection = "Connection: close\r\n";
constexpr std::string_view SseConnection = "Connection: keep-alive\r\n";
constexpr std::string_view NoStore = "Cache-Control: no-store\r\n";
constexpr std::string_view SseCache =
    "Cache-Control: no-cache, no-transform\r\n";
constexpr std::string_view SecurityHeaders =
    "X-Content-Type-Options: nosniff\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "Cross-Origin-Resource-Policy: same-origin\r\n"
    "Content-Security-Policy: default-src 'self'; connect-src 'self'; "
    "img-src 'self' data:; style-src 'self' 'unsafe-inline'; "
    "script-src 'self' 'unsafe-inline'\r\n";
constexpr std::string_view SseContentType =
    "text/event-stream; charset=utf-8";
constexpr std::string_view SseBuffering = "X-Accel-Buffering: no\r\n";
constexpr std::string_view HeaderSeparator = ": ";
constexpr std::string_view LineEnd = "\r\n";

struct StatusDefinition final {
    std::uint16_t status;
    std::string_view reason;
};

constexpr std::array StatusDefinitions{
    StatusDefinition{200U, "OK"},
    StatusDefinition{201U, "Created"},
    StatusDefinition{202U, "Accepted"},
    StatusDefinition{204U, "No Content"},
    StatusDefinition{400U, "Bad Request"},
    StatusDefinition{401U, "Unauthorized"},
    StatusDefinition{403U, "Forbidden"},
    StatusDefinition{404U, "Not Found"},
    StatusDefinition{405U, "Method Not Allowed"},
    StatusDefinition{409U, "Conflict"},
    StatusDefinition{413U, "Content Too Large"},
    StatusDefinition{414U, "URI Too Long"},
    StatusDefinition{415U, "Unsupported Media Type"},
    StatusDefinition{422U, "Unprocessable Content"},
    StatusDefinition{429U, "Too Many Requests"},
    StatusDefinition{431U, "Request Header Fields Too Large"},
    StatusDefinition{500U, "Internal Server Error"},
    StatusDefinition{501U, "Not Implemented"},
    StatusDefinition{503U, "Service Unavailable"},
};

struct ExtraHeaderDefinition final {
    std::string_view lowercaseName;
    std::string_view canonicalName;
    std::size_t index;
};

constexpr std::array ExtraHeaderDefinitions{
    ExtraHeaderDefinition{"allow", "Allow", 0U},
    ExtraHeaderDefinition{"content-disposition", "Content-Disposition", 1U},
    ExtraHeaderDefinition{"content-language", "Content-Language", 2U},
    ExtraHeaderDefinition{"etag", "ETag", 3U},
    ExtraHeaderDefinition{"last-modified", "Last-Modified", 4U},
    ExtraHeaderDefinition{"location", "Location", 5U},
    ExtraHeaderDefinition{"retry-after", "Retry-After", 6U},
    ExtraHeaderDefinition{"www-authenticate", "WWW-Authenticate", 7U},
};

struct ValidatedExtraHeader final {
    std::string_view canonicalName;
    std::string_view value;
};

struct ValidatedExtraHeaders final {
    std::array<ValidatedExtraHeader,
               DashboardHttpResponseEncoder::MaximumExtraHeaderCount>
        entries{};
    std::size_t count{};
};

[[nodiscard]] constexpr bool isTokenCharacter(const unsigned char value) noexcept
{
    if ((value >= static_cast<unsigned char>('0') &&
         value <= static_cast<unsigned char>('9')) ||
        (value >= static_cast<unsigned char>('A') &&
         value <= static_cast<unsigned char>('Z')) ||
        (value >= static_cast<unsigned char>('a') &&
         value <= static_cast<unsigned char>('z'))) {
        return true;
    }

    switch (value) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isValidHeaderName(const std::string_view name) noexcept
{
    if (name.empty() ||
        name.size() > DashboardHttpResponseEncoder::MaximumExtraHeaderNameBytes) {
        return false;
    }
    for (const unsigned char value : name) {
        if (!isTokenCharacter(value)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidVisibleAsciiValue(
    const std::string_view value,
    const std::size_t maximumBytes) noexcept
{
    if (value.empty() || value.size() > maximumBytes || value.front() == ' ' ||
        value.back() == ' ') {
        return false;
    }
    for (const unsigned char character : value) {
        if (character < 0x20U || character > 0x7eU) {
            return false;
        }
    }
    return true;
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
    for (std::size_t index{}; index < left.size(); ++index) {
        if (lowerAscii(left[index]) != lowerAscii(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const ExtraHeaderDefinition* findExtraHeaderDefinition(
    const std::string_view name) noexcept
{
    for (const auto& definition : ExtraHeaderDefinitions) {
        if (equalsAsciiCaseInsensitive(name, definition.lowercaseName)) {
            return &definition;
        }
    }
    return nullptr;
}

[[nodiscard]] DashboardHttpEncodingError validateExtraHeaders(
    const std::span<const DashboardHttpHeader> headers,
    ValidatedExtraHeaders& validated) noexcept
{
    if (headers.size() >
        DashboardHttpResponseEncoder::MaximumExtraHeaderCount) {
        return DashboardHttpEncodingError::TooManyExtraHeaders;
    }

    std::array<bool, ExtraHeaderDefinitions.size()> seen{};
    for (const auto& header : headers) {
        if (!isValidHeaderName(header.name)) {
            return DashboardHttpEncodingError::InvalidHeaderName;
        }
        if (!isValidVisibleAsciiValue(
                header.value,
                DashboardHttpResponseEncoder::MaximumExtraHeaderValueBytes)) {
            return DashboardHttpEncodingError::InvalidHeaderValue;
        }

        const auto* definition = findExtraHeaderDefinition(header.name);
        if (definition == nullptr) {
            return DashboardHttpEncodingError::UnsupportedExtraHeader;
        }
        if (seen[definition->index]) {
            return DashboardHttpEncodingError::DuplicateHeader;
        }
        seen[definition->index] = true;
        validated.entries[validated.count++] =
            ValidatedExtraHeader{definition->canonicalName, header.value};
    }
    return DashboardHttpEncodingError::None;
}

[[nodiscard]] std::string_view reasonForStatus(
    const std::uint16_t status) noexcept
{
    for (const auto& definition : StatusDefinitions) {
        if (definition.status == status) {
            return definition.reason;
        }
    }
    return {};
}

struct DecimalText final {
    std::array<char, std::numeric_limits<std::size_t>::digits10 + 2U> storage{};
    std::size_t size{};

    [[nodiscard]] std::string_view view() const noexcept
    {
        return {storage.data(), size};
    }
};

[[nodiscard]] bool decimalText(
    const std::size_t value,
    DecimalText& output) noexcept
{
    const auto converted = std::to_chars(
        output.storage.data(),
        output.storage.data() + output.storage.size(),
        value);
    if (converted.ec != std::errc{}) {
        return false;
    }
    output.size = static_cast<std::size_t>(converted.ptr - output.storage.data());
    return true;
}

[[nodiscard]] bool addMeasured(
    std::size_t& total,
    const std::size_t count,
    const std::size_t maximum) noexcept
{
    if (count > maximum - total) {
        return false;
    }
    total += count;
    return true;
}

[[nodiscard]] bool measure(
    std::size_t& total,
    const std::string_view value,
    const std::size_t maximum) noexcept
{
    return addMeasured(total, value.size(), maximum);
}

[[nodiscard]] bool measureExtraHeaders(
    std::size_t& total,
    const ValidatedExtraHeaders& headers,
    const std::size_t maximum) noexcept
{
    for (std::size_t index{}; index < headers.count; ++index) {
        const auto& header = headers.entries[index];
        if (!measure(total, header.canonicalName, maximum) ||
            !measure(total, HeaderSeparator, maximum) ||
            !measure(total, header.value, maximum) ||
            !measure(total, LineEnd, maximum)) {
            return false;
        }
    }
    return true;
}

void append(
    std::vector<std::byte>& output,
    const std::string_view value)
{
    for (const unsigned char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
}

void appendExtraHeaders(
    std::vector<std::byte>& output,
    const ValidatedExtraHeaders& headers)
{
    for (std::size_t index{}; index < headers.count; ++index) {
        const auto& header = headers.entries[index];
        append(output, header.canonicalName);
        append(output, HeaderSeparator);
        append(output, header.value);
        append(output, LineEnd);
    }
}

} // namespace

DashboardHttpEncodingResult DashboardHttpResponseEncoder::encode(
    const DashboardHttpResponse& response) noexcept
{
    return encodeFixedResponse(
        response.status(),
        response.contentType(),
        response.body(),
        response.body().size(),
        response.extraHeaders(),
        DashboardHttpEncodingResult::Kind::CompleteResponse);
}

DashboardHttpEncodingResult DashboardHttpResponseEncoder::encodeHead(
    const DashboardHttpHeadResponse& response) noexcept
{
    return encodeFixedResponse(
        response.status(),
        response.contentType(),
        {},
        response.representationLength(),
        response.extraHeaders(),
        DashboardHttpEncodingResult::Kind::HeadResponseHead);
}

DashboardHttpEncodingResult DashboardHttpResponseEncoder::encodeFixedResponse(
    const std::uint16_t status,
    const std::string_view contentType,
    const std::span<const std::byte> body,
    const std::size_t representationLength,
    const std::span<const DashboardHttpHeader> responseHeaders,
    const DashboardHttpEncodingResult::Kind kind) noexcept
{
    const auto fail = [](const DashboardHttpEncodingError error) noexcept {
        return DashboardHttpEncodingResult::failure(error);
    };
    try {
        const bool includeBody =
            kind == DashboardHttpEncodingResult::Kind::CompleteResponse;
        if (!includeBody &&
            kind != DashboardHttpEncodingResult::Kind::HeadResponseHead) {
            return fail(DashboardHttpEncodingError::InternalFailure);
        }

        const auto reason = reasonForStatus(status);
        if (reason.empty()) {
            return fail(DashboardHttpEncodingError::UnsupportedStatus);
        }
        if ((status == 204U && representationLength != 0U) ||
            (includeBody && body.size() != representationLength)) {
            return fail(DashboardHttpEncodingError::BodyNotAllowed);
        }
        if (!isValidVisibleAsciiValue(
                contentType, MaximumContentTypeBytes)) {
            return fail(DashboardHttpEncodingError::InvalidContentType);
        }
        if (representationLength > MaximumEncodedResponseBytes) {
            return fail(DashboardHttpEncodingError::ResponseTooLarge);
        }

        ValidatedExtraHeaders extraHeaders;
        const auto headerValidation = validateExtraHeaders(
            responseHeaders, extraHeaders);
        if (headerValidation != DashboardHttpEncodingError::None) {
            return fail(headerValidation);
        }

        DecimalText statusText;
        DecimalText lengthText;
        if (!decimalText(status, statusText) ||
            !decimalText(representationLength, lengthText)) {
            return fail(DashboardHttpEncodingError::InternalFailure);
        }

        const bool includeContentLength = status != 204U;
        std::size_t headerBytes{};
        const auto measureHeader = [&headerBytes](
                                       const std::string_view value) noexcept {
            return measure(
                headerBytes,
                value,
                DashboardHttpResponseEncoder::MaximumEncodedHeaderBytes);
        };
        if (!measureHeader(HttpPrefix) || !measureHeader(statusText.view()) ||
            !measureHeader(" ") || !measureHeader(reason) ||
            !measureHeader(LineEnd) || !measureHeader(ContentTypePrefix) ||
            !measureHeader(contentType) || !measureHeader(LineEnd) ||
            (includeContentLength &&
             (!measureHeader(ContentLengthPrefix) ||
              !measureHeader(lengthText.view()) || !measureHeader(LineEnd))) ||
            !measureHeader(CompleteConnection) || !measureHeader(NoStore) ||
            !measureHeader(SecurityHeaders) ||
            !measureExtraHeaders(
                headerBytes, extraHeaders, MaximumEncodedHeaderBytes) ||
            !measureHeader(LineEnd)) {
            return fail(DashboardHttpEncodingError::HeaderTooLarge);
        }

        std::size_t encodedBytes = headerBytes;
        if (includeBody && !addMeasured(
                encodedBytes,
                body.size(),
                MaximumEncodedResponseBytes)) {
            return fail(DashboardHttpEncodingError::ResponseTooLarge);
        }

        std::vector<std::byte> output;
        output.reserve(encodedBytes);
        append(output, HttpPrefix);
        append(output, statusText.view());
        append(output, " ");
        append(output, reason);
        append(output, LineEnd);
        append(output, ContentTypePrefix);
        append(output, contentType);
        append(output, LineEnd);
        if (includeContentLength) {
            append(output, ContentLengthPrefix);
            append(output, lengthText.view());
            append(output, LineEnd);
        }
        append(output, CompleteConnection);
        append(output, NoStore);
        append(output, SecurityHeaders);
        appendExtraHeaders(output, extraHeaders);
        append(output, LineEnd);
        if (includeBody) {
            output.insert(output.end(), body.begin(), body.end());
        }

        if (output.size() != encodedBytes) {
            return fail(DashboardHttpEncodingError::InternalFailure);
        }
        return DashboardHttpEncodingResult::success(
            kind,
            std::move(output));
    } catch (...) {
        return fail(DashboardHttpEncodingError::InternalFailure);
    }
}

DashboardHttpEncodingResult DashboardHttpResponseEncoder::encodeSseBootstrap(
    const DashboardSseBootstrap& bootstrap) noexcept
{
    const auto fail = [](const DashboardHttpEncodingError error) noexcept {
        return DashboardHttpEncodingResult::failure(error);
    };
    try {
        ValidatedExtraHeaders extraHeaders;
        const auto headerValidation = validateExtraHeaders(
            bootstrap.extraHeaders(), extraHeaders);
        if (headerValidation != DashboardHttpEncodingError::None) {
            return fail(headerValidation);
        }

        std::size_t headerBytes{};
        const auto measureHeader = [&headerBytes](
                                       const std::string_view value) noexcept {
            return measure(
                headerBytes,
                value,
                DashboardHttpResponseEncoder::MaximumEncodedHeaderBytes);
        };
        if (!measureHeader(HttpPrefix) || !measureHeader("200 OK") ||
            !measureHeader(LineEnd) || !measureHeader(ContentTypePrefix) ||
            !measureHeader(SseContentType) || !measureHeader(LineEnd) ||
            !measureHeader(SseCache) || !measureHeader(SseConnection) ||
            !measureHeader(SecurityHeaders) || !measureHeader(SseBuffering) ||
            !measureExtraHeaders(
                headerBytes, extraHeaders, MaximumEncodedHeaderBytes) ||
            !measureHeader(LineEnd)) {
            return fail(DashboardHttpEncodingError::HeaderTooLarge);
        }

        std::vector<std::byte> output;
        output.reserve(headerBytes);
        append(output, HttpPrefix);
        append(output, "200 OK");
        append(output, LineEnd);
        append(output, ContentTypePrefix);
        append(output, SseContentType);
        append(output, LineEnd);
        append(output, SseCache);
        append(output, SseConnection);
        append(output, SecurityHeaders);
        append(output, SseBuffering);
        appendExtraHeaders(output, extraHeaders);
        append(output, LineEnd);

        if (output.size() != headerBytes) {
            return fail(DashboardHttpEncodingError::InternalFailure);
        }
        return DashboardHttpEncodingResult::success(
            DashboardHttpEncodingResult::Kind::SseBootstrapHead,
            std::move(output));
    } catch (...) {
        return fail(DashboardHttpEncodingError::InternalFailure);
    }
}

} // namespace ForgeConductor::Dashboard
