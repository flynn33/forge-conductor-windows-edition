#include "ForgeConductor/Domain/LegacyMemoryModels.h"

#include <algorithm>
#include <utility>

namespace ForgeConductor::Domain {
namespace {

struct Utf8Scalar final {
    char32_t value{};
    std::size_t firstByte{};
    std::size_t nextByte{};
};

[[nodiscard]] bool isContinuation(const unsigned char value) noexcept
{
    return (value & 0xc0U) == 0x80U;
}

[[nodiscard]] bool decodeNext(
    const std::string_view text,
    std::size_t& offset,
    Utf8Scalar& scalar) noexcept
{
    if (offset >= text.size()) {
        return false;
    }

    const auto firstOffset = offset;
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7fU) {
        ++offset;
        scalar = Utf8Scalar{first, firstOffset, offset};
        return true;
    }

    std::size_t length{};
    char32_t value{};
    char32_t minimum{};
    if (first >= 0xc2U && first <= 0xdfU) {
        length = 2U;
        value = first & 0x1fU;
        minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
        length = 3U;
        value = first & 0x0fU;
        minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        length = 4U;
        value = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return false;
    }

    if (length > text.size() - offset) {
        return false;
    }
    for (std::size_t index = 1U; index < length; ++index) {
        const auto continuation =
            static_cast<unsigned char>(text[offset + index]);
        if (!isContinuation(continuation)) {
            return false;
        }
        value = static_cast<char32_t>((value << 6U) | (continuation & 0x3fU));
    }
    if (value < minimum || value > 0x10ffffU ||
        (value >= 0xd800U && value <= 0xdfffU)) {
        return false;
    }

    offset += length;
    scalar = Utf8Scalar{value, firstOffset, offset};
    return true;
}

[[nodiscard]] bool isUnicodeControl(const char32_t value) noexcept
{
    return value <= 0x1fU || (value >= 0x7fU && value <= 0x9fU);
}

[[nodiscard]] bool isUnicodeWhitespace(const char32_t value) noexcept
{
    return (value >= 0x09U && value <= 0x0dU) || value == 0x20U ||
           value == 0x85U || value == 0xa0U || value == 0x1680U ||
           (value >= 0x2000U && value <= 0x200aU) || value == 0x2028U ||
           value == 0x2029U || value == 0x202fU || value == 0x205fU ||
           value == 0x3000U;
}

[[nodiscard]] bool containsEmbeddedNull(const std::string_view value) noexcept
{
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] unsigned char foldAscii(const unsigned char value) noexcept
{
    if (value >= static_cast<unsigned char>('A') &&
        value <= static_cast<unsigned char>('Z')) {
        return static_cast<unsigned char>(value + ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] bool asciiCaseInsensitivePrefix(
    const std::string_view value,
    const std::string_view prefix) noexcept
{
    if (prefix.size() > value.size()) {
        return false;
    }
    for (std::size_t index{}; index < prefix.size(); ++index) {
        if (foldAscii(static_cast<unsigned char>(value[index])) !=
            foldAscii(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<std::string> trimUtf8(
    const std::string_view text,
    const std::string_view field)
{
    std::size_t offset{};
    std::size_t firstNonWhitespace = std::string_view::npos;
    std::size_t lastNonWhitespaceEnd{};
    while (offset < text.size()) {
        Utf8Scalar scalar;
        if (!decodeNext(text, offset, scalar)) {
            return Result<std::string>::failure(makeError(
                ErrorCodes::InvalidRequest,
                std::string{field} + " must be valid UTF-8."));
        }
        if (!isUnicodeWhitespace(scalar.value)) {
            if (firstNonWhitespace == std::string_view::npos) {
                firstNonWhitespace = scalar.firstByte;
            }
            lastNonWhitespaceEnd = scalar.nextByte;
        }
    }
    if (firstNonWhitespace == std::string_view::npos) {
        return Result<std::string>::success({});
    }
    return Result<std::string>::success(std::string{
        text.substr(
            firstNonWhitespace,
            lastNonWhitespaceEnd - firstNonWhitespace)});
}

[[nodiscard]] Result<std::optional<std::string>> normalizeFilter(
    const std::optional<std::string>& filter,
    const std::string_view field)
{
    if (!filter) {
        return Result<std::optional<std::string>>::success(std::nullopt);
    }
    if (filter->size() > LegacyMemoryLimits::MaximumFilterBytes) {
        return Result<std::optional<std::string>>::failure(makeError(
            ErrorCodes::PayloadTooLarge,
            std::string{field} + " exceeds 512 UTF-8 bytes."));
    }
    if (containsEmbeddedNull(*filter)) {
        return Result<std::optional<std::string>>::failure(makeError(
            ErrorCodes::InvalidRequest,
            std::string{field} + " cannot contain U+0000."));
    }
    if (!isValidUtf8(*filter)) {
        return Result<std::optional<std::string>>::failure(makeError(
            ErrorCodes::InvalidRequest,
            std::string{field} + " must be valid UTF-8."));
    }
    return Result<std::optional<std::string>>::success(*filter);
}

[[nodiscard]] Result<void> validateStoredTags(
    const std::vector<std::string>& tags)
{
    if (tags.size() > LegacyMemoryLimits::MaximumTagCount) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "A legacy-memory dependency returned too many stored tags."));
    }
    for (const auto& tag : tags) {
        if (tag.size() > LegacyMemoryLimits::MaximumTagBytes ||
            containsEmbeddedNull(tag) || !isValidUtf8(tag)) {
            return Result<void>::failure(makeError(
                ErrorCodes::IntegrityFailure,
                "A legacy-memory dependency returned an invalid stored tag."));
        }
    }
    return Result<void>::success();
}

} // namespace

bool isSystemMemoryKey(const std::string_view key) noexcept
{
    return key.starts_with("agent_run/") || key.starts_with("agent_active/") ||
           key.starts_with("continuity/");
}

bool isHiddenLegacyMemoryKey(const std::string_view key) noexcept
{
    return asciiCaseInsensitivePrefix(key, "agent_run/") ||
           asciiCaseInsensitivePrefix(key, "agent_active/") ||
           asciiCaseInsensitivePrefix(key, "continuity/");
}

bool isValidUtf8(const std::string_view value) noexcept
{
    std::size_t offset{};
    while (offset < value.size()) {
        Utf8Scalar scalar;
        if (!decodeNext(value, offset, scalar)) {
            return false;
        }
    }
    return true;
}

std::size_t normalizeLegacyMemoryLimit(const std::int64_t requested) noexcept
{
    if (requested < 1) {
        return 1U;
    }
    const auto maximum =
        static_cast<std::int64_t>(LegacyMemoryLimits::MaximumQueryLimit);
    if (requested > maximum) {
        return LegacyMemoryLimits::MaximumQueryLimit;
    }
    return static_cast<std::size_t>(requested);
}

Result<std::string> normalizeLegacyMemoryKey(const std::string_view key)
{
    if (key.size() > LegacyMemoryLimits::MaximumKeyBytes) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InvalidKey,
            "key is required: non-empty, max 512 bytes, valid UTF-8, and no "
            "Unicode control characters.",
            true));
    }
    auto normalized = trimUtf8(key, "Memory key");
    if (!normalized) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InvalidKey,
            "key is required: non-empty, max 512 bytes, valid UTF-8, and no "
            "Unicode control characters.",
            true));
    }
    auto value = std::move(normalized).value();
    if (value.empty() || value.size() > LegacyMemoryLimits::MaximumKeyBytes) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InvalidKey,
            "key is required: non-empty, max 512 bytes, valid UTF-8, and no "
            "Unicode control characters.",
            true));
    }

    std::size_t offset{};
    while (offset < value.size()) {
        Utf8Scalar scalar;
        if (!decodeNext(value, offset, scalar) || isUnicodeControl(scalar.value)) {
            return Result<std::string>::failure(makeError(
                ErrorCodes::InvalidKey,
                "key is required: non-empty, max 512 bytes, valid UTF-8, and no "
                "Unicode control characters.",
                true));
        }
    }
    return Result<std::string>::success(std::move(value));
}

Result<std::string> normalizeLegacyMemoryBody(
    const std::optional<std::string>& body)
{
    if (!body) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::MissingBody,
            "body (or content/value) is required.",
            true));
    }
    if (body->size() > LegacyMemoryLimits::MaximumBodyBytes) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::BodyTooLarge,
            "body exceeds 524288 UTF-8 bytes.",
            true));
    }
    if (containsEmbeddedNull(*body)) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Memory body cannot contain U+0000."));
    }
    if (!isValidUtf8(*body)) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Memory body must be valid UTF-8."));
    }
    try {
        return Result<std::string>::success(*body);
    } catch (...) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InternalFailure,
            "Memory body validation could not allocate its bounded result."));
    }
}

Result<std::vector<std::string>> prepareLegacyMemoryTags(
    const std::vector<std::string>& tags)
{
    if (tags.size() > LegacyMemoryLimits::MaximumTagCount) {
        return Result<std::vector<std::string>>::failure(makeError(
            ErrorCodes::LimitExceeded,
            "A memory note may contain at most 32 input tags."));
    }
    try {
        std::vector<std::string> prepared;
        prepared.reserve(tags.size());
        for (const auto& raw : tags) {
            if (raw.size() > LegacyMemoryLimits::MaximumTagBytes) {
                return Result<std::vector<std::string>>::failure(makeError(
                    ErrorCodes::PayloadTooLarge,
                    "A raw memory tag exceeds 128 UTF-8 bytes."));
            }
            if (containsEmbeddedNull(raw)) {
                return Result<std::vector<std::string>>::failure(makeError(
                    ErrorCodes::InvalidRequest,
                    "A memory tag cannot contain U+0000."));
            }
            auto normalized = trimUtf8(raw, "Memory tag");
            if (!normalized) {
                return Result<std::vector<std::string>>::failure(
                    std::move(normalized).error());
            }
            auto value = std::move(normalized).value();
            if (value.empty()) {
                continue;
            }
            if (value.size() > LegacyMemoryLimits::MaximumTagBytes) {
                return Result<std::vector<std::string>>::failure(makeError(
                    ErrorCodes::PayloadTooLarge,
                    "A memory tag exceeds 128 UTF-8 bytes."));
            }
            prepared.push_back(std::move(value));
        }
        return Result<std::vector<std::string>>::success(std::move(prepared));
    } catch (...) {
        return Result<std::vector<std::string>>::failure(makeError(
            ErrorCodes::InternalFailure,
            "Memory tags could not be normalized."));
    }
}

Result<std::string> normalizeLegacyMemoryGetRequest(
    const LegacyMemoryGetRequest& request)
{
    return normalizeLegacyMemoryKey(request.key);
}

Result<LegacyMemoryListQuery> normalizeLegacyMemoryListRequest(
    const LegacyMemoryListRequest& request)
{
    auto prefix = normalizeFilter(request.prefix, "Memory prefix filter");
    if (!prefix) {
        return Result<LegacyMemoryListQuery>::failure(std::move(prefix).error());
    }
    auto tag = normalizeFilter(request.tag, "Memory tag filter");
    if (!tag) {
        return Result<LegacyMemoryListQuery>::failure(std::move(tag).error());
    }
    return Result<LegacyMemoryListQuery>::success(LegacyMemoryListQuery{
        std::move(prefix).value(),
        std::move(tag).value(),
        request.includeSystem,
        request.includeBody,
        normalizeLegacyMemoryLimit(request.requestedLimit)});
}

Result<std::string> normalizeLegacyMemoryRemoveRequest(
    const LegacyMemoryRemoveRequest& request)
{
    return normalizeLegacyMemoryKey(request.key);
}

Result<LegacyMemorySearchQuery> normalizeLegacyMemorySearchRequest(
    const LegacyMemorySearchRequest& request)
{
    if (!request.query) {
        return Result<LegacyMemorySearchQuery>::failure(makeError(
            ErrorCodes::MissingQuery,
            "query is required.",
            true));
    }
    if (request.query->size() > LegacyMemoryLimits::MaximumQueryBytes) {
        return Result<LegacyMemorySearchQuery>::failure(makeError(
            ErrorCodes::PayloadTooLarge,
            "Memory search query exceeds 4096 UTF-8 bytes."));
    }
    if (containsEmbeddedNull(*request.query)) {
        return Result<LegacyMemorySearchQuery>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Memory search query cannot contain U+0000."));
    }
    auto query = trimUtf8(*request.query, "Memory search query");
    if (!query) {
        return Result<LegacyMemorySearchQuery>::failure(
            std::move(query).error());
    }
    auto value = std::move(query).value();
    if (value.empty()) {
        return Result<LegacyMemorySearchQuery>::failure(makeError(
            ErrorCodes::EmptyQuery,
            "query must not be empty.",
            true));
    }
    return Result<LegacyMemorySearchQuery>::success(LegacyMemorySearchQuery{
        std::move(value),
        request.includeSystem,
        request.includeBody,
        normalizeLegacyMemoryLimit(request.requestedLimit)});
}

Result<void> validateMemoryNote(const MemoryNote& note)
{
    auto key = normalizeLegacyMemoryKey(note.key);
    if (!key) {
        return Result<void>::failure(std::move(key).error());
    }
    if (key.value() != note.key) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "A legacy-memory dependency returned a non-canonical key."));
    }
    if (note.body.size() > LegacyMemoryLimits::MaximumBodyBytes) {
        return Result<void>::failure(makeError(
            ErrorCodes::BodyTooLarge,
            "Memory body exceeds 524288 UTF-8 bytes."));
    }
    if (containsEmbeddedNull(note.body) || !isValidUtf8(note.body)) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "A legacy-memory dependency returned a malformed UTF-8 body."));
    }
    return validateStoredTags(note.tags);
}

Result<void> validateLegacyMemoryProjection(
    const LegacyMemoryNoteProjection& projection,
    const bool includeBody)
{
    auto key = normalizeLegacyMemoryKey(projection.key);
    if (!key || key.value() != projection.key) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "A legacy-memory dependency returned an invalid projected key."));
    }
    auto tags = validateStoredTags(projection.tags);
    if (!tags) {
        return tags;
    }
    if (projection.bodyUtf8Bytes > LegacyMemoryLimits::MaximumBodyBytes) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "A legacy-memory dependency returned an oversized body length."));
    }
    if (projection.body.has_value() != includeBody) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "A legacy-memory dependency violated the requested body projection."));
    }
    if (projection.body &&
        (containsEmbeddedNull(*projection.body) ||
         !isValidUtf8(*projection.body) ||
         projection.body->size() != projection.bodyUtf8Bytes)) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "A legacy-memory dependency returned an inconsistent body projection."));
    }
    return Result<void>::success();
}

} // namespace ForgeConductor::Domain
