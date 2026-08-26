#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"

#include "Detail/UtfConversion.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::string_view Replacement = "<redacted>";

[[nodiscard]] char asciiLower(const char value) noexcept
{
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] bool matchesInsensitiveAt(
    const std::string_view value,
    const std::size_t position,
    const std::string_view needle) noexcept
{
    if (position > value.size() || needle.size() > value.size() - position) {
        return false;
    }
    for (std::size_t index = 0; index < needle.size(); ++index) {
        if (asciiLower(value[position + index]) != asciiLower(needle[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t findInsensitive(
    const std::string_view value,
    const std::string_view needle,
    const std::size_t start = 0) noexcept
{
    if (needle.empty() || start > value.size() || needle.size() > value.size()) {
        return std::string_view::npos;
    }
    const std::size_t last = value.size() - needle.size();
    for (std::size_t position = start; position <= last; ++position) {
        if (matchesInsensitiveAt(value, position, needle)) {
            return position;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] bool isAsciiWhitespace(const char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
        value == '\f' || value == '\v';
}

[[nodiscard]] bool isAsciiAlphaNumeric(const char value) noexcept
{
    return (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9');
}

[[nodiscard]] bool isAsciiWord(const char value) noexcept
{
    return isAsciiAlphaNumeric(value) || value == '_';
}

[[nodiscard]] bool isSecretTokenCharacter(const char value) noexcept
{
    return isAsciiAlphaNumeric(value) || value == '_' || value == '-' ||
        value == '.' || value == '~' || value == '+' || value == '/' ||
        value == '=';
}

[[nodiscard]] bool containsPrivateKeyMaterial(const std::string_view value) noexcept
{
    std::size_t position = 0;
    while ((position = findInsensitive(value, "-----begin ", position)) !=
           std::string_view::npos) {
        const std::size_t lineEnd = value.find_first_of("\r\n", position);
        const std::size_t count = lineEnd == std::string_view::npos
            ? value.size() - position
            : lineEnd - position;
        if (findInsensitive(value.substr(position, count), "private key-----") !=
            std::string_view::npos) {
            return true;
        }
        position += 11U;
    }
    return false;
}

void redactAuthorization(std::string& output)
{
    std::size_t search = 0;
    while (true) {
        const std::size_t position = findInsensitive(output, "authorization:", search);
        if (position == std::string::npos) {
            return;
        }
        std::size_t cursor = position + 14U;
        while (cursor < output.size() && isAsciiWhitespace(output[cursor])) {
            ++cursor;
        }
        const bool bearer = matchesInsensitiveAt(output, cursor, "bearer");
        const bool basic = matchesInsensitiveAt(output, cursor, "basic");
        if (!bearer && !basic) {
            search = position + 1U;
            continue;
        }
        cursor += bearer ? 6U : 5U;
        if (cursor >= output.size() || !isAsciiWhitespace(output[cursor])) {
            search = position + 1U;
            continue;
        }
        while (cursor < output.size() && isAsciiWhitespace(output[cursor])) {
            ++cursor;
        }
        const std::size_t tokenStart = cursor;
        while (cursor < output.size() && isSecretTokenCharacter(output[cursor])) {
            ++cursor;
        }
        if (cursor == tokenStart) {
            search = position + 1U;
            continue;
        }
        output.replace(position, cursor - position, Replacement);
        search = position + Replacement.size();
    }
}

void redactSkTokens(std::string& output)
{
    std::size_t search = 0;
    while ((search = output.find("sk-", search)) != std::string::npos) {
        if (search != 0 && isAsciiWord(output[search - 1U])) {
            ++search;
            continue;
        }
        std::size_t end = search + 3U;
        while (end < output.size() &&
               (isAsciiAlphaNumeric(output[end]) || output[end] == '_' ||
                output[end] == '-')) {
            ++end;
        }
        const std::size_t secretLength = end - (search + 3U);
        if (secretLength < 16U || !isAsciiWord(output[end - 1U])) {
            ++search;
            continue;
        }
        output.replace(search, end - search, Replacement);
        search += Replacement.size();
    }
}

void redactGithubTokens(std::string& output)
{
    std::size_t search = 0;
    while ((search = output.find("gh", search)) != std::string::npos) {
        if (search != 0 && isAsciiWord(output[search - 1U])) {
            ++search;
            continue;
        }
        if (search + 4U > output.size() ||
            std::string_view{"pousr"}.find(output[search + 2U]) ==
                std::string_view::npos ||
            output[search + 3U] != '_') {
            ++search;
            continue;
        }
        std::size_t end = search + 4U;
        while (end < output.size() && isAsciiAlphaNumeric(output[end])) {
            ++end;
        }
        if (end - (search + 4U) < 20U ||
            (end < output.size() && isAsciiWord(output[end]))) {
            ++search;
            continue;
        }
        output.replace(search, end - search, Replacement);
        search += Replacement.size();
    }
}

void redactAwsAccessKeys(std::string& output)
{
    std::size_t search = 0;
    while ((search = output.find("AKIA", search)) != std::string::npos) {
        if (search != 0 && isAsciiWord(output[search - 1U])) {
            ++search;
            continue;
        }
        constexpr std::size_t TokenLength = 20U;
        if (TokenLength > output.size() - search) {
            ++search;
            continue;
        }
        const std::size_t end = search + TokenLength;
        const bool validBody = std::all_of(
            output.begin() + static_cast<std::ptrdiff_t>(search + 4U),
            output.begin() + static_cast<std::ptrdiff_t>(end),
            [](const char value) {
                return (value >= 'A' && value <= 'Z') ||
                    (value >= '0' && value <= '9');
            });
        if (!validBody || (end < output.size() && isAsciiWord(output[end]))) {
            ++search;
            continue;
        }
        output.replace(search, TokenLength, Replacement);
        search += Replacement.size();
    }
}

void redactAssignments(std::string& output)
{
    static constexpr std::array<std::string_view, 8> Labels{
        "api_key",
        "api-key",
        "apikey",
        "access_token",
        "access-token",
        "accesstoken",
        "secret",
        "password"};

    std::size_t position = 0;
    while (position < output.size()) {
        if (position != 0 && isAsciiWord(output[position - 1U])) {
            ++position;
            continue;
        }
        bool replaced = false;
        for (const std::string_view label : Labels) {
            if (!matchesInsensitiveAt(output, position, label)) {
                continue;
            }
            std::size_t cursor = position + label.size();
            while (cursor < output.size() && isAsciiWhitespace(output[cursor])) {
                ++cursor;
            }
            if (cursor >= output.size() ||
                (output[cursor] != ':' && output[cursor] != '=')) {
                continue;
            }
            ++cursor;
            while (cursor < output.size() && isAsciiWhitespace(output[cursor])) {
                ++cursor;
            }
            const std::size_t valueStart = cursor;
            while (cursor < output.size() && !isAsciiWhitespace(output[cursor]) &&
                   output[cursor] != ',' && output[cursor] != ';') {
                ++cursor;
            }
            if (cursor == valueStart) {
                continue;
            }
            output.replace(position, cursor - position, Replacement);
            position += Replacement.size();
            replaced = true;
            break;
        }
        if (!replaced) {
            ++position;
        }
    }
}

} // namespace

Domain::Result<std::string> SecretRedactor::redact(
    const std::string_view value) noexcept
{
    try {
        if (value.size() > MaximumInputBytes) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "Redaction input exceeds 262144 UTF-8 bytes."));
        }
        if (value.find('\0') != std::string_view::npos) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Redaction input cannot contain NUL."));
        }
        auto utf8Validation = Detail::strictUtf8ToUtf16(value);
        if (!utf8Validation) {
            return Domain::Result<std::string>::failure(
                std::move(utf8Validation).error());
        }
        if (containsPrivateKeyMaterial(value)) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::RedactionRejected,
                "Private-key material is not accepted by the redaction boundary."));
        }

        std::string output{value};
        redactAuthorization(output);
        redactSkTokens(output);
        redactGithubTokens(output);
        redactAwsAccessKeys(output);
        redactAssignments(output);
        return Domain::Result<std::string>::success(std::move(output));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Redaction could not allocate its bounded output."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
