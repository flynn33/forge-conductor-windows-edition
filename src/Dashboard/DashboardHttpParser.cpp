#include "ForgeConductor/Dashboard/DashboardHttpParser.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Dashboard {
namespace {

constexpr std::string_view HeaderDelimiter{"\r\n\r\n"};
constexpr std::string_view LineDelimiter{"\r\n"};

[[nodiscard]] DashboardHttpParseResult reject(
    const std::uint16_t status,
    std::string code,
    std::string message)
{
    return DashboardHttpParseResult::rejected(DashboardHttpRejection{
        status,
        std::move(code),
        std::move(message),
        {}});
}

[[nodiscard]] DashboardHttpParseResult badRequest(std::string message)
{
    return reject(400U, "invalid_request", std::move(message));
}

[[nodiscard]] DashboardHttpParseResult headerLimit(std::string message)
{
    return reject(431U, "header_limit_exceeded", std::move(message));
}

[[nodiscard]] std::string_view byteView(
    const std::span<const std::byte> bytes) noexcept
{
    if (bytes.empty()) {
        return {};
    }
    return {
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size()};
}

[[nodiscard]] bool isContinuationByte(const unsigned char value) noexcept
{
    return (value & 0xc0U) == 0x80U;
}

[[nodiscard]] bool isStrictUtf8(const std::string_view value) noexcept
{
    std::size_t index{};
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        const auto continuation = [&](const std::size_t offset) noexcept {
            return index + offset < value.size() &&
                isContinuationByte(
                    static_cast<unsigned char>(value[index + offset]));
        };

        if (first >= 0xc2U && first <= 0xdfU) {
            if (!continuation(1U)) {
                return false;
            }
            index += 2U;
            continue;
        }
        if (first == 0xe0U) {
            if (index + 2U >= value.size()) {
                return false;
            }
            const auto second =
                static_cast<unsigned char>(value[index + 1U]);
            if (second < 0xa0U || second > 0xbfU || !continuation(2U)) {
                return false;
            }
            index += 3U;
            continue;
        }
        if ((first >= 0xe1U && first <= 0xecU) ||
            (first >= 0xeeU && first <= 0xefU)) {
            if (!continuation(1U) || !continuation(2U)) {
                return false;
            }
            index += 3U;
            continue;
        }
        if (first == 0xedU) {
            if (index + 2U >= value.size()) {
                return false;
            }
            const auto second =
                static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x80U || second > 0x9fU || !continuation(2U)) {
                return false;
            }
            index += 3U;
            continue;
        }
        if (first == 0xf0U) {
            if (index + 3U >= value.size()) {
                return false;
            }
            const auto second =
                static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x90U || second > 0xbfU ||
                !continuation(2U) || !continuation(3U)) {
                return false;
            }
            index += 4U;
            continue;
        }
        if (first >= 0xf1U && first <= 0xf3U) {
            if (!continuation(1U) || !continuation(2U) ||
                !continuation(3U)) {
                return false;
            }
            index += 4U;
            continue;
        }
        if (first == 0xf4U) {
            if (index + 3U >= value.size()) {
                return false;
            }
            const auto second =
                static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x80U || second > 0x8fU ||
                !continuation(2U) || !continuation(3U)) {
                return false;
            }
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool isTokenCharacter(const unsigned char value) noexcept
{
    if ((value >= static_cast<unsigned char>('0') &&
         value <= static_cast<unsigned char>('9')) ||
        (value >= static_cast<unsigned char>('A') &&
         value <= static_cast<unsigned char>('Z')) ||
        (value >= static_cast<unsigned char>('a') &&
         value <= static_cast<unsigned char>('z'))) {
        return true;
    }

    constexpr std::string_view Punctuation{"!#$%&'*+-.^_`|~"};
    return Punctuation.find(static_cast<char>(value)) != std::string_view::npos;
}

[[nodiscard]] char asciiLower(const char value) noexcept
{
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] char asciiUpper(const char value) noexcept
{
    if (value >= 'a' && value <= 'z') {
        return static_cast<char>(value - ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] bool isHexDigit(const char value) noexcept
{
    return (value >= '0' && value <= '9') ||
        (value >= 'A' && value <= 'F') ||
        (value >= 'a' && value <= 'f');
}

[[nodiscard]] bool isValidTarget(const std::string_view target) noexcept
{
    if (target.empty() || target.front() != '/') {
        return false;
    }

    for (std::size_t index{}; index < target.size(); ++index) {
        const auto value = static_cast<unsigned char>(target[index]);
        if (value < 0x21U || value > 0x7eU || value == '#' || value == '\\') {
            return false;
        }
        if (value == '%') {
            if (index + 2U >= target.size() ||
                !isHexDigit(target[index + 1U]) ||
                !isHexDigit(target[index + 2U])) {
                return false;
            }
            index += 2U;
        }
    }
    return true;
}

[[nodiscard]] bool hasDisallowedFieldValueByte(
    const std::string_view value) noexcept
{
    return std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte < 0x20U && byte != 0x09U) || byte == 0x7fU;
    });
}

[[nodiscard]] std::string_view trimOptionalWhitespace(
    std::string_view value) noexcept
{
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] std::size_t delimiterPrefixAtEnd(
    const std::string_view value) noexcept
{
    const auto maximum = (std::min)(value.size(), HeaderDelimiter.size() - 1U);
    for (std::size_t length = maximum; length > 0U; --length) {
        if (value.substr(value.size() - length) ==
            HeaderDelimiter.substr(0U, length)) {
            return length;
        }
    }
    return 0U;
}

[[nodiscard]] bool containsHeader(
    const std::vector<DashboardHttpHeader>& headers,
    const std::string_view lowercaseName) noexcept
{
    return std::any_of(
        headers.begin(), headers.end(),
        [lowercaseName](const DashboardHttpHeader& header) {
            return header.name == lowercaseName;
        });
}

struct ParsedRequestLine final {
    std::string method;
    std::string target;
};

[[nodiscard]] bool parseRequestLine(
    const std::string_view line,
    ParsedRequestLine& parsed)
{
    if (line.find('\r') != std::string_view::npos ||
        line.find('\n') != std::string_view::npos) {
        return false;
    }

    const auto firstSpace = line.find(' ');
    if (firstSpace == std::string_view::npos || firstSpace == 0U) {
        return false;
    }
    const auto secondSpace = line.find(' ', firstSpace + 1U);
    if (secondSpace == std::string_view::npos ||
        secondSpace == firstSpace + 1U ||
        line.substr(secondSpace + 1U) != "HTTP/1.1") {
        return false;
    }

    const auto method = line.substr(0U, firstSpace);
    if (!std::all_of(method.begin(), method.end(), [](const char value) {
            return isTokenCharacter(static_cast<unsigned char>(value));
        })) {
        return false;
    }

    const auto target =
        line.substr(firstSpace + 1U, secondSpace - firstSpace - 1U);
    if (target.size() > DashboardHttpParser::MaximumTargetBytes) {
        return false;
    }
    if (!isValidTarget(target)) {
        return false;
    }

    parsed.method.assign(method);
    std::transform(
        parsed.method.begin(), parsed.method.end(), parsed.method.begin(),
        asciiUpper);
    parsed.target.assign(target);
    return true;
}

enum class RequestLineResult { Accepted, Invalid, TargetTooLong };

[[nodiscard]] RequestLineResult parseBoundedRequestLine(
    const std::string_view line,
    ParsedRequestLine& parsed)
{
    const auto firstSpace = line.find(' ');
    if (firstSpace != std::string_view::npos) {
        const auto secondSpace = line.find(' ', firstSpace + 1U);
        if (secondSpace != std::string_view::npos &&
            secondSpace > firstSpace + 1U &&
            secondSpace - firstSpace - 1U >
                DashboardHttpParser::MaximumTargetBytes) {
            return RequestLineResult::TargetTooLong;
        }
    }
    return parseRequestLine(line, parsed)
        ? RequestLineResult::Accepted
        : RequestLineResult::Invalid;
}

[[nodiscard]] DashboardHttpParseResult parseCompleteHeader(
    const std::span<const std::byte> buffer,
    const std::string_view headerText,
    const std::size_t bodyOffset,
    const bool streamComplete)
{
    if (!isStrictUtf8(headerText)) {
        return badRequest("The HTTP header section is not strict UTF-8.");
    }

    const auto requestLineEnd = headerText.find(LineDelimiter);
    const auto requestLine = requestLineEnd == std::string_view::npos
        ? headerText
        : headerText.substr(0U, requestLineEnd);

    ParsedRequestLine parsedLine;
    switch (parseBoundedRequestLine(requestLine, parsedLine)) {
    case RequestLineResult::TargetTooLong:
        return reject(
            414U,
            "target_too_large",
            "The HTTP request target exceeds the configured byte limit.");
    case RequestLineResult::Invalid:
        return badRequest("The HTTP request line is invalid.");
    case RequestLineResult::Accepted:
        break;
    }

    std::vector<DashboardHttpHeader> headers;
    headers.reserve((std::min)(
        DashboardHttpParser::MaximumHeaderCount,
        static_cast<std::size_t>(16U)));
    std::size_t contentLength{};

    auto cursor = requestLineEnd == std::string_view::npos
        ? headerText.size()
        : requestLineEnd + LineDelimiter.size();
    while (cursor < headerText.size()) {
        const auto lineEnd = headerText.find(LineDelimiter, cursor);
        const auto end = lineEnd == std::string_view::npos
            ? headerText.size()
            : lineEnd;
        const auto line = headerText.substr(cursor, end - cursor);

        if (headers.size() >= DashboardHttpParser::MaximumHeaderCount) {
            return headerLimit(
                "The HTTP request contains too many header fields.");
        }
        if (line.empty() || line.front() == ' ' || line.front() == '\t' ||
            line.find('\r') != std::string_view::npos ||
            line.find('\n') != std::string_view::npos) {
            return badRequest("The HTTP header field syntax is invalid.");
        }

        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            return badRequest("The HTTP header field syntax is invalid.");
        }
        const auto rawName = line.substr(0U, colon);
        if (!std::all_of(
                rawName.begin(), rawName.end(), [](const char value) {
                    return isTokenCharacter(
                        static_cast<unsigned char>(value));
                })) {
            return badRequest("The HTTP header name is invalid.");
        }

        std::string name{rawName};
        std::transform(
            name.begin(), name.end(), name.begin(), asciiLower);
        if (containsHeader(headers, name)) {
            return badRequest("Duplicate HTTP header fields are not accepted.");
        }
        if (name == "transfer-encoding") {
            return badRequest("Transfer-Encoding is not accepted.");
        }

        const auto rawValue = trimOptionalWhitespace(line.substr(colon + 1U));
        if (hasDisallowedFieldValueByte(rawValue)) {
            return badRequest("The HTTP header value contains a control byte.");
        }

        if (name == "content-length") {
            if (rawValue.empty()) {
                return badRequest("Content-Length must be an unsigned decimal value.");
            }
            std::size_t parsedLength{};
            for (const char character : rawValue) {
                if (character < '0' || character > '9') {
                    return badRequest(
                        "Content-Length must be an unsigned decimal value.");
                }
                const auto digit =
                    static_cast<std::size_t>(character - '0');
                if (parsedLength >
                    (DashboardHttpParser::MaximumBodyBytes - digit) / 10U) {
                    return reject(
                        413U,
                        "payload_too_large",
                        "The HTTP request body exceeds the configured byte limit.");
                }
                parsedLength = parsedLength * 10U + digit;
            }
            contentLength = parsedLength;
        }

        headers.push_back(DashboardHttpHeader{
            std::move(name), std::string{rawValue}});
        cursor = lineEnd == std::string_view::npos
            ? headerText.size()
            : lineEnd + LineDelimiter.size();
    }

    const auto availableBodyBytes = buffer.size() - bodyOffset;
    if (availableBodyBytes < contentLength) {
        return streamComplete
            ? badRequest("The HTTP request body is incomplete.")
            : DashboardHttpParseResult::needMoreData();
    }
    if (availableBodyBytes > contentLength) {
        return badRequest(
            "Bytes after the declared HTTP request body are not accepted.");
    }

    const auto bodySpan = buffer.subspan(bodyOffset, contentLength);
    std::vector<std::byte> body{bodySpan.begin(), bodySpan.end()};
    return DashboardHttpParseResult::accepted(
        DashboardHttpRequest{
            std::move(parsedLine.method),
            std::move(parsedLine.target),
            std::move(headers),
            std::move(body)},
        bodyOffset + contentLength);
}

} // namespace

DashboardHttpParseResult DashboardHttpParser::parse(
    const std::span<const std::byte> buffer,
    const bool streamComplete) const noexcept
{
    try {
        const auto wire = byteView(buffer);
        const auto searchLength = (std::min)(
            wire.size(), MaximumHeaderBytes + HeaderDelimiter.size());
        const auto headerEnd =
            wire.substr(0U, searchLength).find(HeaderDelimiter);
        if (headerEnd == std::string_view::npos) {
            if (wire.size() >
                MaximumHeaderBytes + HeaderDelimiter.size() - 1U) {
                return headerLimit(
                    "The HTTP header section exceeds the configured byte limit.");
            }
            const auto possibleDelimiterBytes = delimiterPrefixAtEnd(wire);
            if (wire.size() - possibleDelimiterBytes > MaximumHeaderBytes) {
                return headerLimit(
                    "The HTTP header section exceeds the configured byte limit.");
            }
            return streamComplete
                ? badRequest("The HTTP header section is incomplete.")
                : DashboardHttpParseResult::needMoreData();
        }
        if (headerEnd > MaximumHeaderBytes) {
            return headerLimit(
                "The HTTP header section exceeds the configured byte limit.");
        }

        return parseCompleteHeader(
            buffer,
            wire.substr(0U, headerEnd),
            headerEnd + HeaderDelimiter.size(),
            streamComplete);
    } catch (...) {
        return reject(
            500U,
            "internal_failure",
            "The HTTP request could not be parsed safely.");
    }
}

} // namespace ForgeConductor::Dashboard
