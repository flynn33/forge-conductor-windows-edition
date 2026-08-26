#include "ForgeConductor/Domain/ProjectMemoryModels.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace ForgeConductor::Domain {
namespace {

[[nodiscard]] std::string trimAscii(std::string value)
{
    const auto isWhitespace = [](const unsigned char character) {
        return std::isspace(character) != 0;
    };
    const auto first = std::find_if_not(value.begin(), value.end(), isWhitespace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isWhitespace).base();
    if (first >= last) {
        return {};
    }
    return std::string{first, last};
}

[[nodiscard]] std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool isIdentifier(const std::string_view value) noexcept
{
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' || character == '-' ||
               character == '.';
    });
}

[[nodiscard]] Result<void> requireMaximumBytes(
    const std::string_view value,
    const std::size_t maximum,
    const std::string_view field)
{
    if (value.size() <= maximum) {
        return Result<void>::success();
    }
    return Result<void>::failure(makeError(
        ErrorCodes::PayloadTooLarge,
        std::string{field} + " exceeds " + std::to_string(maximum) + " UTF-8 bytes."));
}

[[nodiscard]] Result<void> normalizeKindFilters(
    std::vector<std::string>& kinds,
    const ProjectMemoryLimits& limits)
{
    if (kinds.size() > limits.maximumPageCount) {
        return Result<void>::failure(makeError(
            ErrorCodes::PayloadTooLarge,
            "Project memory kind filters exceed the bounded page maximum."));
    }
    for (auto& raw : kinds) {
        auto kind = lowerAscii(trimAscii(std::move(raw)));
        if (kind.empty() || kind.size() > 64 || !isIdentifier(kind)) {
            return Result<void>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Memory kind filters must contain 1...64 ASCII identifier bytes."));
        }
        raw = std::move(kind);
    }
    return Result<void>::success();
}

[[nodiscard]] int base64Value(const unsigned char character) noexcept
{
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

[[nodiscard]] Result<void> validateCursor(
    const std::optional<std::string>& cursor) noexcept
{
    if (!cursor || cursor->empty()) {
        return Result<void>::success();
    }

    const std::string_view encoded{*cursor};
    if ((encoded.size() % 4U) != 0U) {
        return Result<void>::failure(
            makeError(ErrorCodes::InvalidRequest, "Project memory cursor is invalid."));
    }

    // The sole current-source encoder emits base64("v1:<nonnegative Int>").
    // This fixed buffer follows the numeric range instead of inventing a cursor byte cap.
    std::array<unsigned char, 3U + std::numeric_limits<std::size_t>::digits10 + 1U>
        decoded{};
    std::size_t decodedCount{};
    const auto append = [&](const unsigned char value) noexcept {
        if (decodedCount >= decoded.size()) {
            return false;
        }
        decoded[decodedCount++] = value;
        return true;
    };

    for (std::size_t offset = 0; offset < encoded.size(); offset += 4U) {
        const bool finalQuartet = offset + 4U == encoded.size();
        const auto first = base64Value(static_cast<unsigned char>(encoded[offset]));
        const auto second = base64Value(static_cast<unsigned char>(encoded[offset + 1U]));
        if (first < 0 || second < 0) {
            return Result<void>::failure(
                makeError(ErrorCodes::InvalidRequest, "Project memory cursor is invalid."));
        }

        const auto thirdCharacter = encoded[offset + 2U];
        const auto fourthCharacter = encoded[offset + 3U];
        const bool thirdPadding = thirdCharacter == '=';
        const bool fourthPadding = fourthCharacter == '=';
        const auto third = thirdPadding
            ? 0
            : base64Value(static_cast<unsigned char>(thirdCharacter));
        const auto fourth = fourthPadding
            ? 0
            : base64Value(static_cast<unsigned char>(fourthCharacter));
        if (third < 0 || fourth < 0 ||
            (thirdPadding && !fourthPadding) ||
            ((thirdPadding || fourthPadding) && !finalQuartet) ||
            (thirdPadding && (second & 0x0F) != 0) ||
            (fourthPadding && !thirdPadding && (third & 0x03) != 0)) {
            return Result<void>::failure(
                makeError(ErrorCodes::InvalidRequest, "Project memory cursor is invalid."));
        }

        if (!append(static_cast<unsigned char>((first << 2) | (second >> 4))) ||
            (!thirdPadding &&
             !append(static_cast<unsigned char>(((second & 0x0F) << 4) | (third >> 2)))) ||
            (!fourthPadding &&
             !append(static_cast<unsigned char>(((third & 0x03) << 6) | fourth)))) {
            return Result<void>::failure(
                makeError(ErrorCodes::InvalidRequest, "Project memory cursor is out of range."));
        }
    }

    if (decodedCount < 4U || decoded[0] != 'v' || decoded[1] != '1' ||
        decoded[2] != ':') {
        return Result<void>::failure(
            makeError(ErrorCodes::InvalidRequest, "Project memory cursor is invalid."));
    }

    std::size_t numericValue{};
    const auto digitCount = decodedCount - 3U;
    if (digitCount > 1U && decoded[3] == '0') {
        return Result<void>::failure(
            makeError(ErrorCodes::InvalidRequest, "Project memory cursor is not canonical."));
    }
    for (std::size_t index = 3U; index < decodedCount; ++index) {
        const auto character = decoded[index];
        if (character < '0' || character > '9') {
            return Result<void>::failure(
                makeError(ErrorCodes::InvalidRequest, "Project memory cursor is invalid."));
        }
        const auto digit = static_cast<std::size_t>(character - '0');
        if (numericValue > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return Result<void>::failure(
                makeError(ErrorCodes::InvalidRequest, "Project memory cursor is out of range."));
        }
        numericValue = numericValue * 10U + digit;
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validateObservedArtifactBytes(
    const std::size_t observedArtifactBytes,
    const ProjectMemoryLimits& limits)
{
    if (observedArtifactBytes > limits.maximumArtifactBytes) {
        return Result<void>::failure(makeError(
            ErrorCodes::PayloadTooLarge,
            "Project memory artifact exceeds the 32 MiB limit."));
    }
    return Result<void>::success();
}

} // namespace

ProjectMemoryLimits projectMemoryLimitsForProfile(const ResourceProfile profile) noexcept
{
    ProjectMemoryLimits limits;
    limits.maximumOpenProjects = budgetsForProfile(profile).openProjectRepositoriesMaximum;
    return limits;
}

std::string_view wireName(const MemoryWriteDisposition value) noexcept
{
    switch (value) {
    case MemoryWriteDisposition::Inserted: return "inserted";
    case MemoryWriteDisposition::Deduplicated: return "deduplicated";
    case MemoryWriteDisposition::Updated: return "updated";
    }
    return "inserted";
}

std::string_view wireName(const ForgetDisposition value) noexcept
{
    switch (value) {
    case ForgetDisposition::Tombstoned: return "tombstoned";
    case ForgetDisposition::NotFound: return "not_found";
    }
    return "not_found";
}

std::string_view wireName(const LinkDisposition value) noexcept
{
    switch (value) {
    case LinkDisposition::Inserted: return "inserted";
    case LinkDisposition::Deduplicated: return "deduplicated";
    }
    return "inserted";
}

std::string_view wireName(const ImportDisposition value) noexcept
{
    switch (value) {
    case ImportDisposition::Preview: return "preview";
    case ImportDisposition::Imported: return "imported";
    }
    return "preview";
}

Result<std::vector<std::string>> normalizeProjectMemoryTags(
    std::vector<std::string> tags,
    const ProjectMemoryLimits& limits)
{
    if (tags.size() > limits.maximumTagCount) {
        return Result<std::vector<std::string>>::failure(
            makeError(ErrorCodes::PayloadTooLarge, "Project memory contains too many tags."));
    }

    std::vector<std::string> normalized;
    normalized.reserve(tags.size());
    for (auto& raw : tags) {
        auto tag = lowerAscii(trimAscii(std::move(raw)));
        if (tag.empty()) {
            continue;
        }
        if (tag.size() > limits.maximumTagBytes) {
            return Result<std::vector<std::string>>::failure(makeError(
                ErrorCodes::PayloadTooLarge,
                "Project memory tag exceeds " +
                    std::to_string(limits.maximumTagBytes) + " UTF-8 bytes."));
        }
        normalized.push_back(std::move(tag));
    }

    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    return Result<std::vector<std::string>>::success(std::move(normalized));
}

Result<ProjectMemoryWrite> validateProjectMemoryWrite(
    ProjectMemoryWrite write,
    const ProjectMemoryLimits& limits)
{
    write.kind = lowerAscii(trimAscii(std::move(write.kind)));
    if (write.kind.empty() || write.kind.size() > 64 || !isIdentifier(write.kind)) {
        return Result<ProjectMemoryWrite>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "kind must contain 1...64 ASCII identifier bytes."));
    }

    write.title = trimAscii(std::move(write.title));
    write.summary = trimAscii(std::move(write.summary));
    if (write.title.empty() || write.summary.empty()) {
        return Result<ProjectMemoryWrite>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "title and summary are required."));
    }

    for (const auto [value, maximum, field] : {
             std::tuple<std::string_view, std::size_t, std::string_view>{
                 write.title, limits.maximumTitleBytes, "title"},
             {write.summary, limits.maximumSummaryBytes, "summary"}}) {
        auto result = requireMaximumBytes(value, maximum, field);
        if (!result) {
            return Result<ProjectMemoryWrite>::failure(std::move(result).error());
        }
    }
    if (write.body) {
        auto result = requireMaximumBytes(*write.body, limits.maximumBodyBytes, "body");
        if (!result) {
            return Result<ProjectMemoryWrite>::failure(std::move(result).error());
        }
    }
    if (write.sourceReference) {
        auto result = requireMaximumBytes(
            *write.sourceReference,
            limits.maximumSourceReferenceBytes,
            "source_reference");
        if (!result) {
            return Result<ProjectMemoryWrite>::failure(std::move(result).error());
        }
    }

    if (!std::isfinite(write.importance) || !std::isfinite(write.confidence) ||
        write.importance < 0.0 || write.importance > 1.0 ||
        write.confidence < 0.0 || write.confidence > 1.0) {
        return Result<ProjectMemoryWrite>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "importance and confidence must be finite values within 0...1."));
    }

    auto tags = normalizeProjectMemoryTags(std::move(write.tags), limits);
    if (!tags) {
        return Result<ProjectMemoryWrite>::failure(std::move(tags).error());
    }
    write.tags = std::move(tags).value();
    if (write.relatedIds.size() > limits.maximumRelatedIdCount) {
        write.relatedIds.erase(
            write.relatedIds.begin() + static_cast<std::ptrdiff_t>(limits.maximumRelatedIdCount),
            write.relatedIds.end());
    }
    return Result<ProjectMemoryWrite>::success(std::move(write));
}

Result<std::vector<ProjectMemoryWrite>> validateProjectMemoryBatch(
    std::vector<ProjectMemoryWrite> writes,
    const ProjectMemoryLimits& limits)
{
    if (writes.empty() || writes.size() > limits.maximumBatchCount) {
        return Result<std::vector<ProjectMemoryWrite>>::failure(makeError(
            ErrorCodes::PayloadTooLarge,
            "Project memory batch count must be within 1..." +
                std::to_string(limits.maximumBatchCount) + "."));
    }

    std::vector<ProjectMemoryWrite> normalized;
    normalized.reserve(writes.size());
    std::size_t encodedBytes{};
    for (auto& write : writes) {
        auto validated = validateProjectMemoryWrite(std::move(write), limits);
        if (!validated) {
            return Result<std::vector<ProjectMemoryWrite>>::failure(
                std::move(validated).error());
        }
        auto item = std::move(validated).value();
        const auto itemBytes = item.title.size() + item.summary.size() +
                               (item.body ? item.body->size() : 0U);
        if (itemBytes > limits.maximumBatchBytes -
                            std::min(encodedBytes, limits.maximumBatchBytes)) {
            return Result<std::vector<ProjectMemoryWrite>>::failure(makeError(
                ErrorCodes::PayloadTooLarge,
                "Project memory batch exceeds " +
                    std::to_string(limits.maximumBatchBytes) + " UTF-8 bytes."));
        }
        encodedBytes += itemBytes;
        normalized.push_back(std::move(item));
    }
    return Result<std::vector<ProjectMemoryWrite>>::success(std::move(normalized));
}

Result<void> validateProjectMemoryQuery(
    const std::string_view query,
    const ProjectMemoryLimits& limits)
{
    auto normalized = trimAscii(std::string{query});
    if (normalized.empty()) {
        return Result<void>::failure(
            makeError(ErrorCodes::InvalidRequest, "query is required."));
    }
    return requireMaximumBytes(normalized, limits.maximumQueryBytes, "query");
}
Result<SearchProjectMemoryRequest> validateSearchProjectMemoryRequest(
    SearchProjectMemoryRequest request,
    const ProjectMemoryLimits& limits)
{
    request.query = trimAscii(std::move(request.query));
    auto query = validateProjectMemoryQuery(request.query, limits);
    if (!query) {
        return Result<SearchProjectMemoryRequest>::failure(std::move(query).error());
    }
    auto kinds = normalizeKindFilters(request.kinds, limits);
    if (!kinds) {
        return Result<SearchProjectMemoryRequest>::failure(std::move(kinds).error());
    }
    auto tags = normalizeProjectMemoryTags(std::move(request.tags), limits);
    if (!tags) {
        return Result<SearchProjectMemoryRequest>::failure(std::move(tags).error());
    }
    request.tags = std::move(tags).value();
    auto cursor = validateCursor(request.cursor);
    if (!cursor) {
        return Result<SearchProjectMemoryRequest>::failure(std::move(cursor).error());
    }
    request.limit = normalizeProjectMemoryPageLimit(request.limit, limits);
    request.maximumResponseBytes =
        normalizeProjectMemoryResponseLimit(request.maximumResponseBytes, limits);
    return Result<SearchProjectMemoryRequest>::success(std::move(request));
}

Result<void> validateGetProjectMemoryRequest(
    const GetProjectMemoryRequest& request,
    const ProjectMemoryLimits& limits)
{
    if (request.ids.empty() || request.ids.size() > limits.maximumPageCount) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Project memory ids must contain 1...100 values."));
    }
    if (request.maximumResponseBytes < 1024U ||
        request.maximumResponseBytes > limits.maximumResponseBytes) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Project memory get response bytes must be within 1024...262144."));
    }
    return Result<void>::success();
}

Result<UpdateProjectMemoryRequest> validateUpdateProjectMemoryRequest(
    UpdateProjectMemoryRequest request,
    const ProjectMemoryLimits& limits)
{
    if (request.expectedVersion == 0U) {
        return Result<UpdateProjectMemoryRequest>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Project memory expected_version must be positive."));
    }
    for (auto* value : {&request.title, &request.summary}) {
        if (!*value) {
            continue;
        }
        **value = trimAscii(std::move(**value));
        if ((**value).empty()) {
            return Result<UpdateProjectMemoryRequest>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Project memory title and summary updates must not be empty."));
        }
    }
    for (const auto [value, maximum, field] : {
             std::tuple<const std::optional<std::string>*, std::size_t, std::string_view>{
                 &request.title, limits.maximumTitleBytes, "title"},
             {&request.summary, limits.maximumSummaryBytes, "summary"},
             {&request.body, limits.maximumBodyBytes, "body"}}) {
        if (!*value) {
            continue;
        }
        auto bounded = requireMaximumBytes(**value, maximum, field);
        if (!bounded) {
            return Result<UpdateProjectMemoryRequest>::failure(
                std::move(bounded).error());
        }
    }
    if (request.tags) {
        auto tags = normalizeProjectMemoryTags(std::move(*request.tags), limits);
        if (!tags) {
            return Result<UpdateProjectMemoryRequest>::failure(std::move(tags).error());
        }
        request.tags = std::move(tags).value();
    }
    return Result<UpdateProjectMemoryRequest>::success(std::move(request));
}

Result<ListRecentProjectMemoryRequest> validateListRecentProjectMemoryRequest(
    ListRecentProjectMemoryRequest request,
    const ProjectMemoryLimits& limits)
{
    auto kinds = normalizeKindFilters(request.kinds, limits);
    if (!kinds) {
        return Result<ListRecentProjectMemoryRequest>::failure(std::move(kinds).error());
    }
    auto cursor = validateCursor(request.cursor);
    if (!cursor) {
        return Result<ListRecentProjectMemoryRequest>::failure(std::move(cursor).error());
    }
    request.limit = normalizeProjectMemoryPageLimit(request.limit, limits);
    request.maximumResponseBytes =
        normalizeProjectMemoryResponseLimit(request.maximumResponseBytes, limits);
    return Result<ListRecentProjectMemoryRequest>::success(std::move(request));
}

Result<LinkProjectMemoryRequest> validateLinkProjectMemoryRequest(
    LinkProjectMemoryRequest request)
{
    request.relation = trimAscii(std::move(request.relation));
    if (request.relation.empty() || request.relation.size() > 128U) {
        return Result<LinkProjectMemoryRequest>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Project memory relation must contain 1...128 UTF-8 bytes."));
    }
    return Result<LinkProjectMemoryRequest>::success(std::move(request));
}

Result<void> validateExportProjectMemoryRequest(
    const ExportProjectMemoryRequest& request,
    const std::size_t observedArtifactBytes,
    const ProjectMemoryLimits& limits)
{
    (void)request;
    return validateObservedArtifactBytes(observedArtifactBytes, limits);
}

Result<void> validateImportProjectMemoryRequest(
    const ImportProjectMemoryRequest& request,
    const std::size_t observedArtifactBytes,
    const ProjectMemoryLimits& limits)
{
    (void)request;
    return validateObservedArtifactBytes(observedArtifactBytes, limits);
}

Result<void> validateProjectMemoryStatusRequest(
    const ProjectMemoryStatusRequest& request) noexcept
{
    (void)request;
    return Result<void>::success();
}


Result<void> validateProjectMemoryDeadline(const std::chrono::milliseconds deadline)
{
    if (deadline < MinimumProjectMemoryDeadline || deadline > MaximumProjectMemoryDeadline) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "deadline must be within 1...60000 milliseconds."));
    }
    return Result<void>::success();
}

Result<void> validateDestructiveConfirmation(
    const DestructiveConfirmation& confirmation,
    const std::string_view expectedAction,
    const std::string_view expectedScope,
    const std::string_view expectedToken)
{
    if (expectedAction.empty() || expectedScope.empty() || expectedToken.empty()) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Expected destructive confirmation values must not be empty."));
    }
    if (confirmation.action != expectedAction || confirmation.scope != expectedScope ||
        confirmation.token != expectedToken) {
        return Result<void>::failure(makeError(
            ErrorCodes::Unauthorized,
            "Destructive confirmation does not match the requested action and scope."));
    }
    return Result<void>::success();
}

std::size_t normalizeProjectMemoryPageLimit(
    const std::size_t requested,
    const ProjectMemoryLimits& limits) noexcept
{
    return std::clamp(requested, std::size_t{1}, limits.maximumPageCount);
}

std::size_t normalizeProjectMemoryResponseLimit(
    const std::size_t requested,
    const ProjectMemoryLimits& limits) noexcept
{
    return std::clamp(requested, std::size_t{1024}, limits.maximumResponseBytes);
}

} // namespace ForgeConductor::Domain
