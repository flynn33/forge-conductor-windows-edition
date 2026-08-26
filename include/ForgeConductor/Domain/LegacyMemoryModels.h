#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Domain {

namespace ErrorCodes {

inline constexpr std::string_view InvalidKey = "invalid_key";
inline constexpr std::string_view MissingBody = "missing_body";
inline constexpr std::string_view BodyTooLarge = "body_too_large";
inline constexpr std::string_view MissingQuery = "missing_query";
inline constexpr std::string_view EmptyQuery = "empty_query";
inline constexpr std::string_view StoreError = "store_error";

} // namespace ErrorCodes

struct LegacyMemoryLimits final {
    static constexpr std::size_t MaximumKeyBytes = 512U;
    static constexpr std::size_t MaximumBodyBytes = 512U * 1024U;
    static constexpr std::size_t MaximumTagCount = 32U;
    static constexpr std::size_t MaximumTagBytes = 128U;
    static constexpr std::size_t MaximumFilterBytes = 512U;
    static constexpr std::size_t MaximumQueryBytes = 4U * 1024U;
    static constexpr std::size_t DefaultQueryLimit = 50U;
    static constexpr std::size_t MaximumQueryLimit = 200U;
};

struct MemoryNote final {
    std::string key;
    std::string body;
    std::vector<std::string> tags;
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;

    bool operator==(const MemoryNote&) const = default;
};

struct LegacyMemorySetRequest final {
    std::string key;
    std::optional<std::string> body;
    std::vector<std::string> tags;
};

// Canonical write value passed from the application layer to persistence.
struct LegacyMemoryUpsert final {
    std::string key;
    std::string body;
    std::vector<std::string> tags;

    bool operator==(const LegacyMemoryUpsert&) const = default;
};

struct LegacyMemoryGetRequest final {
    std::string key;
};

struct LegacyMemoryListRequest final {
    std::optional<std::string> prefix;
    std::optional<std::string> tag;
    bool includeSystem{};
    bool includeBody{};
    std::int64_t requestedLimit{
        static_cast<std::int64_t>(LegacyMemoryLimits::DefaultQueryLimit)};
};

// Canonical query passed from the application layer to persistence.
struct LegacyMemoryListQuery final {
    std::optional<std::string> prefix;
    std::optional<std::string> tag;
    bool includeSystem{};
    bool includeBody{};
    std::size_t limit{LegacyMemoryLimits::DefaultQueryLimit};

    bool operator==(const LegacyMemoryListQuery&) const = default;
};

struct LegacyMemoryRemoveRequest final {
    std::string key;
};

struct LegacyMemorySearchRequest final {
    std::optional<std::string> query;
    bool includeSystem{};
    bool includeBody{true};
    std::int64_t requestedLimit{
        static_cast<std::int64_t>(LegacyMemoryLimits::DefaultQueryLimit)};
};

// Canonical query passed from the application layer to persistence.
struct LegacyMemorySearchQuery final {
    std::string query;
    bool includeSystem{};
    bool includeBody{true};
    std::size_t limit{LegacyMemoryLimits::DefaultQueryLimit};

    bool operator==(const LegacyMemorySearchQuery&) const = default;
};

// A repository must leave body disengaged when includeBody is false. This lets
// list/search avoid reading or materializing large bodies while retaining the
// legacy body_chars behavior through bodyUtf8Bytes.
struct LegacyMemoryNoteProjection final {
    std::string key;
    std::optional<std::string> body;
    std::size_t bodyUtf8Bytes{};
    std::vector<std::string> tags;
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;

    bool operator==(const LegacyMemoryNoteProjection&) const = default;
};

struct LegacyMemorySetOutcome final {
    MemoryNote note;
    bool stored{true};
};

struct LegacyMemoryGetOutcome final {
    std::string key;
    std::optional<MemoryNote> note;
};

struct LegacyMemoryListOutcome final {
    std::vector<LegacyMemoryNoteProjection> notes;
    std::size_t visibleTotal{};
};

struct LegacyMemoryDeleteOutcome final {
    std::string key;
    bool deleted{};
    bool existed{};
    bool systemKey{};
};

struct LegacyMemorySearchOutcome final {
    std::string query;
    std::vector<LegacyMemoryNoteProjection> notes;
};

struct LegacyMemoryPurgeOutcome final {
    std::size_t notesRemoved{};
    bool verified{};
};

[[nodiscard]] bool isSystemMemoryKey(std::string_view key) noexcept;
[[nodiscard]] bool isHiddenLegacyMemoryKey(std::string_view key) noexcept;

[[nodiscard]] std::size_t normalizeLegacyMemoryLimit(
    std::int64_t requested) noexcept;

[[nodiscard]] Result<std::string> normalizeLegacyMemoryKey(
    std::string_view key);
[[nodiscard]] Result<std::string> normalizeLegacyMemoryBody(
    const std::optional<std::string>& body);
// Validates the raw bounded collection, trims each tag, drops empty values,
// and preserves input order and duplicate spellings for a caller-supplied
// canonical-equivalence policy.
[[nodiscard]] Result<std::vector<std::string>> prepareLegacyMemoryTags(
    const std::vector<std::string>& tags);
[[nodiscard]] Result<std::string> normalizeLegacyMemoryGetRequest(
    const LegacyMemoryGetRequest& request);
[[nodiscard]] Result<LegacyMemoryListQuery> normalizeLegacyMemoryListRequest(
    const LegacyMemoryListRequest& request);
[[nodiscard]] Result<std::string> normalizeLegacyMemoryRemoveRequest(
    const LegacyMemoryRemoveRequest& request);
[[nodiscard]] Result<LegacyMemorySearchQuery> normalizeLegacyMemorySearchRequest(
    const LegacyMemorySearchRequest& request);

[[nodiscard]] Result<void> validateMemoryNote(const MemoryNote& note);
[[nodiscard]] Result<void> validateLegacyMemoryProjection(
    const LegacyMemoryNoteProjection& projection,
    bool includeBody);

} // namespace ForgeConductor::Domain
