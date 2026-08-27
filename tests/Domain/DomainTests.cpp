#include "ForgeConductor/Domain/Domain.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Domain = ForgeConductor::Domain;

namespace {

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define REQUIRE(expression)                                                                     \
    do {                                                                                        \
        if (!(expression)) {                                                                    \
            throw TestFailure{std::string{"Requirement failed: "} + #expression + " at line " + \
                              std::to_string(__LINE__)};                                         \
        }                                                                                       \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw TestFailure{"Unexpected failure: " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parsed(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::UtcTimePoint fixedTime(const std::int64_t seconds)
{
    return Domain::UtcTimePoint{std::chrono::seconds{seconds}};
}

[[nodiscard]] Domain::ProjectId projectId()
{
    return parsed<Domain::ProjectId>("11111111-1111-4111-8111-111111111111");
}

[[nodiscard]] Domain::SessionId predecessorSessionId()
{
    return parsed<Domain::SessionId>("22222222-2222-4222-8222-222222222222");
}

[[nodiscard]] Domain::SessionId successorSessionId()
{
    return parsed<Domain::SessionId>("33333333-3333-4333-8333-333333333333");
}

[[nodiscard]] Domain::ContinuityOperationId continuityOperationId()
{
    return parsed<Domain::ContinuityOperationId>("44444444-4444-4444-8444-444444444444");
}

[[nodiscard]] Domain::ContinuityHandoffId continuityHandoffId()
{
    return parsed<Domain::ContinuityHandoffId>("55555555-5555-4555-8555-555555555555");
}

[[nodiscard]] Domain::AdapterId adapterId()
{
    return parsed<Domain::AdapterId>("forge-native-session-host");
}

[[nodiscard]] Domain::Sha256Digest digest(const char value)
{
    return parsed<Domain::Sha256Digest>(std::string(64, value));
}

[[nodiscard]] Domain::ContinuityHandoff validHandoff()
{
    return Domain::ContinuityHandoff{
        continuityHandoffId(),
        continuityOperationId(),
        fixedTime(100),
        Domain::ContinuityProject{
            projectId(),
            "Forge Conductor",
            take(Domain::PathText::create("C:\\workspace")),
            "main",
            "0123456789abcdef",
            {"M include/ForgeConductor/Domain/ContinuityModels.h"}},
        Domain::ContinuitySession{predecessorSessionId(), std::nullopt, "model", "provider"},
        Domain::ContinuitySession{successorSessionId(), std::nullopt, "model", "provider"},
        "Complete the Windows port",
        {"Do not weaken security"},
        Domain::ContinuityCurrentWork{
            "P05",
            "domain-models",
            "Implement portable Domain models",
            {take(Domain::PathText::create(
                "C:\\workspace\\include\\ForgeConductor\\Domain\\ContinuityModels.h"))}},
        {Domain::ContinuityWorkEntry{"inventory", "Inventory complete", "completed"}},
        {Domain::ContinuityWorkEntry{"tests", "Run unit tests", "open"}},
        {Domain::ContinuityDecision{"Use C++20 value objects", "Required by governance"}},
        Domain::ContinuityValidation{{"G04"}, {"G05"}, {}},
        {},
        {},
        {Domain::ContinuityNextAction{1, "Run tests", std::nullopt, "All tests pass"}},
        Domain::ContinuityHostState{
            adapterId(), Domain::ContinuityState::BootstrapSending, "provider_exact", {}},
        digest('a'),
        true};
}

[[nodiscard]] Domain::ContinuityOperation validOperation()
{
    return Domain::ContinuityOperation{
        continuityOperationId(),
        projectId(),
        predecessorSessionId(),
        successorSessionId(),
        continuityHandoffId(),
        Domain::ContinuityState::BootstrapSending,
        1,
        adapterId(),
        take(Domain::IdempotencyKey::create("rollover:project:1")),
        std::nullopt,
        std::nullopt,
        fixedTime(90),
        fixedTime(100),
        std::nullopt,
        std::nullopt,
        digest('b')};
}

void resultAndIdentifierBoundaries()
{
    auto success = Domain::Result<int>::success(42);
    REQUIRE(success);
    REQUIRE(success.value() == 42);
    auto failure = Domain::Result<int>::failure(
        Domain::makeError(Domain::ErrorCodes::Conflict, "conflict"));
    REQUIRE(!failure);
    REQUIRE(failure.error().code == Domain::ErrorCodes::Conflict);

    auto voidFailure = Domain::Result<void>::failure(
        Domain::makeError(Domain::ErrorCodes::Cancelled, "cancelled"));
    bool threw{};
    try {
        voidFailure.value();
    } catch (const std::bad_variant_access&) {
        threw = true;
    }
    REQUIRE(threw);

    auto uuid = take(Domain::Uuid::parse("ABCDEFAB-CDEF-4ABC-8DEF-ABCDEFABCDEF"));
    REQUIRE(uuid.value() == "abcdefab-cdef-4abc-8def-abcdefabcdef");
    REQUIRE(!Domain::Uuid::parse("not-a-uuid"));
    REQUIRE(!Domain::Uuid::parse("abcdefab-cdef-4abc-xdef-abcdefabcdef"));
    REQUIRE(!Domain::ClientId::parse("../escape"));
    REQUIRE(take(Domain::ClientId::parse("client-1")).value() == "client-1");
    REQUIRE(!Domain::IdempotencyKey::create(""));
    REQUIRE(!Domain::Sha256Digest::parse(std::string(63, 'a')));
    REQUIRE(!Domain::Sha256Digest::parse(std::string(64, 'A')));
    REQUIRE(take(Domain::Sha256Digest::parse(std::string(64, 'a'))).value() ==
            std::string(64, 'a'));
}

void pathBoundaries()
{
    REQUIRE(!Domain::PathText::create(""));
    const std::string embeddedNul{"C:\\safe\0escape", 14};
    REQUIRE(!Domain::PathText::create(std::string_view{embeddedNul.data(), embeddedNul.size()}));
    REQUIRE(!Domain::PathText::create(std::string(Domain::PathText::MaximumBytes + 1, 'x')));
}

void resourceAndConfigurationBoundaries()
{
    constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
    REQUIRE(Domain::selectResourceProfile(8 * gibibyte) ==
            Domain::ResourceProfile::Constrained8GiB);
    REQUIRE(Domain::selectResourceProfile(16 * gibibyte) ==
            Domain::ResourceProfile::Standard16GiB);
    REQUIRE(Domain::selectResourceProfile(32 * gibibyte) ==
            Domain::ResourceProfile::Expanded32GiBPlus);
    const auto constrained = Domain::budgetsForProfile(Domain::ResourceProfile::Constrained8GiB);
    const auto expanded = Domain::budgetsForProfile(Domain::ResourceProfile::Expanded32GiBPlus);
    REQUIRE(constrained.telemetryPendingSnapshotsMaximum == 1);
    REQUIRE(constrained.openProjectRepositoriesMaximum == 4);
    REQUIRE(expanded.openProjectRepositoriesMaximum == 16);
    REQUIRE(expanded.toolStdoutBytesMaximum == 80'000);
    REQUIRE(expanded.toolStderrBytesMaximum == 20'000);

    auto config = Domain::defaultAppConfig();
    REQUIRE(Domain::validateAppConfig(config));
    static_assert(Domain::MaximumAppConfigAllowedRootCount == 32U);
    const auto allowedRoot = take(Domain::PathText::create(R"(C:\workspace)"));
    config.allowedRoots.assign(Domain::MaximumAppConfigAllowedRootCount, allowedRoot);
    REQUIRE(Domain::validateAppConfig(config));
    config.allowedRoots.push_back(allowedRoot);
    const auto tooManyAllowedRoots = Domain::validateAppConfig(config);
    REQUIRE(!tooManyAllowedRoots);
    REQUIRE(tooManyAllowedRoots.error().code == Domain::ErrorCodes::LimitExceeded);
    config.allowedRoots.clear();
    REQUIRE(Domain::validateAppConfig(config));

    REQUIRE(config.dashboard.host == "127.0.0.1");
    Domain::AppConfigPatch patch;
    patch.shellEnabled = false;
    patch.dashboardHost = "::1";
    patch.mcpRole = Domain::McpRole::Fallback;
    auto updated = Domain::applyConfigPatch(config, patch);
    REQUIRE(updated);
    REQUIRE(updated.value().mcpRole == Domain::McpRole::Fallback);
    patch.dashboardHost = "0.0.0.0";
    REQUIRE(!Domain::applyConfigPatch(config, patch));
    REQUIRE(Domain::wireName(Domain::LogLevel::Warning) == "warn");
}

void agentAndLegacyMemoryParity()
{
    REQUIRE(Domain::ErrorCodes::InvalidKey == "invalid_key");
    REQUIRE(Domain::ErrorCodes::MissingBody == "missing_body");
    REQUIRE(Domain::ErrorCodes::BodyTooLarge == "body_too_large");
    REQUIRE(Domain::ErrorCodes::MissingQuery == "missing_query");
    REQUIRE(Domain::ErrorCodes::EmptyQuery == "empty_query");
    REQUIRE(Domain::ErrorCodes::StoreError == "store_error");
    REQUIRE(Domain::isOpen(Domain::SessionStatus::Open));
    REQUIRE(Domain::isOpen(Domain::SessionStatus::Running));
    REQUIRE(!Domain::isOpen(Domain::SessionStatus::Completed));
    REQUIRE(Domain::wireName(Domain::SessionStatus::Started) == "started");
    REQUIRE(Domain::isSystemMemoryKey("agent_run/session"));
    REQUIRE(Domain::isSystemMemoryKey("continuity/current"));
    REQUIRE(!Domain::isSystemMemoryKey("Agent_run/session"));
    REQUIRE(!Domain::isSystemMemoryKey("user/note"));
    REQUIRE(Domain::isHiddenLegacyMemoryKey("agent_run/session"));
    REQUIRE(Domain::isHiddenLegacyMemoryKey("Agent_run/session"));
    REQUIRE(Domain::isHiddenLegacyMemoryKey("CONTINUITY/current"));
    REQUIRE(!Domain::isHiddenLegacyMemoryKey("user/note"));

    Domain::MemoryNote note{"key", "body", {"alpha"}, fixedTime(1), fixedTime(2)};
    REQUIRE(Domain::validateMemoryNote(note));
    note.key = std::string{"bad\0key", 7};
    const auto controlKey = Domain::validateMemoryNote(note);
    REQUIRE(!controlKey);
    REQUIRE(controlKey.error().code == Domain::ErrorCodes::InvalidKey);
    note.key = std::string{"bad\xc2\x80", 5};
    REQUIRE(!Domain::validateMemoryNote(note));
    note.key = std::string{"bad\xc3", 4};
    REQUIRE(!Domain::validateMemoryNote(note));
    REQUIRE(Domain::normalizeLegacyMemoryKey(
        std::string(Domain::LegacyMemoryLimits::MaximumKeyBytes, 'k')));
    REQUIRE(!Domain::normalizeLegacyMemoryKey(
        std::string(Domain::LegacyMemoryLimits::MaximumKeyBytes + 1U, 'k')));
    const auto paddedOversizedKey = Domain::normalizeLegacyMemoryKey(
        std::string(Domain::LegacyMemoryLimits::MaximumKeyBytes, ' ') + "k");
    REQUIRE(!paddedOversizedKey);
    REQUIRE(paddedOversizedKey.error().code == Domain::ErrorCodes::InvalidKey);
    note.key = "key";
    note.body.assign(Domain::LegacyMemoryLimits::MaximumBodyBytes + 1, 'x');
    const auto oversizedNote = Domain::validateMemoryNote(note);
    REQUIRE(!oversizedNote);
    REQUIRE(oversizedNote.error().code == Domain::ErrorCodes::BodyTooLarge);

    const auto normalizedKey = Domain::normalizeLegacyMemoryKey(
        std::string{"\xc2\xa0"} + " memory/key " + "\xe3\x80\x80");
    REQUIRE(normalizedKey);
    REQUIRE(normalizedKey.value() == "memory/key");
    const auto normalizedBody = Domain::normalizeLegacyMemoryBody("value");
    REQUIRE(normalizedBody);
    REQUIRE(normalizedBody.value() == "value");
    const auto preparedTags = Domain::prepareLegacyMemoryTags(
        {" z ", "", "a", "z", std::string{"\xc2\xa0"} + "b" + "\xc2\xa0"});
    REQUIRE(preparedTags);
    REQUIRE(preparedTags.value() ==
            std::vector<std::string>({"z", "a", "z", "b"}));

    const auto missingBody = Domain::normalizeLegacyMemoryBody(std::nullopt);
    REQUIRE(!missingBody);
    REQUIRE(missingBody.error().code == Domain::ErrorCodes::MissingBody);
    const auto oversizedBody = Domain::normalizeLegacyMemoryBody(
        std::string(Domain::LegacyMemoryLimits::MaximumBodyBytes + 1U, 'x'));
    REQUIRE(!oversizedBody);
    REQUIRE(oversizedBody.error().code == Domain::ErrorCodes::BodyTooLarge);
    const auto malformedBody = Domain::normalizeLegacyMemoryBody(
        std::string{"\xf0\x80\x80\x80", 4});
    REQUIRE(!malformedBody);
    const auto nullBody = Domain::normalizeLegacyMemoryBody(
        std::string{"body\0tail", 9});
    REQUIRE(!nullBody);
    REQUIRE(nullBody.error().code == Domain::ErrorCodes::InvalidRequest);
    REQUIRE(Domain::normalizeLegacyMemoryBody(
        std::string(Domain::LegacyMemoryLimits::MaximumBodyBytes, 'x')));

    std::vector<std::string> tooManyTags;
    for (std::size_t index{};
         index <= Domain::LegacyMemoryLimits::MaximumTagCount;
         ++index) {
        tooManyTags.push_back("tag-" + std::to_string(index));
    }
    const auto excessiveTags = Domain::prepareLegacyMemoryTags(tooManyTags);
    REQUIRE(!excessiveTags);
    REQUIRE(excessiveTags.error().code == Domain::ErrorCodes::LimitExceeded);
    const auto emptyTagFlood = Domain::prepareLegacyMemoryTags(
        std::vector<std::string>(
            Domain::LegacyMemoryLimits::MaximumTagCount + 1U, ""));
    REQUIRE(!emptyTagFlood);
    REQUIRE(emptyTagFlood.error().code == Domain::ErrorCodes::LimitExceeded);
    const auto duplicateTagFlood = Domain::prepareLegacyMemoryTags(
        std::vector<std::string>(
            Domain::LegacyMemoryLimits::MaximumTagCount + 1U, "duplicate"));
    REQUIRE(!duplicateTagFlood);
    REQUIRE(duplicateTagFlood.error().code == Domain::ErrorCodes::LimitExceeded);
    tooManyTags.resize(Domain::LegacyMemoryLimits::MaximumTagCount);
    REQUIRE(Domain::prepareLegacyMemoryTags(tooManyTags));
    const auto oversizedTag = Domain::prepareLegacyMemoryTags(
        {std::string(Domain::LegacyMemoryLimits::MaximumTagBytes + 1U, 't')});
    REQUIRE(!oversizedTag);
    REQUIRE(oversizedTag.error().code == Domain::ErrorCodes::PayloadTooLarge);
    const auto paddedOversizedTag = Domain::prepareLegacyMemoryTags(
        {std::string(Domain::LegacyMemoryLimits::MaximumTagBytes, ' ') + "t"});
    REQUIRE(!paddedOversizedTag);
    REQUIRE(paddedOversizedTag.error().code == Domain::ErrorCodes::PayloadTooLarge);
    const auto nullTag = Domain::prepareLegacyMemoryTags(
        {std::string{"tag\0tail", 8}});
    REQUIRE(!nullTag);
    REQUIRE(nullTag.error().code == Domain::ErrorCodes::InvalidRequest);

    const auto listLow = Domain::normalizeLegacyMemoryListRequest(
        Domain::LegacyMemoryListRequest{"", "  ", false, false, -1});
    REQUIRE(listLow);
    REQUIRE(listLow.value().prefix && listLow.value().prefix->empty());
    REQUIRE(listLow.value().tag && *listLow.value().tag == "  ");
    REQUIRE(listLow.value().limit == 1U);
    const auto listHigh = Domain::normalizeLegacyMemoryListRequest(
        Domain::LegacyMemoryListRequest{
            std::nullopt,
            std::nullopt,
            false,
            false,
            static_cast<std::int64_t>(
                Domain::LegacyMemoryLimits::MaximumQueryLimit + 1U)});
    REQUIRE(listHigh);
    REQUIRE(listHigh.value().limit ==
            Domain::LegacyMemoryLimits::MaximumQueryLimit);
    const auto malformedFilter = Domain::normalizeLegacyMemoryListRequest(
        Domain::LegacyMemoryListRequest{
            std::string{"\xc3", 1}, std::nullopt, false, false, 50});
    REQUIRE(!malformedFilter);
    REQUIRE(Domain::normalizeLegacyMemoryListRequest(
        Domain::LegacyMemoryListRequest{
            std::string(Domain::LegacyMemoryLimits::MaximumFilterBytes, 'p'),
            std::nullopt,
            false,
            false,
            50}));
    REQUIRE(!Domain::normalizeLegacyMemoryListRequest(
        Domain::LegacyMemoryListRequest{
            std::string(Domain::LegacyMemoryLimits::MaximumFilterBytes + 1U, 'p'),
            std::nullopt,
            false,
            false,
            50}));
    const auto paddedOversizedFilter = Domain::normalizeLegacyMemoryListRequest(
        Domain::LegacyMemoryListRequest{
            std::string(Domain::LegacyMemoryLimits::MaximumFilterBytes, ' ') + "p",
            std::nullopt,
            false,
            false,
            50});
    REQUIRE(!paddedOversizedFilter);
    REQUIRE(paddedOversizedFilter.error().code ==
            Domain::ErrorCodes::PayloadTooLarge);
    const auto nullPrefixFilter = Domain::normalizeLegacyMemoryListRequest(
        Domain::LegacyMemoryListRequest{
            std::string{"pre\0fix", 7}, std::nullopt, false, false, 50});
    REQUIRE(!nullPrefixFilter);
    REQUIRE(nullPrefixFilter.error().code == Domain::ErrorCodes::InvalidRequest);
    const auto nullTagFilter = Domain::normalizeLegacyMemoryListRequest(
        Domain::LegacyMemoryListRequest{
            std::nullopt, std::string{"tag\0filter", 10}, false, false, 50});
    REQUIRE(!nullTagFilter);
    REQUIRE(nullTagFilter.error().code == Domain::ErrorCodes::InvalidRequest);

    const auto missingQuery = Domain::normalizeLegacyMemorySearchRequest(
        Domain::LegacyMemorySearchRequest{});
    REQUIRE(!missingQuery);
    REQUIRE(missingQuery.error().code == Domain::ErrorCodes::MissingQuery);
    const auto emptyQuery = Domain::normalizeLegacyMemorySearchRequest(
        Domain::LegacyMemorySearchRequest{" \r\n ", false, true, 50});
    REQUIRE(!emptyQuery);
    REQUIRE(emptyQuery.error().code == Domain::ErrorCodes::EmptyQuery);
    const auto normalizedQuery = Domain::normalizeLegacyMemorySearchRequest(
        Domain::LegacyMemorySearchRequest{"  needle\t", true, false, 0});
    REQUIRE(normalizedQuery);
    REQUIRE(normalizedQuery.value().query == "needle");
    REQUIRE(normalizedQuery.value().limit == 1U);
    const auto oversizedQuery = Domain::normalizeLegacyMemorySearchRequest(
        Domain::LegacyMemorySearchRequest{
            std::string(Domain::LegacyMemoryLimits::MaximumQueryBytes + 1U, 'q'),
            false,
            true,
            50});
    REQUIRE(!oversizedQuery);
    REQUIRE(oversizedQuery.error().code == Domain::ErrorCodes::PayloadTooLarge);
    const auto paddedOversizedQuery = Domain::normalizeLegacyMemorySearchRequest(
        Domain::LegacyMemorySearchRequest{
            std::string(Domain::LegacyMemoryLimits::MaximumQueryBytes, ' ') + "q",
            false,
            true,
            50});
    REQUIRE(!paddedOversizedQuery);
    REQUIRE(paddedOversizedQuery.error().code ==
            Domain::ErrorCodes::PayloadTooLarge);
    const auto nullQuery = Domain::normalizeLegacyMemorySearchRequest(
        Domain::LegacyMemorySearchRequest{
            std::string{"que\0ry", 6}, false, true, 50});
    REQUIRE(!nullQuery);
    REQUIRE(nullQuery.error().code == Domain::ErrorCodes::InvalidRequest);
    REQUIRE(Domain::normalizeLegacyMemorySearchRequest(
        Domain::LegacyMemorySearchRequest{
            std::string(Domain::LegacyMemoryLimits::MaximumQueryBytes, 'q'),
            false,
            true,
            50}));

    const std::string utf8Body{"\xc3\xa9", 2};
    Domain::MemoryNote migratedNote{
        "legacy/key",
        utf8Body,
        {"z", "alpha", "alpha"},
        fixedTime(1),
        fixedTime(2)};
    REQUIRE(Domain::validateMemoryNote(migratedNote));
    migratedNote.body = std::string{"body\0tail", 9};
    REQUIRE(!Domain::validateMemoryNote(migratedNote));
    migratedNote.body = utf8Body;
    migratedNote.tags = {std::string{"tag\0tail", 8}};
    REQUIRE(!Domain::validateMemoryNote(migratedNote));
    Domain::LegacyMemoryNoteProjection projected{
        "key",
        std::nullopt,
        utf8Body.size(),
        {"z", "alpha", "alpha"},
        fixedTime(1),
        fixedTime(2)};
    REQUIRE(Domain::validateLegacyMemoryProjection(projected, false));
    projected.body = utf8Body;
    REQUIRE(Domain::validateLegacyMemoryProjection(projected, true));
    ++projected.bodyUtf8Bytes;
    REQUIRE(!Domain::validateLegacyMemoryProjection(projected, true));
}

void forgeStatusProjectionBoundaries()
{
    Domain::ForgeStatusProjection empty;
    REQUIRE(Domain::validateForgeStatusProjection(empty));

    Domain::ForgeStatusProjection bounded;
    bounded.presenceCount = 7U;
    bounded.openSessionIds.reserve(
        Domain::ForgeStatusLimits::MaximumOpenSessionIds);
    for (std::size_t index{};
         index < Domain::ForgeStatusLimits::MaximumOpenSessionIds;
         ++index) {
        const auto suffix = std::to_string(index);
        bounded.openSessionIds.push_back(parsed<Domain::SessionId>(
            "00000000-0000-4000-8000-" +
            std::string(12U - suffix.size(), '0') + suffix));
    }
    REQUIRE(Domain::validateForgeStatusProjection(bounded));

    Domain::ForgeStatusProjection duplicate{
        1U,
        {bounded.openSessionIds.front(), bounded.openSessionIds.front()}};
    const auto duplicateResult =
        Domain::validateForgeStatusProjection(duplicate);
    REQUIRE(!duplicateResult);
    REQUIRE(duplicateResult.error().code ==
            Domain::ErrorCodes::IntegrityFailure);

    bounded.openSessionIds.push_back(parsed<Domain::SessionId>(
        "ffffffff-ffff-4fff-8fff-ffffffffffff"));
    const auto oversizedResult =
        Domain::validateForgeStatusProjection(bounded);
    REQUIRE(!oversizedResult);
    REQUIRE(oversizedResult.error().code == Domain::ErrorCodes::LimitExceeded);
}

void projectMemoryBoundaries()
{
    const auto limits = Domain::projectMemoryLimitsForProfile(
        Domain::ResourceProfile::Constrained8GiB);
    REQUIRE(limits.maximumOpenProjects == 4);
    REQUIRE(limits.maximumBodyBytes == 256 * 1024);

    Domain::ProjectMemoryWrite write;
    write.kind = "  Decision.Record ";
    write.title = "  Typed domain  ";
    write.summary = "  Preserve invariants  ";
    write.tags = {" Beta ", "alpha", "ALPHA", "   "};
    write.relatedIds = {
        parsed<Domain::MemoryRecordId>("77777777-7777-4777-8777-777777777777")};
    auto validated = Domain::validateProjectMemoryWrite(write, limits);
    REQUIRE(validated);
    REQUIRE(validated.value().kind == "decision.record");
    REQUIRE(validated.value().title == "Typed domain");
    REQUIRE(validated.value().tags == std::vector<std::string>({"alpha", "beta"}));

    write.importance = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!Domain::validateProjectMemoryWrite(write, limits));
    write.importance = 0.5;
    write.body = std::string(limits.maximumBodyBytes + 1, 'x');
    REQUIRE(!Domain::validateProjectMemoryWrite(write, limits));

    Domain::ProjectMemoryWrite batchItem;
    batchItem.kind = "fact";
    batchItem.title = "Title";
    batchItem.summary = "Summary";
    REQUIRE(Domain::validateProjectMemoryBatch({batchItem}, limits));
    REQUIRE(!Domain::validateProjectMemoryBatch({}, limits));
    REQUIRE(!Domain::validateProjectMemoryBatch(
        std::vector<Domain::ProjectMemoryWrite>(limits.maximumBatchCount + 1, batchItem), limits));
    REQUIRE(!Domain::validateProjectMemoryQuery("   ", limits));
    REQUIRE(Domain::validateProjectMemoryQuery("typed query", limits));
    REQUIRE(!Domain::validateProjectMemoryDeadline(std::chrono::milliseconds{0}));
    REQUIRE(Domain::validateProjectMemoryDeadline(std::chrono::milliseconds{60'000}));
    REQUIRE(Domain::normalizeProjectMemoryPageLimit(0, limits) == 1);
    REQUIRE(Domain::normalizeProjectMemoryPageLimit(1'000, limits) == 100);
    REQUIRE(Domain::normalizeProjectMemoryResponseLimit(1, limits) == 1024);

    const Domain::DestructiveConfirmation confirmation{"reset", "project:1", "token"};
    REQUIRE(Domain::validateDestructiveConfirmation(
        confirmation, "reset", "project:1", "token"));
    REQUIRE(!Domain::validateDestructiveConfirmation(
        confirmation, "reset", "project:2", "token"));
}

void projectMemoryRequestBoundaries()
{
    const auto limits = Domain::projectMemoryLimitsForProfile(
        Domain::ResourceProfile::Constrained8GiB);
    REQUIRE(limits.maximumTitleBytes == 512U);
    REQUIRE(limits.maximumSummaryBytes == 4U * 1024U);
    REQUIRE(limits.maximumBodyBytes == 256U * 1024U);
    REQUIRE(limits.maximumSourceReferenceBytes == 2U * 1024U);
    REQUIRE(limits.maximumTagCount == 32U);
    REQUIRE(limits.maximumTagBytes == 128U);
    REQUIRE(limits.maximumRelatedIdCount == 32U);
    REQUIRE(limits.maximumBatchCount == 50U);
    REQUIRE(limits.maximumBatchBytes == 1024U * 1024U);
    REQUIRE(limits.maximumQueryBytes == 4U * 1024U);
    REQUIRE(limits.defaultPageCount == 20U);
    REQUIRE(limits.maximumPageCount == 100U);
    REQUIRE(limits.defaultResponseBytes == 64U * 1024U);
    REQUIRE(limits.maximumResponseBytes == 256U * 1024U);
    REQUIRE(limits.maximumArtifactBytes == 32U * 1024U * 1024U);
    REQUIRE(limits.maximumOpenProjects == 4U);
    REQUIRE(Domain::projectMemoryLimitsForProfile(
                Domain::ResourceProfile::Standard16GiB).maximumOpenProjects == 8U);
    REQUIRE(Domain::projectMemoryLimitsForProfile(
                Domain::ResourceProfile::Expanded32GiBPlus).maximumOpenProjects == 16U);
    const auto recordId =
        parsed<Domain::MemoryRecordId>("77777777-7777-4777-8777-777777777777");

    Domain::ProjectMemoryWrite write;
    write.kind = "fact";
    write.title.assign(limits.maximumTitleBytes, 't');
    write.summary = "summary";
    REQUIRE(Domain::validateProjectMemoryWrite(write, limits));
    write.title.push_back('t');
    REQUIRE(!Domain::validateProjectMemoryWrite(write, limits));

    write.title = "title";
    write.summary.assign(limits.maximumSummaryBytes, 's');
    REQUIRE(Domain::validateProjectMemoryWrite(write, limits));
    write.summary.push_back('s');
    REQUIRE(!Domain::validateProjectMemoryWrite(write, limits));

    write.summary = "summary";
    write.body = std::string(limits.maximumBodyBytes, 'b');
    REQUIRE(Domain::validateProjectMemoryWrite(write, limits));
    write.body->push_back('b');
    REQUIRE(!Domain::validateProjectMemoryWrite(write, limits));

    write.body.reset();
    write.sourceReference = std::string(limits.maximumSourceReferenceBytes, 'r');
    REQUIRE(Domain::validateProjectMemoryWrite(write, limits));
    write.sourceReference->push_back('r');
    REQUIRE(!Domain::validateProjectMemoryWrite(write, limits));

    write.sourceReference.reset();
    write.tags.assign(limits.maximumTagCount, "tag");
    REQUIRE(Domain::validateProjectMemoryWrite(write, limits));
    write.tags.push_back("tag");
    REQUIRE(!Domain::validateProjectMemoryWrite(write, limits));
    write.tags = {std::string(limits.maximumTagBytes, 'a')};
    REQUIRE(Domain::validateProjectMemoryWrite(write, limits));
    write.tags.front().push_back('a');
    REQUIRE(!Domain::validateProjectMemoryWrite(write, limits));

    write.tags.clear();
    write.relatedIds.assign(limits.maximumRelatedIdCount + 1U, recordId);
    const auto related = Domain::validateProjectMemoryWrite(write, limits);
    REQUIRE(related);
    REQUIRE(related.value().relatedIds.size() == limits.maximumRelatedIdCount);

    Domain::ProjectMemoryWrite batchItem;
    batchItem.kind = "fact";
    batchItem.title = "T";
    batchItem.summary = "S";
    batchItem.body = std::string((limits.maximumBatchBytes / 4U) - 2U, 'b');
    std::vector<Domain::ProjectMemoryWrite> exactBatch(4, batchItem);
    REQUIRE(Domain::validateProjectMemoryBatch(exactBatch, limits));
    exactBatch.front().body->push_back('b');
    REQUIRE(!Domain::validateProjectMemoryBatch(exactBatch, limits));

    batchItem.body.reset();
    REQUIRE(Domain::validateProjectMemoryBatch(
        std::vector<Domain::ProjectMemoryWrite>(limits.maximumBatchCount, batchItem),
        limits));
    REQUIRE(!Domain::validateProjectMemoryBatch(
        std::vector<Domain::ProjectMemoryWrite>(limits.maximumBatchCount + 1U, batchItem),
        limits));

    REQUIRE(Domain::validateProjectMemoryQuery(
        std::string(limits.maximumQueryBytes, 'q'), limits));
    REQUIRE(!Domain::validateProjectMemoryQuery(
        std::string(limits.maximumQueryBytes + 1U, 'q'), limits));
    REQUIRE(Domain::validateProjectMemoryDeadline(
        Domain::MinimumProjectMemoryDeadline));
    REQUIRE(!Domain::validateProjectMemoryDeadline(std::chrono::milliseconds{0}));
    REQUIRE(Domain::validateProjectMemoryDeadline(
        Domain::MaximumProjectMemoryDeadline));
    REQUIRE(!Domain::validateProjectMemoryDeadline(
        Domain::MaximumProjectMemoryDeadline + std::chrono::milliseconds{1}));

    Domain::SearchProjectMemoryRequest search{
        projectId(),
        std::string(limits.maximumQueryBytes, 'q'),
        {std::string(64, 'k')},
        std::vector<std::string>(limits.maximumTagCount,
                                 std::string(limits.maximumTagBytes, 't')),
        std::nullopt,
        limits.maximumPageCount + 1U,
        std::string{"djE6MTAw"},
        true,
        limits.maximumResponseBytes + 1U};
    auto normalizedSearch =
        Domain::validateSearchProjectMemoryRequest(search, limits);
    REQUIRE(normalizedSearch);
    REQUIRE(normalizedSearch.value().limit == limits.maximumPageCount);
    REQUIRE(normalizedSearch.value().maximumResponseBytes ==
            limits.maximumResponseBytes);
    REQUIRE(normalizedSearch.value().kinds.front().size() == 64U);
    REQUIRE(normalizedSearch.value().tags.front().size() ==
            limits.maximumTagBytes);

    search.query.push_back('q');
    REQUIRE(!Domain::validateSearchProjectMemoryRequest(search, limits));
    search.query = "query";
    search.kinds = {std::string(65, 'k')};
    search.tags.clear();
    REQUIRE(!Domain::validateSearchProjectMemoryRequest(search, limits));

    search.kinds.assign(limits.maximumPageCount, "fact");
    REQUIRE(Domain::validateSearchProjectMemoryRequest(search, limits));
    search.kinds.push_back("fact");
    auto tooManySearchKinds = Domain::validateSearchProjectMemoryRequest(search, limits);
    REQUIRE(!tooManySearchKinds);
    REQUIRE(tooManySearchKinds.error().code == Domain::ErrorCodes::PayloadTooLarge);
    search.kinds = {"fact"};
    search.tags.assign(limits.maximumTagCount + 1U, "tag");
    REQUIRE(!Domain::validateSearchProjectMemoryRequest(search, limits));

    search.tags.clear();
    search.cursor = "not-base64";
    REQUIRE(!Domain::validateSearchProjectMemoryRequest(search, limits));
    search.cursor = "djE6MDA=";
    REQUIRE(!Domain::validateSearchProjectMemoryRequest(search, limits));
    if constexpr (sizeof(std::size_t) == 8U) {
        search.cursor = "djE6MTg0NDY3NDQwNzM3MDk1NTE2MTU=";
        REQUIRE(Domain::validateSearchProjectMemoryRequest(search, limits));
        search.cursor = "djE6MTg0NDY3NDQwNzM3MDk1NTE2MTY=";
        REQUIRE(!Domain::validateSearchProjectMemoryRequest(search, limits));
    }

    Domain::GetProjectMemoryRequest get{
        projectId(),
        std::vector<Domain::MemoryRecordId>(limits.maximumPageCount, recordId),
        true};
    REQUIRE(Domain::validateGetProjectMemoryRequest(get, limits));
    get.maximumResponseBytes = 1023U;
    REQUIRE(!Domain::validateGetProjectMemoryRequest(get, limits));
    get.maximumResponseBytes = limits.maximumResponseBytes + 1U;
    REQUIRE(!Domain::validateGetProjectMemoryRequest(get, limits));
    get.maximumResponseBytes = limits.maximumResponseBytes;
    REQUIRE(Domain::validateGetProjectMemoryRequest(get, limits));
    get.ids.push_back(recordId);
    REQUIRE(!Domain::validateGetProjectMemoryRequest(get, limits));
    get.ids.clear();
    REQUIRE(!Domain::validateGetProjectMemoryRequest(get, limits));

    Domain::UpdateProjectMemoryRequest update{
        projectId(),
        recordId,
        1,
        std::string(limits.maximumTitleBytes, 't'),
        std::nullopt,
        std::nullopt,
        std::nullopt};
    REQUIRE(Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.title->push_back('t');
    REQUIRE(!Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.title.reset();
    REQUIRE(Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.expectedVersion = 0;
    update.summary = "summary";
    REQUIRE(!Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.expectedVersion = 1;
    update.summary.reset();
    update.body = std::string(limits.maximumBodyBytes, 'b');
    REQUIRE(Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.body->push_back('b');
    REQUIRE(!Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.body.reset();
    update.summary = std::string(limits.maximumSummaryBytes, 's');
    REQUIRE(Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.summary->push_back('s');
    REQUIRE(!Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.summary.reset();
    update.tags = std::vector<std::string>(limits.maximumTagCount, "tag");
    REQUIRE(Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.tags->push_back("tag");
    REQUIRE(!Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.tags = std::vector<std::string>{
        std::string(limits.maximumTagBytes, 't')};
    REQUIRE(Domain::validateUpdateProjectMemoryRequest(update, limits));
    update.tags->front().push_back('t');
    REQUIRE(!Domain::validateUpdateProjectMemoryRequest(update, limits));

    Domain::ListRecentProjectMemoryRequest recent{
        projectId(),
        {" Fact "},
        std::nullopt,
        limits.maximumPageCount + 1U,
        std::string{"djE6MA=="},
        false,
        limits.maximumResponseBytes + 1U};
    auto normalizedRecent =
        Domain::validateListRecentProjectMemoryRequest(recent, limits);
    REQUIRE(normalizedRecent);
    REQUIRE(normalizedRecent.value().kinds.front() == "fact");
    REQUIRE(normalizedRecent.value().limit == limits.maximumPageCount);
    REQUIRE(normalizedRecent.value().maximumResponseBytes ==
            limits.maximumResponseBytes);
    recent.kinds.assign(limits.maximumPageCount, "fact");
    REQUIRE(Domain::validateListRecentProjectMemoryRequest(recent, limits));
    recent.kinds.push_back("fact");
    auto tooManyRecentKinds = Domain::validateListRecentProjectMemoryRequest(recent, limits);
    REQUIRE(!tooManyRecentKinds);
    REQUIRE(tooManyRecentKinds.error().code == Domain::ErrorCodes::PayloadTooLarge);
    recent.kinds = {"fact"};
    recent.cursor = "invalid";
    REQUIRE(!Domain::validateListRecentProjectMemoryRequest(recent, limits));

    Domain::LinkProjectMemoryRequest link{
        projectId(), recordId, recordId, std::string(128, 'r')};
    REQUIRE(Domain::validateLinkProjectMemoryRequest(link));
    link.relation.push_back('r');
    REQUIRE(!Domain::validateLinkProjectMemoryRequest(link));
    link.relation = "   ";
    REQUIRE(!Domain::validateLinkProjectMemoryRequest(link));

    const Domain::ExportProjectMemoryRequest exportRequest{projectId()};
    REQUIRE(Domain::validateExportProjectMemoryRequest(
        exportRequest, limits.maximumArtifactBytes, limits));
    REQUIRE(!Domain::validateExportProjectMemoryRequest(
        exportRequest, limits.maximumArtifactBytes + 1U, limits));

    const Domain::ImportProjectMemoryRequest importRequest{
        projectId(),
        take(Domain::PathText::create("C:\\workspace\\artifact.json")),
        true,
        false};
    REQUIRE(Domain::validateImportProjectMemoryRequest(
        importRequest, limits.maximumArtifactBytes, limits));
    REQUIRE(!Domain::validateImportProjectMemoryRequest(
        importRequest, limits.maximumArtifactBytes + 1U, limits));
    REQUIRE(Domain::validateProjectMemoryStatusRequest(
        Domain::ProjectMemoryStatusRequest{projectId()}));
}


void continuityStateAndIntegrity()
{
    using State = Domain::ContinuityState;
    const std::vector<State> mainPath{
        State::Idle,
        State::CheckpointPreparing,
        State::CheckpointPersisted,
        State::SuccessorCreating,
        State::SuccessorCreated,
        State::BootstrapSending,
        State::Acknowledged,
        State::PredecessorSealing,
        State::Completed};
    for (std::size_t index = 1; index < mainPath.size(); ++index) {
        REQUIRE(Domain::isAllowedContinuityTransition(mainPath[index - 1], mainPath[index]));
    }
    REQUIRE(!Domain::isAllowedContinuityTransition(State::Idle, State::SuccessorCreated));
    REQUIRE(Domain::isAllowedContinuityTransition(State::BootstrapSending, State::FailedRecoverable));
    REQUIRE(Domain::isAllowedContinuityTransition(State::FailedRecoverable, State::RetryWait));
    REQUIRE(Domain::isAllowedContinuityTransition(State::RetryWait, State::BootstrapSending));
    REQUIRE(Domain::isAllowedContinuityTransition(State::BootstrapSending, State::Cancelling));
    REQUIRE(Domain::isAllowedContinuityTransition(State::Cancelling, State::Cancelled));
    REQUIRE(Domain::isTerminal(State::Completed));
    REQUIRE(Domain::canonicalState(Domain::LegacyContinuityState::SuccessorRequested) ==
            State::SuccessorCreating);
    REQUIRE(Domain::canonicalState(Domain::LegacyContinuityState::PredecessorSealed) ==
            State::Completed);

    auto budget = Domain::evaluateContextBudget(
        100, 75, 10, Domain::ContextBudgetSource::ProviderUsage, 1.0);
    REQUIRE(budget);
    REQUIRE(budget.value().action == Domain::ContextBudgetAction::Checkpoint);
    budget = Domain::evaluateContextBudget(
        100, 85, 10, Domain::ContextBudgetSource::ProviderUsage, 1.0);
    REQUIRE(budget.value().action == Domain::ContextBudgetAction::Rollover);
    budget = Domain::evaluateContextBudget(
        100, 100, 10, Domain::ContextBudgetSource::ProviderOverflow, 1.0);
    REQUIRE(budget.value().action == Domain::ContextBudgetAction::Emergency);
    REQUIRE(!Domain::evaluateContextBudget(
        100, 1, 100, Domain::ContextBudgetSource::ProviderUsage, 1.0));
    REQUIRE(take(Domain::estimateContextBudget(1000, 350, 100)).used == 100);

    auto handoff = validHandoff();
    REQUIRE(Domain::validateContinuityHandoff(
        handoff, Domain::MaximumContinuityHandoffEncodedBytes));
    REQUIRE(!Domain::validateContinuityHandoff(
        handoff, Domain::MaximumContinuityHandoffEncodedBytes + 1));
    handoff.constraints.assign(Domain::MaximumContinuityHandoffListItems + 1, "constraint");
    REQUIRE(!Domain::validateContinuityHandoff(handoff, 1024));
    handoff = validHandoff();

    auto operation = validOperation();
    Domain::HandoffAcknowledgement acknowledgement{
        continuityHandoffId(), successorSessionId(), adapterId(), digest('a')};
    REQUIRE(Domain::validateHandoffAcknowledgement(operation, handoff, acknowledgement));
    acknowledgement.canonicalHandoffSha256 = digest('c');
    REQUIRE(!Domain::validateHandoffAcknowledgement(operation, handoff, acknowledgement));
}

void continuityBindingAndIsolation()
{
    const auto idempotencyKey =
        take(Domain::IdempotencyKey::create("rollover:project:1"));
    const Domain::SessionCreationRequest creationRequest{
        continuityOperationId(),
        projectId(),
        predecessorSessionId(),
        idempotencyKey};
    const Domain::HostSession session{
        successorSessionId(),
        projectId(),
        continuityOperationId(),
        predecessorSessionId(),
        idempotencyKey,
        std::nullopt,
        "model",
        Domain::HostSessionStatus::Ready};

    REQUIRE(Domain::validateHostSessionBinding(session, creationRequest));

    auto mismatchedSession = session;
    mismatchedSession.projectId =
        parsed<Domain::ProjectId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    REQUIRE(!Domain::validateHostSessionBinding(mismatchedSession, creationRequest));
    mismatchedSession = session;
    mismatchedSession.operationId =
        parsed<Domain::ContinuityOperationId>("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    REQUIRE(!Domain::validateHostSessionBinding(mismatchedSession, creationRequest));
    mismatchedSession = session;
    mismatchedSession.predecessorSessionId =
        parsed<Domain::SessionId>("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    REQUIRE(!Domain::validateHostSessionBinding(mismatchedSession, creationRequest));
    mismatchedSession = session;
    mismatchedSession.idempotencyKey =
        take(Domain::IdempotencyKey::create("rollover:project:2"));
    REQUIRE(!Domain::validateHostSessionBinding(mismatchedSession, creationRequest));

    const auto handoff = validHandoff();
    REQUIRE(Domain::validateBootstrapCompatibility(session, handoff));
    mismatchedSession = session;
    mismatchedSession.projectId =
        parsed<Domain::ProjectId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    REQUIRE(!Domain::validateBootstrapCompatibility(mismatchedSession, handoff));
    mismatchedSession = session;
    mismatchedSession.operationId =
        parsed<Domain::ContinuityOperationId>("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    REQUIRE(!Domain::validateBootstrapCompatibility(mismatchedSession, handoff));
    mismatchedSession = session;
    mismatchedSession.predecessorSessionId =
        parsed<Domain::SessionId>("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    REQUIRE(!Domain::validateBootstrapCompatibility(mismatchedSession, handoff));
    mismatchedSession = session;
    mismatchedSession.id =
        parsed<Domain::SessionId>("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    REQUIRE(!Domain::validateBootstrapCompatibility(mismatchedSession, handoff));
    auto handoffWithoutSuccessor = handoff;
    handoffWithoutSuccessor.successorSession.reset();
    REQUIRE(!Domain::validateBootstrapCompatibility(session, handoffWithoutSuccessor));

    const Domain::HandoffAcknowledgement acknowledgement{
        continuityHandoffId(), successorSessionId(), adapterId(), digest('a')};
    REQUIRE(Domain::validateHandoffAcknowledgement(
        validOperation(), handoff, acknowledgement));

    auto mismatchedOperation = validOperation();
    mismatchedOperation.projectId =
        parsed<Domain::ProjectId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    REQUIRE(!Domain::validateHandoffAcknowledgement(
        mismatchedOperation, handoff, acknowledgement));
    mismatchedOperation = validOperation();
    mismatchedOperation.operationId =
        parsed<Domain::ContinuityOperationId>("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    REQUIRE(!Domain::validateHandoffAcknowledgement(
        mismatchedOperation, handoff, acknowledgement));
    mismatchedOperation = validOperation();
    mismatchedOperation.handoffId =
        parsed<Domain::ContinuityHandoffId>("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    REQUIRE(!Domain::validateHandoffAcknowledgement(
        mismatchedOperation, handoff, acknowledgement));
    mismatchedOperation = validOperation();
    mismatchedOperation.predecessorSessionId =
        parsed<Domain::SessionId>("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    REQUIRE(!Domain::validateHandoffAcknowledgement(
        mismatchedOperation, handoff, acknowledgement));

    auto mismatchedHandoff = handoff;
    mismatchedHandoff.successorSession->sessionId =
        parsed<Domain::SessionId>("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    REQUIRE(!Domain::validateHandoffAcknowledgement(
        validOperation(), mismatchedHandoff, acknowledgement));
    auto mismatchedAcknowledgement = acknowledgement;
    mismatchedAcknowledgement.successorSessionId =
        parsed<Domain::SessionId>("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    REQUIRE(!Domain::validateHandoffAcknowledgement(
        validOperation(), handoff, mismatchedAcknowledgement));
    mismatchedAcknowledgement = acknowledgement;
    mismatchedAcknowledgement.handoffId =
        parsed<Domain::ContinuityHandoffId>("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    REQUIRE(!Domain::validateHandoffAcknowledgement(
        validOperation(), handoff, mismatchedAcknowledgement));
    mismatchedAcknowledgement = acknowledgement;
    mismatchedAcknowledgement.adapterId =
        parsed<Domain::AdapterId>("another-session-host");
    REQUIRE(!Domain::validateHandoffAcknowledgement(
        validOperation(), handoff, mismatchedAcknowledgement));
    mismatchedAcknowledgement = acknowledgement;
    mismatchedAcknowledgement.canonicalHandoffSha256 = digest('c');
    REQUIRE(!Domain::validateHandoffAcknowledgement(
        validOperation(), handoff, mismatchedAcknowledgement));
}

void processToolTelemetryAndManagerBounds()
{
    static_assert(Domain::MaximumProcessArgumentCount == 256U);
    static_assert(Domain::MaximumProcessArgumentBytes == 4U * 1024U);
    static_assert(Domain::MaximumProcessArgumentsBytes == 15U * 1024U);
    static_assert(Domain::MaximumProcessEnvironmentVariableCount == 128U);
    static_assert(Domain::MaximumProcessEnvironmentNameBytes == 128U);
    static_assert(Domain::MaximumProcessEnvironmentValueBytes == 4U * 1024U);
    static_assert(Domain::MaximumProcessEnvironmentBytes == 24U * 1024U);

    const auto budgets = Domain::budgetsForProfile(Domain::ResourceProfile::Constrained8GiB);
    Domain::ProcessRequest process{take(Domain::PathText::create("tool.exe"))};
    REQUIRE(Domain::validateProcessRequest(process, budgets));
    process.timeout = std::chrono::seconds{121};
    REQUIRE(!Domain::validateProcessRequest(process, budgets));
    process.timeout = std::chrono::seconds{1};
    process.maximumStdoutBytes = budgets.toolStdoutBytesMaximum + 1;
    REQUIRE(!Domain::validateProcessRequest(process, budgets));
    process.maximumStdoutBytes = budgets.toolStdoutBytesMaximum;
    process.environment = {Domain::EnvironmentVariable{"BAD=NAME", "value"}};
    REQUIRE(!Domain::validateProcessRequest(process, budgets));
    process.environment.clear();

    process.arguments.assign(Domain::MaximumProcessArgumentCount, "");
    REQUIRE(Domain::validateProcessRequest(process, budgets));
    process.arguments.push_back("");
    const auto tooManyArguments = Domain::validateProcessRequest(process, budgets);
    REQUIRE(!tooManyArguments);
    REQUIRE(tooManyArguments.error().code == Domain::ErrorCodes::LimitExceeded);

    process.arguments = {std::string(Domain::MaximumProcessArgumentBytes, 'a')};
    REQUIRE(Domain::validateProcessRequest(process, budgets));
    process.arguments.front().push_back('a');
    const auto oversizedArgument = Domain::validateProcessRequest(process, budgets);
    REQUIRE(!oversizedArgument);
    REQUIRE(oversizedArgument.error().code == Domain::ErrorCodes::PayloadTooLarge);

    process.arguments = {
        std::string(Domain::MaximumProcessArgumentBytes, 'a'),
        std::string(Domain::MaximumProcessArgumentBytes, 'b'),
        std::string(Domain::MaximumProcessArgumentBytes, 'c'),
        std::string(
            Domain::MaximumProcessArgumentsBytes -
                (3U * Domain::MaximumProcessArgumentBytes),
            'd')};
    REQUIRE(Domain::validateProcessRequest(process, budgets));
    process.arguments.back().push_back('d');
    const auto oversizedArgumentAggregate =
        Domain::validateProcessRequest(process, budgets);
    REQUIRE(!oversizedArgumentAggregate);
    REQUIRE(oversizedArgumentAggregate.error().code ==
            Domain::ErrorCodes::PayloadTooLarge);
    process.arguments.clear();

    process.environment.assign(
        Domain::MaximumProcessEnvironmentVariableCount,
        Domain::EnvironmentVariable{"A", ""});
    REQUIRE(Domain::validateProcessRequest(process, budgets));
    process.environment.push_back(Domain::EnvironmentVariable{"A", ""});
    const auto tooManyEnvironmentVariables =
        Domain::validateProcessRequest(process, budgets);
    REQUIRE(!tooManyEnvironmentVariables);
    REQUIRE(tooManyEnvironmentVariables.error().code ==
            Domain::ErrorCodes::LimitExceeded);

    process.environment = {
        Domain::EnvironmentVariable{
            std::string(Domain::MaximumProcessEnvironmentNameBytes, 'N'),
            ""}};
    REQUIRE(Domain::validateProcessRequest(process, budgets));
    process.environment.front().name.push_back('N');
    const auto oversizedEnvironmentName =
        Domain::validateProcessRequest(process, budgets);
    REQUIRE(!oversizedEnvironmentName);
    REQUIRE(oversizedEnvironmentName.error().code ==
            Domain::ErrorCodes::PayloadTooLarge);

    process.environment = {
        Domain::EnvironmentVariable{
            "A",
            std::string(Domain::MaximumProcessEnvironmentValueBytes, 'v')}};
    REQUIRE(Domain::validateProcessRequest(process, budgets));
    process.environment.front().value.push_back('v');
    const auto oversizedEnvironmentValue =
        Domain::validateProcessRequest(process, budgets);
    REQUIRE(!oversizedEnvironmentValue);
    REQUIRE(oversizedEnvironmentValue.error().code ==
            Domain::ErrorCodes::PayloadTooLarge);

    constexpr std::size_t aggregateEnvironmentNameBytes = 6U;
    const auto finalEnvironmentValueBytes =
        Domain::MaximumProcessEnvironmentBytes -
        (5U * Domain::MaximumProcessEnvironmentValueBytes) -
        aggregateEnvironmentNameBytes;
    process.environment = {
        {"A", std::string(Domain::MaximumProcessEnvironmentValueBytes, 'v')},
        {"B", std::string(Domain::MaximumProcessEnvironmentValueBytes, 'v')},
        {"C", std::string(Domain::MaximumProcessEnvironmentValueBytes, 'v')},
        {"D", std::string(Domain::MaximumProcessEnvironmentValueBytes, 'v')},
        {"E", std::string(Domain::MaximumProcessEnvironmentValueBytes, 'v')},
        {"F", std::string(finalEnvironmentValueBytes, 'v')}};
    REQUIRE(Domain::validateProcessRequest(process, budgets));
    process.environment.back().value.push_back('v');
    const auto oversizedEnvironmentAggregate =
        Domain::validateProcessRequest(process, budgets);
    REQUIRE(!oversizedEnvironmentAggregate);
    REQUIRE(oversizedEnvironmentAggregate.error().code ==
            Domain::ErrorCodes::PayloadTooLarge);

    Domain::ToolDescriptor descriptor{
        "project_memory.status",
        "Report project memory status.",
        "project_memory",
        Domain::ToolEffect::Read,
        Domain::ToolAvailability::Available,
        true,
        false};
    REQUIRE(Domain::validateToolDescriptor(descriptor));
    descriptor.name = "bad/name";
    REQUIRE(!Domain::validateToolDescriptor(descriptor));
    REQUIRE(Domain::validateMcpFrame(Domain::McpFrame{"{\"jsonrpc\":\"2.0\"}"}, budgets));
    REQUIRE(!Domain::validateMcpFrame(Domain::McpFrame{"{}\n{}"}, budgets));

    Domain::SystemMetrics system{};
    system.cpu.percent = 25.0;
    system.ram.percent = 50.0;
    Domain::ForgeSnapshot forge{fixedTime(1), take(Domain::PathText::create("C:\\forge"))};
    Domain::TelemetrySnapshot snapshot{system, forge, fixedTime(2), {}, "windows-native"};
    REQUIRE(Domain::validateTelemetrySnapshot(snapshot, budgets));
    auto oneHistoryBudget = budgets;
    oneHistoryBudget.historyPointsHardMaximum = 1;
    snapshot.history.resize(2);
    REQUIRE(!Domain::validateTelemetrySnapshot(snapshot, oneHistoryBudget));
    REQUIRE(Domain::toneFor(Domain::TelemetryHealth::Warn) ==
            Domain::TelemetryStatusTone::Caution);
    REQUIRE(Domain::mostSevere({Domain::TelemetryStatusTone::Healthy,
                                Domain::TelemetryStatusTone::Failure}) ==
            Domain::TelemetryStatusTone::Failure);

    Domain::ManagerSettings settings;
    REQUIRE(Domain::validateManagerSettings(settings));
    settings.dashboardHost = "::1";
    REQUIRE(Domain::validateManagerSettings(settings));

    const std::vector<std::string> rejectedDashboardHosts{
        "localhost", "LOCALHOST", "0.0.0.0", "127.0.0.2", "::", "[::1]", "example.test", ""};
    for (const auto& host : rejectedDashboardHosts) {
        settings.dashboardHost = host;
        REQUIRE(!Domain::validateManagerSettings(settings));
    }
    settings.dashboardHost = "127.0.0.1";

    Domain::ManagerSettingsPatch settingsPatch;
    settingsPatch.autoRestart = false;
    settingsPatch.dashboardHost = "::1";
    auto patched = Domain::applyManagerSettingsPatch(settings, settingsPatch);
    REQUIRE(patched);
    REQUIRE(!patched.value().autoRestart);
    settingsPatch.dashboardHost = "0.0.0.0";
    REQUIRE(!Domain::applyManagerSettingsPatch(settings, settingsPatch));
    settingsPatch.dashboardHost = "localhost";
    REQUIRE(!Domain::applyManagerSettingsPatch(settings, settingsPatch));
}

void deploymentAndManagerDefaults()
{
    const auto appConfig = Domain::defaultAppConfig();
    const Domain::ManagerSettings managerSettings;
    const Domain::ManagerStatus managerStatus{
        .home = take(Domain::PathText::create("C:\\forge"))};
    static_assert(Domain::DefaultManagerDashboardPort == 7788U);
    REQUIRE(appConfig.dashboard.port == Domain::DefaultManagerDashboardPort);
    REQUIRE(managerSettings.dashboardPort == Domain::DefaultManagerDashboardPort);
    REQUIRE(managerStatus.dashboardPort == Domain::DefaultManagerDashboardPort);

    auto dashboardConfig = appConfig;
    dashboardConfig.dashboard.host = "127.0.0.1";
    REQUIRE(Domain::validateAppConfig(dashboardConfig));
    dashboardConfig.dashboard.host = "::1";
    REQUIRE(Domain::validateAppConfig(dashboardConfig));
    const std::vector<std::string> rejectedAppConfigDashboardHosts{
        "localhost",
        "LOCALHOST",
        "0.0.0.0",
        "127.0.0.2",
        "::",
        "[::1]",
        "example.test",
        ""};
    for (const auto& host : rejectedAppConfigDashboardHosts) {
        dashboardConfig.dashboard.host = host;
        REQUIRE(!Domain::validateAppConfig(dashboardConfig));
    }

    const Domain::DeploymentRequest install;
    REQUIRE(Domain::validateDeploymentRequest(install, "all-user-data", "token"));

    auto nonDestructive = install;
    nonDestructive.action = Domain::DeploymentAction::Uninstall;
    REQUIRE(Domain::validateDeploymentRequest(
        nonDestructive, "all-user-data", "token"));
    nonDestructive.preserveUserData = false;
    REQUIRE(!Domain::validateDeploymentRequest(
        nonDestructive, "all-user-data", "token"));
    nonDestructive.preserveUserData = true;
    nonDestructive.confirmation =
        Domain::DestructiveConfirmation{"purge", "all-user-data", "token"};
    REQUIRE(!Domain::validateDeploymentRequest(
        nonDestructive, "all-user-data", "token"));

    Domain::DeploymentRequest purge{
        Domain::DeploymentAction::Purge,
        std::nullopt,
        false,
        Domain::DestructiveConfirmation{"purge", "all-user-data", "token"}};
    REQUIRE(Domain::validateDeploymentRequest(purge, "all-user-data", "token"));

    purge.preserveUserData = true;
    REQUIRE(!Domain::validateDeploymentRequest(purge, "all-user-data", "token"));
    purge.preserveUserData = false;
    purge.confirmation.reset();
    REQUIRE(!Domain::validateDeploymentRequest(purge, "all-user-data", "token"));

    purge.confirmation =
        Domain::DestructiveConfirmation{"reset", "all-user-data", "token"};
    REQUIRE(!Domain::validateDeploymentRequest(purge, "all-user-data", "token"));
    purge.confirmation =
        Domain::DestructiveConfirmation{"purge", "one-project", "token"};
    REQUIRE(!Domain::validateDeploymentRequest(purge, "all-user-data", "token"));
    purge.confirmation =
        Domain::DestructiveConfirmation{"purge", "all-user-data", "wrong-token"};
    REQUIRE(!Domain::validateDeploymentRequest(purge, "all-user-data", "token"));
}

void diagnosticsEnvironmentAndGraphics()
{
    static_assert(Domain::MaximumDiagnosticEventBytes == 256U);
    static_assert(Domain::MaximumDiagnosticRoleBytes == 64U);
    static_assert(Domain::MaximumDiagnosticFieldCount == 64U);
    static_assert(Domain::MaximumDiagnosticFieldNameBytes == 128U);
    static_assert(Domain::MaximumDiagnosticFieldValueBytes == 4U * 1024U);

    const auto counts = Domain::summarizeAuditOutcomes(
        {"ok", "error", "denied", "warn", "unexpected"});
    REQUIRE(counts.errorCount == 1);
    REQUIRE(counts.deniedCount == 1);
    REQUIRE(counts.warningCount == 1);
    REQUIRE(counts.otherCount == 1);

    Domain::DiagnosticEnvelope envelope{
        fixedTime(1),
        "domain_test",
        Domain::DiagnosticSeverity::Info,
        "test",
        1,
        Domain::DiagnosticCategory::Diagnostics,
        {{"result", "ok"}}};
    REQUIRE(Domain::validateDiagnosticEnvelope(envelope));

    envelope.event.assign(Domain::MaximumDiagnosticEventBytes, 'e');
    REQUIRE(Domain::validateDiagnosticEnvelope(envelope));
    envelope.event.push_back('e');
    const auto oversizedDiagnosticEvent = Domain::validateDiagnosticEnvelope(envelope);
    REQUIRE(!oversizedDiagnosticEvent);
    REQUIRE(oversizedDiagnosticEvent.error().code == Domain::ErrorCodes::PayloadTooLarge);
    envelope.event = "domain_test";

    envelope.role.assign(Domain::MaximumDiagnosticRoleBytes, 'r');
    REQUIRE(Domain::validateDiagnosticEnvelope(envelope));
    envelope.role.push_back('r');
    const auto oversizedDiagnosticRole = Domain::validateDiagnosticEnvelope(envelope);
    REQUIRE(!oversizedDiagnosticRole);
    REQUIRE(oversizedDiagnosticRole.error().code == Domain::ErrorCodes::PayloadTooLarge);
    envelope.role = "test";

    envelope.fields.assign(
        Domain::MaximumDiagnosticFieldCount,
        Domain::DiagnosticField{"field", "value"});
    REQUIRE(Domain::validateDiagnosticEnvelope(envelope));
    envelope.fields.push_back(Domain::DiagnosticField{"field", "value"});
    const auto tooManyDiagnosticFields = Domain::validateDiagnosticEnvelope(envelope);
    REQUIRE(!tooManyDiagnosticFields);
    REQUIRE(tooManyDiagnosticFields.error().code == Domain::ErrorCodes::LimitExceeded);

    envelope.fields = {
        {std::string(Domain::MaximumDiagnosticFieldNameBytes, 'n'),
         std::string(Domain::MaximumDiagnosticFieldValueBytes, 'v')}};
    REQUIRE(Domain::validateDiagnosticEnvelope(envelope));
    envelope.fields.front().name.push_back('n');
    const auto oversizedDiagnosticFieldName =
        Domain::validateDiagnosticEnvelope(envelope);
    REQUIRE(!oversizedDiagnosticFieldName);
    REQUIRE(oversizedDiagnosticFieldName.error().code ==
            Domain::ErrorCodes::PayloadTooLarge);
    envelope.fields.front().name = "field";
    envelope.fields.front().value.push_back('v');
    const auto oversizedDiagnosticFieldValue =
        Domain::validateDiagnosticEnvelope(envelope);
    REQUIRE(!oversizedDiagnosticFieldValue);
    REQUIRE(oversizedDiagnosticFieldValue.error().code ==
            Domain::ErrorCodes::PayloadTooLarge);

    REQUIRE(Domain::deriveLMStudioConnectionState({
                Domain::LMStudioConnectorHealth{
                    Domain::LMStudioConnectorRole::Primary, true, std::nullopt, 1, "ready"},
                Domain::LMStudioConnectorHealth{
                    Domain::LMStudioConnectorRole::Fallback, false, std::nullopt, 0, "down"}}) ==
            Domain::LMStudioConnectionState::PrimaryOnly);
    REQUIRE(Domain::validateRenderRequest(Domain::RenderRequest{100, 100, 1.0}));
    REQUIRE(!Domain::validateRenderRequest(Domain::RenderRequest{0, 100, 1.0}));
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"result_and_identifier_boundaries", resultAndIdentifierBoundaries},
        {"path_boundaries", pathBoundaries},
        {"resource_and_configuration_boundaries", resourceAndConfigurationBoundaries},
        {"agent_and_legacy_memory_parity", agentAndLegacyMemoryParity},
        {"forge_status_projection_boundaries", forgeStatusProjectionBoundaries},
        {"project_memory_boundaries", projectMemoryBoundaries},
        {"continuity_state_and_integrity", continuityStateAndIntegrity},
        {"process_tool_telemetry_and_manager_bounds", processToolTelemetryAndManagerBounds},
        {"project_memory_request_boundaries", projectMemoryRequestBoundaries},
        {"continuity_binding_and_isolation", continuityBindingAndIsolation},
        {"deployment_and_manager_defaults", deploymentAndManagerDefaults},
        {"diagnostics_environment_and_graphics", diagnosticsEnvironmentAndGraphics}};

    std::size_t failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << "SUMMARY passed=" << (tests.size() - failures)
              << " failed=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
