#include "ForgeConductor/Domain/Identifiers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace ForgeConductor::Domain {
namespace {

[[nodiscard]] bool isAsciiHex(const char value) noexcept
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

[[nodiscard]] bool isSafeIdentifierCharacter(const char value) noexcept
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '_' ||
           value == '.';
}

} // namespace

Result<Uuid> Uuid::parse(const std::string_view value)
{
    constexpr std::array<std::size_t, 4> HyphenOffsets{8, 13, 18, 23};
    if (value.size() != 36) {
        return Result<Uuid>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "UUID must contain exactly 36 ASCII characters."));
    }

    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool isHyphenOffset =
            std::find(HyphenOffsets.begin(), HyphenOffsets.end(), index) !=
            HyphenOffsets.end();
        if ((isHyphenOffset && value[index] != '-') ||
            (!isHyphenOffset && !isAsciiHex(value[index]))) {
            return Result<Uuid>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "UUID has an invalid canonical representation."));
        }
    }

    std::string canonical{value};
    std::transform(
        canonical.begin(),
        canonical.end(),
        canonical.begin(),
        [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return Result<Uuid>::success(Uuid{std::move(canonical)});
}

Result<std::string> validateOpaqueIdentifier(
    const std::string_view value,
    const std::size_t maximumBytes)
{
    if (value.empty() || value.size() > maximumBytes || value == "." || value == "..") {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Identifier is empty, reserved, or exceeds its byte limit."));
    }
    if (!std::all_of(value.begin(), value.end(), isSafeIdentifierCharacter)) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Identifier contains a character outside [A-Za-z0-9._-]."));
    }
    return Result<std::string>::success(std::string{value});
}

Result<IdempotencyKey> IdempotencyKey::create(const std::string_view value)
{
    if (value.empty() || value.size() > MaximumBytes) {
        return Result<IdempotencyKey>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Idempotency key must contain 1 to 256 bytes."));
    }
    for (const unsigned char character : value) {
        if (character < 0x20 || character == 0x7f) {
            return Result<IdempotencyKey>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Idempotency key cannot contain control characters."));
        }
    }
    return Result<IdempotencyKey>::success(IdempotencyKey{std::string{value}});
}

Result<Sha256Digest> Sha256Digest::parse(const std::string_view value)
{
    if (value.size() != 64 ||
        !std::all_of(value.begin(), value.end(), [](const char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        })) {
        return Result<Sha256Digest>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "SHA-256 digest must contain 64 lowercase hexadecimal characters."));
    }
    return Result<Sha256Digest>::success(Sha256Digest{std::string{value}});
}

} // namespace ForgeConductor::Domain
