#include "ForgeConductor/Application/LegacyMemoryService.h"
#include "Fakes/FoundationFakes.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;

static_assert(std::is_final_v<Application::LegacyMemoryService>);
static_assert(!std::is_copy_constructible_v<Application::LegacyMemoryService>);
static_assert(!std::is_move_constructible_v<Application::LegacyMemoryService>);

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} + #condition}; \
        }                                                                        \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

enum class Corruption {
    None,
    SetKey,
    GetKey,
    ListBodyProjection,
    ListVisibleTotal,
    ListSystemKey,
    RemoveFlags,
    RemoveSystemKey,
    SearchQuery,
    PurgeVerification
};

[[nodiscard]] std::shared_ptr<Fakes::UnicodeCanonicalizerFake>
makeUnicodeCanonicalizerFake()
{
    const std::string decomposedEAcute{"e\xCC\x81", 3U};
    const std::string composedEAcute{"\xC3\xA9", 2U};
    return std::make_shared<Fakes::UnicodeCanonicalizerFake>(
        std::vector<Fakes::UnicodeCanonicalizerFake::Mapping>{
            {decomposedEAcute, composedEAcute},
            {composedEAcute, composedEAcute}});
}

class ScriptedLegacyMemoryRepository final
    : public Contracts::ILegacyMemoryRepository {
public:
    explicit ScriptedLegacyMemoryRepository(
        const Contracts::IUnicodeCanonicalizer& unicodeCanonicalizer) noexcept
        : unicodeCanonicalizer_{unicodeCanonicalizer}
    {
    }

    std::optional<Domain::LegacyMemoryUpsert> lastUpsert;
    std::optional<std::string> lastKey;
    std::optional<Domain::LegacyMemoryListQuery> lastListQuery;
    std::optional<Domain::LegacyMemorySearchQuery> lastSearchQuery;
    std::optional<Domain::DestructiveConfirmation> lastConfirmation;
    std::optional<Domain::Error> nextFailure;
    Corruption corruption{Corruption::None};
    std::size_t calls{};
    bool closeCalled{};

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySetOutcome> upsert(
        const Domain::LegacyMemoryUpsert& note,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            ++calls;
            lastUpsert = note;
            if (nextFailure) {
                auto error = std::move(*nextFailure);
                nextFailure.reset();
                return Domain::Result<Domain::LegacyMemorySetOutcome>::failure(
                    std::move(error));
            }
            Domain::MemoryNote stored{
                note.key,
                note.body,
                note.tags,
                Domain::UtcTimePoint{},
                Domain::UtcTimePoint{}};
            const auto existing = find(note.key);
            if (existing == notes_.end()) {
                notes_.push_back(stored);
            } else {
                stored.createdAt = existing->createdAt;
                *existing = stored;
            }
            if (consume(Corruption::SetKey)) {
                stored.key = "wrong/key";
            }
            return Domain::Result<Domain::LegacyMemorySetOutcome>::success(
                Domain::LegacyMemorySetOutcome{std::move(stored), true});
        } catch (...) {
            return internalFailure<Domain::LegacyMemorySetOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryGetOutcome> get(
        const std::string_view key,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            ++calls;
            lastKey = std::string{key};
            if (nextFailure) {
                auto error = std::move(*nextFailure);
                nextFailure.reset();
                return Domain::Result<Domain::LegacyMemoryGetOutcome>::failure(
                    std::move(error));
            }
            const auto match = find(key);
            Domain::LegacyMemoryGetOutcome outcome{
                std::string{key},
                match == notes_.end() ? std::optional<Domain::MemoryNote>{}
                                      : std::optional<Domain::MemoryNote>{*match}};
            if (consume(Corruption::GetKey)) {
                outcome.key = "wrong/key";
            }
            return Domain::Result<Domain::LegacyMemoryGetOutcome>::success(
                std::move(outcome));
        } catch (...) {
            return internalFailure<Domain::LegacyMemoryGetOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryListOutcome> list(
        const Domain::LegacyMemoryListQuery& query,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            ++calls;
            lastListQuery = query;
            if (nextFailure) {
                auto error = std::move(*nextFailure);
                nextFailure.reset();
                return Domain::Result<Domain::LegacyMemoryListOutcome>::failure(
                    std::move(error));
            }

            Domain::LegacyMemoryListOutcome outcome;
            std::size_t rowsBeforeTagFilter{};
            for (const auto& note : notes_) {
                if (!query.includeSystem &&
                    Domain::isHiddenLegacyMemoryKey(note.key)) {
                    continue;
                }
                ++outcome.visibleTotal;
                if (query.prefix && !asciiPrefix(note.key, *query.prefix)) {
                    continue;
                }
                if (rowsBeforeTagFilter >= query.limit) {
                    continue;
                }
                ++rowsBeforeTagFilter;
                if (query.tag &&
                    !containsCanonicalTag(note.tags, *query.tag)) {
                    continue;
                }
                outcome.notes.push_back(project(note, query.includeBody));
            }
            if (consume(Corruption::ListBodyProjection)) {
                ensureProjection(outcome, query.includeBody);
                outcome.notes.front().body = std::string{"unexpected"};
            } else if (consume(Corruption::ListVisibleTotal)) {
                ensureProjection(outcome, query.includeBody);
                outcome.visibleTotal = 0U;
            } else if (consume(Corruption::ListSystemKey)) {
                outcome.notes = {Domain::LegacyMemoryNoteProjection{
                    "Agent_run/latest",
                    query.includeBody ? std::optional<std::string>{"hidden"}
                                      : std::nullopt,
                    6U,
                    {},
                    {},
                    {}}};
                outcome.visibleTotal = 1U;
            }
            return Domain::Result<Domain::LegacyMemoryListOutcome>::success(
                std::move(outcome));
        } catch (...) {
            return internalFailure<Domain::LegacyMemoryListOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(
        const std::string_view key,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            ++calls;
            lastKey = std::string{key};
            if (nextFailure) {
                auto error = std::move(*nextFailure);
                nextFailure.reset();
                return Domain::Result<Domain::LegacyMemoryDeleteOutcome>::failure(
                    std::move(error));
            }
            const auto original = notes_.size();
            notes_.erase(
                std::remove_if(
                    notes_.begin(), notes_.end(), [&](const Domain::MemoryNote& note) {
                        return note.key == key;
                    }),
                notes_.end());
            const bool deleted = original != notes_.size();
            Domain::LegacyMemoryDeleteOutcome outcome{
                std::string{key},
                deleted,
                deleted,
                Domain::isSystemMemoryKey(key)};
            if (consume(Corruption::RemoveFlags)) {
                outcome.existed = !outcome.deleted;
            } else if (consume(Corruption::RemoveSystemKey)) {
                outcome.systemKey = !outcome.systemKey;
            }
            return Domain::Result<Domain::LegacyMemoryDeleteOutcome>::success(
                std::move(outcome));
        } catch (...) {
            return internalFailure<Domain::LegacyMemoryDeleteOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySearchOutcome> search(
        const Domain::LegacyMemorySearchQuery& query,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            ++calls;
            lastSearchQuery = query;
            if (nextFailure) {
                auto error = std::move(*nextFailure);
                nextFailure.reset();
                return Domain::Result<Domain::LegacyMemorySearchOutcome>::failure(
                    std::move(error));
            }

            Domain::LegacyMemorySearchOutcome outcome{query.query, {}};
            for (const auto& note : notes_) {
                if (outcome.notes.size() >= query.limit) {
                    break;
                }
                if (!query.includeSystem &&
                    Domain::isHiddenLegacyMemoryKey(note.key)) {
                    continue;
                }
                bool matches = asciiContains(note.key, query.query) ||
                               asciiContains(note.body, query.query);
                for (const auto& tag : note.tags) {
                    matches = matches || asciiContains(tag, query.query);
                }
                if (matches) {
                    outcome.notes.push_back(project(note, query.includeBody));
                }
            }
            if (consume(Corruption::SearchQuery)) {
                outcome.query = "wrong query";
            }
            return Domain::Result<Domain::LegacyMemorySearchOutcome>::success(
                std::move(outcome));
        } catch (...) {
            return internalFailure<Domain::LegacyMemorySearchOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryPurgeOutcome> purge(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            ++calls;
            lastConfirmation = confirmation;
            if (nextFailure) {
                auto error = std::move(*nextFailure);
                nextFailure.reset();
                return Domain::Result<Domain::LegacyMemoryPurgeOutcome>::failure(
                    std::move(error));
            }
            const auto removed = notes_.size();
            notes_.clear();
            return Domain::Result<Domain::LegacyMemoryPurgeOutcome>::success(
                Domain::LegacyMemoryPurgeOutcome{
                    removed, !consume(Corruption::PurgeVerification)});
        } catch (...) {
            return internalFailure<Domain::LegacyMemoryPurgeOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            ++calls;
            if (nextFailure) {
                auto error = std::move(*nextFailure);
                nextFailure.reset();
                return Domain::Result<void>::failure(std::move(error));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The scripted repository failed internally."));
        }
    }

    void close() noexcept override { closeCalled = true; }

private:
    template <typename T>
    [[nodiscard]] static Domain::Result<T> internalFailure()
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The scripted repository failed internally."));
    }

    [[nodiscard]] bool consume(const Corruption expected) noexcept
    {
        if (corruption != expected) {
            return false;
        }
        corruption = Corruption::None;
        return true;
    }

    [[nodiscard]] static unsigned char foldAscii(unsigned char value) noexcept
    {
        if (value >= static_cast<unsigned char>('A') &&
            value <= static_cast<unsigned char>('Z')) {
            return static_cast<unsigned char>(value + ('a' - 'A'));
        }
        return value;
    }

    [[nodiscard]] static bool asciiPrefix(
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

    [[nodiscard]] static bool asciiContains(
        const std::string_view value,
        const std::string_view query) noexcept
    {
        if (query.empty()) {
            return true;
        }
        if (query.size() > value.size()) {
            return false;
        }
        for (std::size_t start{}; start + query.size() <= value.size(); ++start) {
            if (asciiPrefix(value.substr(start), query)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static Domain::LegacyMemoryNoteProjection project(
        const Domain::MemoryNote& note,
        const bool includeBody)
    {
        return Domain::LegacyMemoryNoteProjection{
            note.key,
            includeBody ? std::optional<std::string>{note.body} : std::nullopt,
            note.body.size(),
            note.tags,
            note.createdAt,
            note.updatedAt};
    }

    [[nodiscard]] bool containsCanonicalTag(
        const std::vector<std::string>& tags,
        const std::string_view candidate) const
    {
        const auto candidateKey = take(unicodeCanonicalizer_.nfcKey(candidate));
        return std::any_of(
            tags.begin(), tags.end(), [&](const std::string& tag) {
                return take(unicodeCanonicalizer_.nfcKey(tag)) == candidateKey;
            });
    }

    static void ensureProjection(
        Domain::LegacyMemoryListOutcome& outcome,
        const bool includeBody)
    {
        if (outcome.notes.empty()) {
            outcome.notes.push_back(Domain::LegacyMemoryNoteProjection{
                "seed/key",
                includeBody ? std::optional<std::string>{"seed"} : std::nullopt,
                4U,
                {},
                {},
                {}});
            outcome.visibleTotal = 1U;
        }
    }

    [[nodiscard]] std::vector<Domain::MemoryNote>::iterator find(
        const std::string_view key)
    {
        return std::find_if(
            notes_.begin(), notes_.end(), [&](const Domain::MemoryNote& note) {
                return note.key == key;
            });
    }

    [[nodiscard]] std::vector<Domain::MemoryNote>::const_iterator find(
        const std::string_view key) const
    {
        return std::find_if(
            notes_.begin(), notes_.end(), [&](const Domain::MemoryNote& note) {
                return note.key == key;
            });
    }

    const Contracts::IUnicodeCanonicalizer& unicodeCanonicalizer_;
    std::vector<Domain::MemoryNote> notes_;
};

struct Fixture final {
    std::shared_ptr<Fakes::UnicodeCanonicalizerFake> unicodeCanonicalizer{
        makeUnicodeCanonicalizerFake()};
    ScriptedLegacyMemoryRepository repository{*unicodeCanonicalizer};
    Application::LegacyMemoryService service{
        repository, unicodeCanonicalizer};

    [[nodiscard]] Domain::OperationContext context() const
    {
        return Domain::OperationContext{
            parse<Domain::OperationId>("11111111-1111-4111-8111-111111111111"),
            std::chrono::steady_clock::now() + 5s,
            std::stop_token{},
            parse<Domain::CorrelationId>("legacy-memory-application-test")};
    }
};

[[nodiscard]] Domain::LegacyMemorySetRequest noteRequest(
    std::string key = "project/alpha",
    std::string body = "Resume package work")
{
    return Domain::LegacyMemorySetRequest{
        std::move(key), std::move(body), {" project ", "alpha", "alpha", ""}};
}

void fiveOperationsNormalizeAndProject()
{
    Fixture fixture;
    const auto set = fixture.service.set(
        noteRequest("  project/alpha\t"), fixture.context());
    REQUIRE(set);
    REQUIRE(set.value().stored);
    REQUIRE(set.value().note.key == "project/alpha");
    REQUIRE(fixture.repository.lastUpsert);
    REQUIRE(fixture.repository.lastUpsert->tags ==
            std::vector<std::string>({"alpha", "project"}));

    const auto get = fixture.service.get(
        Domain::LegacyMemoryGetRequest{" project/alpha "}, fixture.context());
    REQUIRE(get);
    REQUIRE(get.value().key == "project/alpha");
    REQUIRE(get.value().note && get.value().note->body == "Resume package work");

    const auto list = fixture.service.list(
        Domain::LegacyMemoryListRequest{
            "PROJECT/",
            "alpha",
            false,
            false,
            static_cast<std::int64_t>(
                Domain::LegacyMemoryLimits::MaximumQueryLimit + 1U)},
        fixture.context());
    REQUIRE(list);
    REQUIRE(list.value().notes.size() == 1U);
    REQUIRE(!list.value().notes.front().body);
    REQUIRE(list.value().notes.front().bodyUtf8Bytes == 19U);
    REQUIRE(fixture.repository.lastListQuery);
    REQUIRE(fixture.repository.lastListQuery->prefix ==
            std::optional<std::string>{"PROJECT/"});
    REQUIRE(fixture.repository.lastListQuery->limit ==
            Domain::LegacyMemoryLimits::MaximumQueryLimit);

    const auto search = fixture.service.search(
        Domain::LegacyMemorySearchRequest{" package ", false, true, 0},
        fixture.context());
    REQUIRE(search);
    REQUIRE(search.value().query == "package");
    REQUIRE(search.value().notes.size() == 1U);
    REQUIRE(search.value().notes.front().body ==
            std::optional<std::string>{"Resume package work"});
    REQUIRE(fixture.repository.lastSearchQuery);
    REQUIRE(fixture.repository.lastSearchQuery->limit == 1U);

    const std::string decomposedEAcute{"e\xCC\x81", 3U};
    const std::string composedEAcute{"\xC3\xA9", 2U};
    const std::string bmpPrivateUse{"\xEE\x80\x80", 3U};
    const std::string supplementary{"\xF0\x90\x80\x80", 4U};
    const auto unicodeSet = fixture.service.set(
        Domain::LegacyMemorySetRequest{
            "unicode/tags",
            "canonical-equivalence",
            {decomposedEAcute,
             composedEAcute,
             supplementary,
             bmpPrivateUse}},
        fixture.context());
    REQUIRE(unicodeSet);
    REQUIRE(unicodeSet.value().note.tags ==
            std::vector<std::string>(
                {decomposedEAcute, bmpPrivateUse, supplementary}));
    const auto unicodeList = fixture.service.list(
        Domain::LegacyMemoryListRequest{
            "unicode/", composedEAcute, false, false, 50},
        fixture.context());
    REQUIRE(unicodeList);
    REQUIRE(unicodeList.value().notes.size() == 1U);
    REQUIRE(unicodeList.value().notes.front().tags.front() ==
            decomposedEAcute);

    const auto caseVariantSystemSet = fixture.service.set(
        noteRequest("Agent_run/case-variant", "hidden note"), fixture.context());
    REQUIRE(caseVariantSystemSet);
    const auto defaultVisibilityList = fixture.service.list(
        Domain::LegacyMemoryListRequest{}, fixture.context());
    REQUIRE(defaultVisibilityList);
    REQUIRE(defaultVisibilityList.value().notes.size() == 2U);
    REQUIRE(std::none_of(
        defaultVisibilityList.value().notes.begin(),
        defaultVisibilityList.value().notes.end(),
        [](const Domain::LegacyMemoryNoteProjection& note) {
            return note.key == "Agent_run/case-variant";
        }));
    const auto defaultVisibilitySearch = fixture.service.search(
        Domain::LegacyMemorySearchRequest{"Agent_run", false, true, 50},
        fixture.context());
    REQUIRE(defaultVisibilitySearch);
    REQUIRE(defaultVisibilitySearch.value().notes.empty());
    const auto removedCaseVariantSystem = fixture.service.remove(
        Domain::LegacyMemoryRemoveRequest{"Agent_run/case-variant"},
        fixture.context());
    REQUIRE(removedCaseVariantSystem);
    REQUIRE(removedCaseVariantSystem.value().deleted);
    REQUIRE(removedCaseVariantSystem.value().existed);
    REQUIRE(!removedCaseVariantSystem.value().systemKey);

    const auto removed = fixture.service.remove(
        Domain::LegacyMemoryRemoveRequest{" project/alpha "}, fixture.context());
    REQUIRE(removed);
    REQUIRE(removed.value().key == "project/alpha");
    REQUIRE(removed.value().deleted);
    REQUIRE(removed.value().existed);
    REQUIRE(!removed.value().systemKey);
}

void validationAndLimitErrors()
{
    Fixture fixture;
    const auto callsBeforeValidation = fixture.repository.calls;

    const auto invalidKey = fixture.service.set(
        noteRequest(std::string{"bad\xc2\x80", 5}), fixture.context());
    REQUIRE(!invalidKey);
    REQUIRE(invalidKey.error().code == Domain::ErrorCodes::InvalidKey);
    const auto paddedOversizedKey = fixture.service.set(
        noteRequest(
            std::string(Domain::LegacyMemoryLimits::MaximumKeyBytes, ' ') + "k"),
        fixture.context());
    REQUIRE(!paddedOversizedKey);
    REQUIRE(paddedOversizedKey.error().code == Domain::ErrorCodes::InvalidKey);

    auto missingBody = noteRequest();
    missingBody.body.reset();
    const auto missing = fixture.service.set(missingBody, fixture.context());
    REQUIRE(!missing);
    REQUIRE(missing.error().code == Domain::ErrorCodes::MissingBody);

    const auto oversized = fixture.service.set(
        noteRequest(
            "key",
            std::string(Domain::LegacyMemoryLimits::MaximumBodyBytes + 1U, 'x')),
        fixture.context());
    REQUIRE(!oversized);
    REQUIRE(oversized.error().code == Domain::ErrorCodes::BodyTooLarge);
    const auto nullBody = fixture.service.set(
        noteRequest("key", std::string{"body\0tail", 9}), fixture.context());
    REQUIRE(!nullBody);
    REQUIRE(nullBody.error().code == Domain::ErrorCodes::InvalidRequest);

    auto emptyTagFlood = noteRequest();
    emptyTagFlood.tags.assign(
        Domain::LegacyMemoryLimits::MaximumTagCount + 1U, "");
    const auto emptyTags = fixture.service.set(emptyTagFlood, fixture.context());
    REQUIRE(!emptyTags);
    REQUIRE(emptyTags.error().code == Domain::ErrorCodes::LimitExceeded);
    auto duplicateTagFlood = noteRequest();
    duplicateTagFlood.tags.assign(
        Domain::LegacyMemoryLimits::MaximumTagCount + 1U, "duplicate");
    const auto duplicateTags = fixture.service.set(
        duplicateTagFlood, fixture.context());
    REQUIRE(!duplicateTags);
    REQUIRE(duplicateTags.error().code == Domain::ErrorCodes::LimitExceeded);
    auto paddedTag = noteRequest();
    paddedTag.tags = {
        std::string(Domain::LegacyMemoryLimits::MaximumTagBytes, ' ') + "t"};
    const auto oversizedRawTag = fixture.service.set(paddedTag, fixture.context());
    REQUIRE(!oversizedRawTag);
    REQUIRE(oversizedRawTag.error().code == Domain::ErrorCodes::PayloadTooLarge);
    auto nullTag = noteRequest();
    nullTag.tags = {std::string{"tag\0tail", 8}};
    const auto embeddedNullTag = fixture.service.set(nullTag, fixture.context());
    REQUIRE(!embeddedNullTag);
    REQUIRE(embeddedNullTag.error().code == Domain::ErrorCodes::InvalidRequest);

    const auto malformedFilter = fixture.service.list(
        Domain::LegacyMemoryListRequest{
            std::string{"\xc3", 1}, std::nullopt, false, false, 50},
        fixture.context());
    REQUIRE(!malformedFilter);
    REQUIRE(malformedFilter.error().code == Domain::ErrorCodes::InvalidRequest);
    const auto oversizedRawFilter = fixture.service.list(
        Domain::LegacyMemoryListRequest{
            std::string(Domain::LegacyMemoryLimits::MaximumFilterBytes, ' ') + "p",
            std::nullopt,
            false,
            false,
            50},
        fixture.context());
    REQUIRE(!oversizedRawFilter);
    REQUIRE(oversizedRawFilter.error().code ==
            Domain::ErrorCodes::PayloadTooLarge);
    const auto nullPrefix = fixture.service.list(
        Domain::LegacyMemoryListRequest{
            std::string{"pre\0fix", 7}, std::nullopt, false, false, 50},
        fixture.context());
    REQUIRE(!nullPrefix);
    REQUIRE(nullPrefix.error().code == Domain::ErrorCodes::InvalidRequest);
    const auto nullTagFilter = fixture.service.list(
        Domain::LegacyMemoryListRequest{
            std::nullopt, std::string{"tag\0filter", 10}, false, false, 50},
        fixture.context());
    REQUIRE(!nullTagFilter);
    REQUIRE(nullTagFilter.error().code == Domain::ErrorCodes::InvalidRequest);

    const auto missingQuery = fixture.service.search(
        Domain::LegacyMemorySearchRequest{}, fixture.context());
    REQUIRE(!missingQuery);
    REQUIRE(missingQuery.error().code == Domain::ErrorCodes::MissingQuery);
    const auto emptyQuery = fixture.service.search(
        Domain::LegacyMemorySearchRequest{" \r\n ", false, true, 50},
        fixture.context());
    REQUIRE(!emptyQuery);
    REQUIRE(emptyQuery.error().code == Domain::ErrorCodes::EmptyQuery);
    const auto longQuery = fixture.service.search(
        Domain::LegacyMemorySearchRequest{
            std::string(Domain::LegacyMemoryLimits::MaximumQueryBytes + 1U, 'q'),
            false,
            true,
            50},
        fixture.context());
    REQUIRE(!longQuery);
    REQUIRE(longQuery.error().code == Domain::ErrorCodes::PayloadTooLarge);
    const auto paddedLongQuery = fixture.service.search(
        Domain::LegacyMemorySearchRequest{
            std::string(Domain::LegacyMemoryLimits::MaximumQueryBytes, ' ') + "q",
            false,
            true,
            50},
        fixture.context());
    REQUIRE(!paddedLongQuery);
    REQUIRE(paddedLongQuery.error().code == Domain::ErrorCodes::PayloadTooLarge);
    const auto nullQuery = fixture.service.search(
        Domain::LegacyMemorySearchRequest{
            std::string{"que\0ry", 6}, false, true, 50},
        fixture.context());
    REQUIRE(!nullQuery);
    REQUIRE(nullQuery.error().code == Domain::ErrorCodes::InvalidRequest);
    REQUIRE(fixture.repository.calls == callsBeforeValidation);
}

void dependencyFailuresAndPostconditions()
{
    Fixture fixture;
    fixture.repository.nextFailure = Domain::makeError(
        Domain::ErrorCodes::DatabaseBusy, "database busy", true);
    const auto dependencyFailure = fixture.service.set(
        noteRequest(), fixture.context());
    REQUIRE(!dependencyFailure);
    REQUIRE(dependencyFailure.error().code == Domain::ErrorCodes::DatabaseBusy);
    REQUIRE(dependencyFailure.error().retryable);

    fixture.repository.corruption = Corruption::SetKey;
    const auto corruptSet = fixture.service.set(noteRequest(), fixture.context());
    REQUIRE(!corruptSet);
    REQUIRE(corruptSet.error().code == Domain::ErrorCodes::IntegrityFailure);

    REQUIRE(fixture.service.set(noteRequest(), fixture.context()));
    fixture.repository.corruption = Corruption::GetKey;
    const auto corruptGet = fixture.service.get(
        Domain::LegacyMemoryGetRequest{"project/alpha"}, fixture.context());
    REQUIRE(!corruptGet);
    REQUIRE(corruptGet.error().code == Domain::ErrorCodes::IntegrityFailure);

    fixture.repository.corruption = Corruption::ListBodyProjection;
    const auto corruptProjection = fixture.service.list(
        Domain::LegacyMemoryListRequest{}, fixture.context());
    REQUIRE(!corruptProjection);
    REQUIRE(corruptProjection.error().code == Domain::ErrorCodes::IntegrityFailure);

    fixture.repository.corruption = Corruption::ListVisibleTotal;
    const auto corruptTotal = fixture.service.list(
        Domain::LegacyMemoryListRequest{}, fixture.context());
    REQUIRE(!corruptTotal);
    REQUIRE(corruptTotal.error().code == Domain::ErrorCodes::IntegrityFailure);

    fixture.repository.corruption = Corruption::ListSystemKey;
    const auto hiddenSystem = fixture.service.list(
        Domain::LegacyMemoryListRequest{}, fixture.context());
    REQUIRE(!hiddenSystem);
    REQUIRE(hiddenSystem.error().code == Domain::ErrorCodes::IntegrityFailure);

    const auto asciiLikePrefix = fixture.service.list(
        Domain::LegacyMemoryListRequest{"PROJECT/", std::nullopt, false, false, 50},
        fixture.context());
    REQUIRE(asciiLikePrefix);
    REQUIRE(asciiLikePrefix.value().notes.size() == 1U);

    fixture.repository.corruption = Corruption::SearchQuery;
    const auto corruptSearch = fixture.service.search(
        Domain::LegacyMemorySearchRequest{"package"}, fixture.context());
    REQUIRE(!corruptSearch);
    REQUIRE(corruptSearch.error().code == Domain::ErrorCodes::IntegrityFailure);

    fixture.repository.corruption = Corruption::RemoveSystemKey;
    const auto corruptRemove = fixture.service.remove(
        Domain::LegacyMemoryRemoveRequest{"project/alpha"}, fixture.context());
    REQUIRE(!corruptRemove);
    REQUIRE(corruptRemove.error().code == Domain::ErrorCodes::IntegrityFailure);

    fixture.repository.corruption = Corruption::RemoveFlags;
    const auto corruptFlags = fixture.service.remove(
        Domain::LegacyMemoryRemoveRequest{"project/alpha"}, fixture.context());
    REQUIRE(!corruptFlags);
    REQUIRE(corruptFlags.error().code == Domain::ErrorCodes::IntegrityFailure);
}

void cancellationDeadlineAndShutdown()
{
    {
        auto canonicalizer = makeUnicodeCanonicalizerFake();
        std::weak_ptr<Fakes::UnicodeCanonicalizerFake> retained{canonicalizer};
        ScriptedLegacyMemoryRepository repository{*canonicalizer};
        Application::LegacyMemoryService service{repository, canonicalizer};
        canonicalizer.reset();
        REQUIRE(!retained.expired());
        const Domain::OperationContext context{
            parse<Domain::OperationId>(
                "22222222-2222-4222-8222-222222222222"),
            std::chrono::steady_clock::now() + 5s,
            std::stop_token{},
            parse<Domain::CorrelationId>(
                "legacy-memory-owned-canonicalizer-test")};
        REQUIRE(service.set(noteRequest("lifetime/probe"), context));
    }

    {
        auto canonicalizer = makeUnicodeCanonicalizerFake();
        ScriptedLegacyMemoryRepository repository{*canonicalizer};
        bool rejectedNullDependency{};
        try {
            Application::LegacyMemoryService invalid{repository, nullptr};
        } catch (const std::invalid_argument&) {
            rejectedNullDependency = true;
        }
        REQUIRE(rejectedNullDependency);
    }

    Fixture fixture;
    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelledContext = fixture.context();
    cancelledContext.cancellation = cancellation.get_token();
    const auto callsBeforeCancellation = fixture.repository.calls;
    const auto cancelled = fixture.service.get(
        Domain::LegacyMemoryGetRequest{"key"}, cancelledContext);
    REQUIRE(!cancelled);
    REQUIRE(cancelled.error().code == Domain::ErrorCodes::Cancelled);
    REQUIRE(fixture.repository.calls == callsBeforeCancellation);

    auto expiredContext = fixture.context();
    expiredContext.deadline = std::chrono::steady_clock::now() - 1ms;
    const auto expired = fixture.service.list(
        Domain::LegacyMemoryListRequest{}, expiredContext);
    REQUIRE(!expired);
    REQUIRE(expired.error().code == Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(fixture.repository.calls == callsBeforeCancellation);

    REQUIRE(fixture.service.quickCheck(fixture.context()));
    fixture.service.shutdown();
    REQUIRE(fixture.repository.closeCalled);
    const auto afterShutdown = fixture.service.get(
        Domain::LegacyMemoryGetRequest{"key"}, fixture.context());
    REQUIRE(!afterShutdown);
    REQUIRE(afterShutdown.error().code == Domain::ErrorCodes::Cancelled);
}

void ownerPurgeConfirmationForwarding()
{
    Fixture fixture;
    REQUIRE(fixture.service.set(noteRequest(), fixture.context()));
    const auto callsBeforeWrongConfirmation = fixture.repository.calls;
    const Domain::DestructiveConfirmation wrong{
        "purge_legacy_memory", "legacy-global-memory", "wrong token"};
    const auto rejected = fixture.service.purge(wrong, fixture.context());
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == Domain::ErrorCodes::Unauthorized);
    REQUIRE(fixture.repository.calls == callsBeforeWrongConfirmation);

    const Domain::DestructiveConfirmation exact{
        "purge_legacy_memory",
        "legacy-global-memory",
        "PURGE LEGACY GLOBAL MEMORY"};
    const auto purged = fixture.service.purge(exact, fixture.context());
    REQUIRE(purged);
    REQUIRE(purged.value().notesRemoved == 1U);
    REQUIRE(purged.value().verified);
    REQUIRE(fixture.repository.lastConfirmation);
    REQUIRE(fixture.repository.lastConfirmation->action == exact.action);
    REQUIRE(fixture.repository.lastConfirmation->scope == exact.scope);
    REQUIRE(fixture.repository.lastConfirmation->token == exact.token);

    REQUIRE(fixture.service.set(noteRequest(), fixture.context()));
    fixture.repository.corruption = Corruption::PurgeVerification;
    const auto unverified = fixture.service.purge(exact, fixture.context());
    REQUIRE(!unverified);
    REQUIRE(unverified.error().code == Domain::ErrorCodes::IntegrityFailure);
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"five_operations_normalize_and_project", fiveOperationsNormalizeAndProject},
        {"validation_and_limit_errors", validationAndLimitErrors},
        {"dependency_failures_and_postconditions", dependencyFailuresAndPostconditions},
        {"cancellation_deadline_and_shutdown", cancellationDeadlineAndShutdown},
        {"owner_purge_confirmation_forwarding", ownerPurgeConfirmationForwarding}};

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
    return failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
