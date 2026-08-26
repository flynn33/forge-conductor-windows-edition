#include "ForgeConductor/Application/LegacyMemoryService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUnicodeCanonicalizer.h"
#include "ForgeConductor/Persistence/Windows/WindowsLegacyMemoryRepository.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Persistence/PersistenceTestSupport.h"

#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Support = ForgeConductor::Tests::PersistenceSupport;

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
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::UtcTimePoint utcTime(
    const int year,
    const unsigned month,
    const unsigned day,
    const int hour,
    const int minute,
    const int second)
{
    const auto date = std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::month{month} /
        std::chrono::day{day}};
    return Domain::UtcTimePoint{
        date.time_since_epoch() + std::chrono::hours{hour} +
        std::chrono::minutes{minute} + std::chrono::seconds{second}};
}

class MutableClock final : public Contracts::IClock {
public:
    MutableClock(
        const Domain::UtcTimePoint utc,
        const Domain::MonotonicTimePoint monotonic) noexcept
        : utc_{utc}, monotonic_{monotonic}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return utc_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return monotonic_;
    }

    template <typename Rep, typename Period>
    void advance(const std::chrono::duration<Rep, Period> amount) noexcept
    {
        std::lock_guard lock{mutex_};
        utc_ += amount;
        monotonic_ += amount;
    }

private:
    mutable std::mutex mutex_;
    Domain::UtcTimePoint utc_;
    Domain::MonotonicTimePoint monotonic_;
};

[[nodiscard]] Domain::OperationContext activeContext(
    const MutableClock& clock,
    const std::string_view correlation,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("90909090-9090-4090-8090-909090909090"),
        clock.monotonicNow() + std::chrono::minutes{2},
        cancellation,
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] Domain::OperationContext expiredContext(
    const MutableClock& clock,
    const std::string_view correlation)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("91919191-9191-4191-8191-919191919191"),
        clock.monotonicNow(),
        {},
        parse<Domain::CorrelationId>(correlation)};
}

struct RepositoryFixture final {
    explicit RepositoryFixture(
        const std::filesystem::path& directory,
        const Domain::UtcTimePoint initialUtc =
            utcTime(2026, 1U, 2U, 3, 4, 5))
        : directory{directory},
          clock{std::make_shared<MutableClock>(
              initialUtc, std::chrono::steady_clock::now())},
          unicodeCanonicalizer{
              std::make_shared<InfrastructureWindows::WindowsUnicodeCanonicalizer>()}
    {
        paths = std::make_shared<Fakes::RecordingApplicationPathsFake>();
        paths->setNow(clock->monotonicNow());
        paths->dataRootResult.set(Domain::Result<Domain::PathText>::success(
            Support::pathText(directory)));
        diagnostics = std::make_shared<Fakes::RuntimeDiagnosticsFake>(
            clock->monotonicNow());
        open("p09-legacy-repository-open");
    }

    ~RepositoryFixture() noexcept
    {
        if (repository) {
            repository->close();
        }
    }

    void open(const std::string_view correlation)
    {
        REQUIRE(!repository);
        repository = take(PersistenceWindows::WindowsLegacyMemoryRepository::open(
            paths,
            diagnostics,
            clock,
            unicodeCanonicalizer,
            activeContext(*clock, correlation)));
    }

    void close() noexcept
    {
        if (repository) {
            repository->close();
            repository.reset();
        }
    }

    std::filesystem::path directory;
    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<MutableClock> clock;
    std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
        unicodeCanonicalizer;
    std::shared_ptr<PersistenceWindows::WindowsLegacyMemoryRepository> repository;
};

class SqliteDatabase final {
public:
    SqliteDatabase(
        const std::filesystem::path& path,
        const bool create)
    {
        const auto encoded = Support::pathText(path).value();
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
                          SQLITE_OPEN_NOFOLLOW |
                          (create ? SQLITE_OPEN_CREATE : 0);
        sqlite3* opened{};
        const int result = sqlite3_open_v2(
            encoded.c_str(), &opened, flags, nullptr);
        if (result != SQLITE_OK) {
            std::string message = opened != nullptr
                ? sqlite3_errmsg(opened)
                : "Winsqlite returned no database handle";
            if (opened != nullptr) {
                static_cast<void>(sqlite3_close_v2(opened));
            }
            throw std::runtime_error{"Could not open test database: " + message};
        }
        database_ = opened;
        requireSqlite(sqlite3_busy_timeout(database_, 5'000), "set busy timeout");
    }

    ~SqliteDatabase() noexcept
    {
        if (database_ != nullptr) {
            static_cast<void>(sqlite3_close_v2(database_));
        }
    }

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;
    SqliteDatabase(SqliteDatabase&&) = delete;
    SqliteDatabase& operator=(SqliteDatabase&&) = delete;

    void execute(const std::string_view sql)
    {
        const std::string owned{sql};
        char* errorMessage{};
        const int result = sqlite3_exec(
            database_, owned.c_str(), nullptr, nullptr, &errorMessage);
        std::string detail;
        if (errorMessage != nullptr) {
            detail = errorMessage;
            sqlite3_free(errorMessage);
        }
        if (result != SQLITE_OK) {
            throw std::runtime_error{
                "Could not execute test SQL: " + detail};
        }
    }

    void insertMemory(
        const std::string_view key,
        const std::string_view body,
        const std::string_view tagsJson,
        const std::string_view createdAt = "2025-02-03T04:05:06Z",
        const std::string_view updatedAt = "2025-02-03T04:05:07Z")
    {
        sqlite3_stmt* statement{};
        constexpr std::string_view Sql =
            "INSERT OR REPLACE INTO memory_notes("
            "key,body,tags_json,created_at,updated_at) VALUES(?,?,?,?,?)";
        requireSqlite(
            sqlite3_prepare_v2(
                database_, Sql.data(), static_cast<int>(Sql.size()),
                &statement, nullptr),
            "prepare memory insert");
        try {
            bindText(statement, 1, key);
            bindText(statement, 2, body);
            bindText(statement, 3, tagsJson);
            bindText(statement, 4, createdAt);
            bindText(statement, 5, updatedAt);
            requireSqlite(sqlite3_step(statement), "step memory insert", SQLITE_DONE);
            const int finalizeResult = sqlite3_finalize(statement);
            statement = nullptr;
            requireSqlite(finalizeResult, "finalize memory insert");
        } catch (...) {
            if (statement != nullptr) {
                static_cast<void>(sqlite3_finalize(statement));
            }
            throw;
        }
    }

    [[nodiscard]] std::int64_t queryInteger(const std::string_view sql)
    {
        sqlite3_stmt* statement = prepare(sql);
        try {
            requireSqlite(sqlite3_step(statement), "step integer query", SQLITE_ROW);
            const auto value = sqlite3_column_int64(statement, 0);
            requireSqlite(sqlite3_step(statement), "finish integer query", SQLITE_DONE);
            const int finalizeResult = sqlite3_finalize(statement);
            statement = nullptr;
            requireSqlite(finalizeResult, "finalize integer query");
            return value;
        } catch (...) {
            if (statement != nullptr) {
                static_cast<void>(sqlite3_finalize(statement));
            }
            throw;
        }
    }

    [[nodiscard]] std::string queryText(const std::string_view sql)
    {
        sqlite3_stmt* statement = prepare(sql);
        try {
            requireSqlite(sqlite3_step(statement), "step text query", SQLITE_ROW);
            REQUIRE(sqlite3_column_type(statement, 0) != SQLITE_NULL);
            const int byteCount = sqlite3_column_bytes(statement, 0);
            REQUIRE(byteCount >= 0);
            const auto* const text = sqlite3_column_text(statement, 0);
            REQUIRE(text != nullptr || byteCount == 0);
            std::string value;
            if (byteCount != 0) {
                value.assign(
                    reinterpret_cast<const char*>(text),
                    static_cast<std::size_t>(byteCount));
            }
            requireSqlite(sqlite3_step(statement), "finish text query", SQLITE_DONE);
            const int finalizeResult = sqlite3_finalize(statement);
            statement = nullptr;
            requireSqlite(finalizeResult, "finalize text query");
            return value;
        } catch (...) {
            if (statement != nullptr) {
                static_cast<void>(sqlite3_finalize(statement));
            }
            throw;
        }
    }

private:
    void requireSqlite(
        const int actual,
        const std::string_view action,
        const int expected = SQLITE_OK) const
    {
        if (actual != expected) {
            throw std::runtime_error{
                std::string{"Could not "} + std::string{action} + ": " +
                sqlite3_errmsg(database_)};
        }
    }

    void bindText(
        sqlite3_stmt* const statement,
        const int parameter,
        const std::string_view value) const
    {
        REQUIRE(value.size() <= static_cast<std::size_t>(
                                    (std::numeric_limits<int>::max)()));
        requireSqlite(
            sqlite3_bind_text(
                statement,
                parameter,
                value.data(),
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT),
            "bind test text");
    }

    [[nodiscard]] sqlite3_stmt* prepare(const std::string_view sql)
    {
        REQUIRE(sql.size() <= static_cast<std::size_t>(
                                  (std::numeric_limits<int>::max)()));
        sqlite3_stmt* statement{};
        requireSqlite(
            sqlite3_prepare_v2(
                database_,
                sql.data(),
                static_cast<int>(sql.size()),
                &statement,
                nullptr),
            "prepare test query");
        return statement;
    }

    sqlite3* database_{};
};

void createFixtureDatabase(
    const std::filesystem::path& directory,
    const std::filesystem::path& fixtureDirectory)
{
    SqliteDatabase database{directory / L"store.sqlite", true};
    database.execute(Support::readFixture(fixtureDirectory / L"central-v5.sql"));
}

[[nodiscard]] Domain::LegacyMemorySetOutcome upsert(
    RepositoryFixture& fixture,
    std::string key,
    std::string body,
    std::vector<std::string> tags,
    const std::string_view correlation)
{
    return take(fixture.repository->upsert(
        Domain::LegacyMemoryUpsert{
            std::move(key), std::move(body), std::move(tags)},
        activeContext(*fixture.clock, correlation)));
}

[[nodiscard]] bool containsKey(
    const std::vector<Domain::LegacyMemoryNoteProjection>& notes,
    const std::string_view key)
{
    return std::ranges::any_of(notes, [key](const auto& note) {
        return note.key == key;
    });
}

void freshRoundTripAndRestartPersistence()
{
    Support::ScopedTestDirectory directory{L"legacy-memory-roundtrip"};
    RepositoryFixture fixture{directory.path()};
    const std::string body{"caf\xC3\xA9 body"};

    const auto stored = upsert(
        fixture, "notes/alpha", body, {"alpha", "zeta"},
        "p09-roundtrip-set");
    REQUIRE(stored.stored);
    REQUIRE(stored.note.key == "notes/alpha");
    REQUIRE(stored.note.body == body);
    REQUIRE(stored.note.tags == std::vector<std::string>({"alpha", "zeta"}));
    REQUIRE(stored.note.createdAt == stored.note.updatedAt);

    const auto nonCanonical = fixture.repository->upsert(
        Domain::LegacyMemoryUpsert{
            "notes/noncanonical", "must not persist", {" zeta ", "zeta"}},
        activeContext(*fixture.clock, "p09-roundtrip-noncanonical-tags"));
    REQUIRE(!nonCanonical);
    REQUIRE(nonCanonical.error().code == Domain::ErrorCodes::InvalidRequest);
    const auto absentNonCanonical = take(fixture.repository->get(
        "notes/noncanonical",
        activeContext(*fixture.clock, "p09-roundtrip-noncanonical-absent")));
    REQUIRE(!absentNonCanonical.note.has_value());

    const auto fetched = take(fixture.repository->get(
        "notes/alpha", activeContext(*fixture.clock, "p09-roundtrip-get")));
    REQUIRE(fetched.key == "notes/alpha");
    REQUIRE(fetched.note.has_value());
    REQUIRE(*fetched.note == stored.note);

    const auto summaryList = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::nullopt, std::nullopt, false, false, 50U},
        activeContext(*fixture.clock, "p09-roundtrip-list-summary")));
    REQUIRE(summaryList.visibleTotal == 1U);
    REQUIRE(summaryList.notes.size() == 1U);
    REQUIRE(!summaryList.notes.front().body.has_value());
    REQUIRE(summaryList.notes.front().bodyUtf8Bytes == body.size());

    const auto fullList = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::nullopt, std::nullopt, false, true, 50U},
        activeContext(*fixture.clock, "p09-roundtrip-list-full")));
    REQUIRE(fullList.notes.size() == 1U);
    REQUIRE(fullList.notes.front().body == std::optional<std::string>{body});
    REQUIRE(fullList.notes.front().bodyUtf8Bytes == body.size());

    const auto searched = take(fixture.repository->search(
        Domain::LegacyMemorySearchQuery{"ZETA", false, true, 50U},
        activeContext(*fixture.clock, "p09-roundtrip-search")));
    REQUIRE(searched.query == "ZETA");
    REQUIRE(searched.notes.size() == 1U);
    REQUIRE(searched.notes.front().key == "notes/alpha");

    const auto missingDelete = take(fixture.repository->remove(
        "notes/missing",
        activeContext(*fixture.clock, "p09-roundtrip-delete-missing")));
    REQUIRE(missingDelete.key == "notes/missing");
    REQUIRE(!missingDelete.deleted);
    REQUIRE(!missingDelete.existed);
    REQUIRE(!missingDelete.systemKey);

    const auto deleted = take(fixture.repository->remove(
        "notes/alpha", activeContext(*fixture.clock, "p09-roundtrip-delete")));
    REQUIRE(deleted.deleted && deleted.existed && !deleted.systemKey);
    const auto deletedAgain = take(fixture.repository->remove(
        "notes/alpha",
        activeContext(*fixture.clock, "p09-roundtrip-delete-repeated")));
    REQUIRE(!deletedAgain.deleted && !deletedAgain.existed);

    static_cast<void>(upsert(
        fixture, "notes/durable", "persist exactly", {"durable"},
        "p09-roundtrip-durable"));
    fixture.close();
    fixture.open("p09-roundtrip-reopen");
    const auto durable = take(fixture.repository->get(
        "notes/durable",
        activeContext(*fixture.clock, "p09-roundtrip-get-reopened")));
    REQUIRE(durable.note.has_value());
    REQUIRE(durable.note->body == "persist exactly");
    REQUIRE(durable.note->tags == std::vector<std::string>{"durable"});
}

void upsertPreservesCreationTimestamp()
{
    Support::ScopedTestDirectory directory{L"legacy-memory-upsert-time"};
    RepositoryFixture fixture{directory.path()};

    const auto first = upsert(
        fixture, "timestamps/item", "first body", {"first"},
        "p09-time-first");
    fixture.clock->advance(std::chrono::seconds{7});
    const auto second = upsert(
        fixture, "timestamps/item", "replacement body", {"replacement"},
        "p09-time-second");

    REQUIRE(second.note.createdAt == first.note.createdAt);
    REQUIRE(second.note.updatedAt ==
            first.note.updatedAt + std::chrono::seconds{7});
    REQUIRE(second.note.body == "replacement body");
    REQUIRE(second.note.tags == std::vector<std::string>{"replacement"});
    REQUIRE(second.note.updatedAt != second.note.createdAt);
}

void listCompatibilityFilteringAndOrdering()
{
    Support::ScopedTestDirectory directory{L"legacy-memory-list"};
    RepositoryFixture fixture{directory.path()};

    auto putThenAdvance = [&](
                              std::string key,
                              std::vector<std::string> tags = {}) {
        static_cast<void>(upsert(
            fixture, std::move(key), "body", std::move(tags),
            "p09-list-seed"));
        fixture.clock->advance(std::chrono::seconds{1});
    };

    putThenAdvance("literal%/match");
    putThenAdvance("literalX/mismatch");
    putThenAdvance("under_/match");
    putThenAdvance("underX/mismatch");
    putThenAdvance("slash\\path/item");
    putThenAdvance("slashXpath/item");
    putThenAdvance("post/match", {"wanted"});
    putThenAdvance("post/newer", {"Wanted"});
    static_cast<void>(upsert(
        fixture, "order/b", "body", {}, "p09-list-order-b"));
    static_cast<void>(upsert(
        fixture, "order/a", "body", {}, "p09-list-order-a"));
    fixture.clock->advance(std::chrono::seconds{1});
    putThenAdvance("agent_run/hidden");
    putThenAdvance("agent_active/hidden");
    putThenAdvance("continuity/hidden");
    putThenAdvance("Agent_run/visible-case");
    putThenAdvance("agent-run/visible-punctuation");

    const auto percent = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::string{"literal%/"}, std::nullopt, false, false, 200U},
        activeContext(*fixture.clock, "p09-list-percent")));
    REQUIRE(percent.notes.size() == 1U);
    REQUIRE(percent.notes.front().key == "literal%/match");

    const auto underscore = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::string{"under_/"}, std::nullopt, false, false, 200U},
        activeContext(*fixture.clock, "p09-list-underscore")));
    REQUIRE(underscore.notes.size() == 1U);
    REQUIRE(underscore.notes.front().key == "under_/match");

    const auto backslash = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::string{"slash\\path/"}, std::nullopt, false, false, 200U},
        activeContext(*fixture.clock, "p09-list-backslash")));
    REQUIRE(backslash.notes.size() == 1U);
    REQUIRE(backslash.notes.front().key == "slash\\path/item");

    const auto postLimited = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::string{"post/"}, std::string{"wanted"}, false, false, 1U},
        activeContext(*fixture.clock, "p09-list-post-limit")));
    REQUIRE(postLimited.notes.empty());
    REQUIRE(postLimited.visibleTotal == 11U);
    const auto postTwo = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::string{"post/"}, std::string{"wanted"}, false, false, 2U},
        activeContext(*fixture.clock, "p09-list-post-limit-two")));
    REQUIRE(postTwo.notes.size() == 1U);
    REQUIRE(postTwo.notes.front().key == "post/match");
    const auto postOrdered = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::string{"post/"}, std::nullopt, false, false, 2U},
        activeContext(*fixture.clock, "p09-list-updated-order")));
    REQUIRE(postOrdered.notes.size() == 2U);
    REQUIRE(postOrdered.notes[0].key == "post/newer");
    REQUIRE(postOrdered.notes[1].key == "post/match");
    REQUIRE(postOrdered.notes[0].updatedAt > postOrdered.notes[1].updatedAt);

    const auto hidden = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::nullopt, std::nullopt, false, false, 200U},
        activeContext(*fixture.clock, "p09-list-hide-system")));
    REQUIRE(hidden.visibleTotal == 11U);
    REQUIRE(!containsKey(hidden.notes, "agent_run/hidden"));
    REQUIRE(!containsKey(hidden.notes, "agent_active/hidden"));
    REQUIRE(!containsKey(hidden.notes, "continuity/hidden"));
    REQUIRE(!containsKey(hidden.notes, "Agent_run/visible-case"));
    REQUIRE(containsKey(hidden.notes, "agent-run/visible-punctuation"));

    const auto shown = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::nullopt, std::nullopt, true, false, 200U},
        activeContext(*fixture.clock, "p09-list-show-system")));
    REQUIRE(shown.visibleTotal == 15U);
    REQUIRE(containsKey(shown.notes, "agent_run/hidden"));
    REQUIRE(containsKey(shown.notes, "agent_active/hidden"));
    REQUIRE(containsKey(shown.notes, "continuity/hidden"));
    REQUIRE(containsKey(shown.notes, "Agent_run/visible-case"));

    const auto ordered = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::string{"order/"}, std::nullopt, false, false, 200U},
        activeContext(*fixture.clock, "p09-list-tie-order")));
    REQUIRE(ordered.notes.size() == 2U);
    REQUIRE(ordered.notes[0].key == "order/a");
    REQUIRE(ordered.notes[1].key == "order/b");
    REQUIRE(ordered.notes[0].updatedAt == ordered.notes[1].updatedAt);
}

void searchCompatibilityAndBodyProjection()
{
    Support::ScopedTestDirectory directory{L"legacy-memory-search"};
    RepositoryFixture fixture{directory.path()};

    static_cast<void>(upsert(
        fixture, "search/AlphaKey", "neutral body", {"key"},
        "p09-search-key"));
    static_cast<void>(upsert(
        fixture, "search/body", "The NEEDLE is here", {"body"},
        "p09-search-body"));
    static_cast<void>(upsert(
        fixture, "search/tag", "neutral", {"MiXeDTag"},
        "p09-search-tag"));
    static_cast<void>(upsert(
        fixture, "search/percent", "contains % token", {"percent"},
        "p09-search-percent"));
    static_cast<void>(upsert(
        fixture, "search/underscore", "contains _ token", {"underscore"},
        "p09-search-underscore"));
    static_cast<void>(upsert(
        fixture, "search/slash\\leaf", "contains slash token", {"slash"},
        "p09-search-slash"));
    static_cast<void>(upsert(
        fixture, "search/decoy", "contains X token", {"decoy"},
        "p09-search-decoy"));

    auto search = [&](const std::string_view query, const bool includeBody) {
        return take(fixture.repository->search(
            Domain::LegacyMemorySearchQuery{
                std::string{query}, false, includeBody, 200U},
            activeContext(*fixture.clock, "p09-search-query")));
    };

    const auto key = search("alphakey", false);
    REQUIRE(key.notes.size() == 1U);
    REQUIRE(key.notes.front().key == "search/AlphaKey");
    REQUIRE(!key.notes.front().body.has_value());
    REQUIRE(key.notes.front().bodyUtf8Bytes == std::string_view{"neutral body"}.size());
    const auto body = search("needle", true);
    REQUIRE(body.notes.size() == 1U);
    REQUIRE(body.notes.front().key == "search/body");
    REQUIRE(body.notes.front().body == std::optional<std::string>{"The NEEDLE is here"});
    const auto tag = search("mixedtag", true);
    REQUIRE(tag.notes.size() == 1U);
    REQUIRE(tag.notes.front().key == "search/tag");

    const auto percent = search("%", false);
    REQUIRE(percent.notes.size() == 1U);
    REQUIRE(percent.notes.front().key == "search/percent");
    const auto underscore = search("_", false);
    REQUIRE(underscore.notes.size() == 1U);
    REQUIRE(underscore.notes.front().key == "search/underscore");
    const auto backslash = search("\\", false);
    REQUIRE(backslash.notes.size() == 1U);
    REQUIRE(backslash.notes.front().key == "search/slash\\leaf");
}

void centralVersion5MigrationCompatibility(
    const std::filesystem::path& fixtureDirectory)
{
    Support::ScopedTestDirectory directory{L"legacy-memory-v5-migration"};
    createFixtureDatabase(directory.path(), fixtureDirectory);
    RepositoryFixture fixture{directory.path()};

    const auto migrated = take(fixture.repository->get(
        "legacy/note",
        activeContext(*fixture.clock, "p09-migration-read")));
    REQUIRE(migrated.note.has_value());
    REQUIRE(migrated.note->key == "legacy/note");
    REQUIRE(migrated.note->body == "central-v5-preservation-sentinel");
    REQUIRE(migrated.note->tags ==
            std::vector<std::string>({"legacy", "migration"}));
    REQUIRE(migrated.note->createdAt == utcTime(2025, 1U, 2U, 3, 4, 5));
    REQUIRE(migrated.note->updatedAt == utcTime(2025, 1U, 2U, 3, 4, 6));

    const auto updated = upsert(
        fixture,
        "legacy/note",
        "central-v5-updated-exactly",
        {"legacy", "updated"},
        "p09-migration-update");
    REQUIRE(updated.note.createdAt == utcTime(2025, 1U, 2U, 3, 4, 5));
    REQUIRE(updated.note.updatedAt == fixture.clock->utcNow());
    fixture.close();
    fixture.open("p09-migration-reopen");
    const auto reopened = take(fixture.repository->get(
        "legacy/note",
        activeContext(*fixture.clock, "p09-migration-reopened-read")));
    REQUIRE(reopened.note.has_value());
    REQUIRE(reopened.note->body == "central-v5-updated-exactly");
    REQUIRE(reopened.note->tags ==
            std::vector<std::string>({"legacy", "updated"}));
    REQUIRE(reopened.note->createdAt == utcTime(2025, 1U, 2U, 3, 4, 5));
    fixture.close();

    SqliteDatabase database{directory.path() / L"store.sqlite", false};
    REQUIRE(database.queryInteger(
                "SELECT version FROM schema_version") == 6);
    REQUIRE(database.queryText(
                "SELECT summary FROM agent_sessions "
                "WHERE id='legacy-session-1'") ==
            "central-v5-agent-session-sentinel");
    REQUIRE(database.queryText(
                "SELECT args_json FROM audit_events WHERE id=7") ==
            "{\"key\":\"legacy/note\",\"value\":\"central-v5-preservation-sentinel\"}");
    REQUIRE(database.queryInteger(
                "SELECT write_sequence FROM context_handoffs "
                "WHERE id='legacy-handoff-1'") == 41);
    REQUIRE(database.queryText(
                "SELECT packet_json FROM context_handoffs "
                "WHERE id='legacy-handoff-1'")
                .find("central-v5-handoff-sentinel") != std::string::npos);
}

void malformedAndOversizedPersistedTags()
{
    Support::ScopedTestDirectory directory{L"legacy-memory-hostile-tags"};
    RepositoryFixture fixture{directory.path()};
    fixture.close();
    {
        SqliteDatabase database{directory.path() / L"store.sqlite", false};
        database.insertMemory("tags/malformed", "body", "{not-json");
        database.insertMemory("tags/non-array", "body", "{\"tag\":\"value\"}");
        database.insertMemory(
            "tags/heterogeneous", "body", "[\"valid\",7]");
        database.insertMemory(
            "tags/noncanonical", "body", "[\"zeta\",\"alpha\",\"zeta\",\"\"]");
        database.insertMemory(
            "tags/oversized",
            "body",
            "[\"" + std::string(
                Domain::LegacyMemoryLimits::MaximumTagBytes + 1U, 'x') +
                "\"]");
    }
    fixture.open("p09-hostile-tags-reopen");

    const auto malformed = take(fixture.repository->get(
        "tags/malformed",
        activeContext(*fixture.clock, "p09-hostile-tags-malformed")));
    REQUIRE(malformed.note.has_value());
    REQUIRE(malformed.note->tags.empty());
    const auto nonArray = take(fixture.repository->get(
        "tags/non-array",
        activeContext(*fixture.clock, "p09-hostile-tags-non-array")));
    REQUIRE(nonArray.note.has_value());
    REQUIRE(nonArray.note->tags.empty());
    const auto heterogeneous = take(fixture.repository->get(
        "tags/heterogeneous",
        activeContext(*fixture.clock, "p09-hostile-tags-heterogeneous")));
    REQUIRE(heterogeneous.note.has_value());
    REQUIRE(heterogeneous.note->tags.empty());
    const auto noncanonical = take(fixture.repository->get(
        "tags/noncanonical",
        activeContext(*fixture.clock, "p09-hostile-tags-noncanonical")));
    REQUIRE(noncanonical.note.has_value());
    REQUIRE(noncanonical.note->tags ==
            std::vector<std::string>({"zeta", "alpha", "zeta", ""}));
    const auto oversized = fixture.repository->get(
        "tags/oversized",
        activeContext(*fixture.clock, "p09-hostile-tags-oversized"));
    REQUIRE(!oversized);
    REQUIRE(oversized.error().code == Domain::ErrorCodes::IntegrityFailure);

    const std::string decomposedEAcute{"e\xCC\x81", 3U};
    const std::string composedEAcute{"\xC3\xA9", 2U};
    const auto canonical = upsert(
        fixture,
        "tags/canonical-equivalence",
        "body",
        {decomposedEAcute},
        "p09-hostile-tags-canonical-write");
    REQUIRE(canonical.note.tags ==
            std::vector<std::string>({decomposedEAcute}));
    const auto crossFormFilter = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::string{"tags/canonical-equivalence"},
            composedEAcute,
            false,
            false,
            50U},
        activeContext(*fixture.clock, "p09-hostile-tags-cross-form-filter")));
    REQUIRE(crossFormFilter.notes.size() == 1U);
    REQUIRE(crossFormFilter.notes.front().tags.front() == decomposedEAcute);

    const auto canonicalDuplicate = fixture.repository->upsert(
        Domain::LegacyMemoryUpsert{
            "tags/canonical-duplicate",
            "must not persist",
            {decomposedEAcute, composedEAcute}},
        activeContext(*fixture.clock, "p09-hostile-tags-canonical-duplicate"));
    REQUIRE(!canonicalDuplicate);
    REQUIRE(canonicalDuplicate.error().code ==
            Domain::ErrorCodes::InvalidRequest);
    const auto absentDuplicate = take(fixture.repository->get(
        "tags/canonical-duplicate",
        activeContext(*fixture.clock, "p09-hostile-tags-canonical-absent")));
    REQUIRE(!absentDuplicate.note.has_value());

    // Exercise the production application normalization and the production
    // Windows repository together. The service must collapse canonically
    // equivalent input spellings while retaining the first original spelling,
    // and the repository must match the opposite spelling after persistence.
    {
        Application::LegacyMemoryService service{
            *fixture.repository, fixture.unicodeCanonicalizer};
        const auto stored = take(service.set(
            Domain::LegacyMemorySetRequest{
                "tags/service-repository-integration",
                std::string{"body"},
                {decomposedEAcute, composedEAcute}},
            activeContext(
                *fixture.clock,
                "p09-service-repository-canonical-write")));
        REQUIRE(stored.note.tags ==
                std::vector<std::string>({decomposedEAcute}));

        const auto matched = take(service.list(
            Domain::LegacyMemoryListRequest{
                std::string{"tags/service-repository-integration"},
                composedEAcute,
                false,
                false,
                50},
            activeContext(
                *fixture.clock,
                "p09-service-repository-canonical-filter")));
        REQUIRE(matched.notes.size() == 1U);
        REQUIRE(matched.notes.front().tags ==
                std::vector<std::string>({decomposedEAcute}));
    }
}

void cancellationDeadlineCloseAndQuickCheck()
{
    Support::ScopedTestDirectory directory{L"legacy-memory-context-close"};
    RepositoryFixture fixture{directory.path()};

    std::stop_source cancellation;
    REQUIRE(cancellation.request_stop());
    const auto cancelled = fixture.repository->upsert(
        Domain::LegacyMemoryUpsert{"cancelled/key", "body", {}},
        activeContext(
            *fixture.clock,
            "p09-context-cancelled",
            cancellation.get_token()));
    REQUIRE(!cancelled);
    REQUIRE(cancelled.error().code == Domain::ErrorCodes::Cancelled);

    const auto expired = fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::nullopt, std::nullopt, false, false, 50U},
        expiredContext(*fixture.clock, "p09-context-expired"));
    REQUIRE(!expired);
    REQUIRE(expired.error().code == Domain::ErrorCodes::DeadlineExceeded);

    const auto nullFilter = fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::string{"prefix\0suffix", 13U},
            std::nullopt,
            false,
            false,
            50U},
        activeContext(*fixture.clock, "p09-context-null-filter"));
    REQUIRE(!nullFilter);
    REQUIRE(nullFilter.error().code == Domain::ErrorCodes::InvalidRequest);
    const auto nullQuery = fixture.repository->search(
        Domain::LegacyMemorySearchQuery{
            std::string{"query\0suffix", 12U}, false, true, 50U},
        activeContext(*fixture.clock, "p09-context-null-query"));
    REQUIRE(!nullQuery);
    REQUIRE(nullQuery.error().code == Domain::ErrorCodes::InvalidRequest);

    take(fixture.repository->quickCheck(
        activeContext(*fixture.clock, "p09-context-quick-check")));
    const auto missing = take(fixture.repository->get(
        "cancelled/key",
        activeContext(*fixture.clock, "p09-context-no-mutation")));
    REQUIRE(!missing.note.has_value());

    auto closedRepository = fixture.repository;
    fixture.close();
    closedRepository->close();
    const auto closedGet = closedRepository->get(
        "cancelled/key",
        activeContext(*fixture.clock, "p09-context-closed-get"));
    REQUIRE(!closedGet);
    REQUIRE(closedGet.error().code == Domain::ErrorCodes::InvalidRequest);
    const auto closedCheck = closedRepository->quickCheck(
        activeContext(*fixture.clock, "p09-context-closed-check"));
    REQUIRE(!closedCheck);
    REQUIRE(closedCheck.error().code == Domain::ErrorCodes::InvalidRequest);
}

void purgeConfirmationAuditAndIsolation(
    const std::filesystem::path& fixtureDirectory)
{
    Support::ScopedTestDirectory directory{L"legacy-memory-purge"};
    createFixtureDatabase(directory.path(), fixtureDirectory);
    RepositoryFixture fixture{directory.path()};
    static_cast<void>(upsert(
        fixture, "purge/secret", "PURGE-SENSITIVE-BODY", {"private"},
        "p09-purge-user"));
    static_cast<void>(upsert(
        fixture, "agent_run/purge", "system", {}, "p09-purge-agent-run"));
    static_cast<void>(upsert(
        fixture, "agent_active/purge", "system", {}, "p09-purge-agent-active"));
    static_cast<void>(upsert(
        fixture, "continuity/purge", "system", {}, "p09-purge-continuity"));

    const auto rejected = fixture.repository->purge(
        Domain::DestructiveConfirmation{
            "purge_legacy_memory",
            "legacy-global-memory",
            "purge legacy global memory"},
        activeContext(*fixture.clock, "p09-purge-rejected"));
    REQUIRE(!rejected);
    fixture.close();
    {
        SqliteDatabase database{directory.path() / L"store.sqlite", false};
        REQUIRE(database.queryInteger("SELECT COUNT(*) FROM memory_notes") == 5);
        REQUIRE(database.queryInteger("SELECT COUNT(*) FROM audit_events") == 1);
    }

    fixture.open("p09-purge-reopen");
    const auto purged = take(fixture.repository->purge(
        Domain::DestructiveConfirmation{
            "purge_legacy_memory",
            "legacy-global-memory",
            "PURGE LEGACY GLOBAL MEMORY"},
        activeContext(*fixture.clock, "p09-purge-confirmed")));
    REQUIRE(purged.verified);
    REQUIRE(purged.notesRemoved == 5U);
    const auto repeated = take(fixture.repository->purge(
        Domain::DestructiveConfirmation{
            "purge_legacy_memory",
            "legacy-global-memory",
            "PURGE LEGACY GLOBAL MEMORY"},
        activeContext(*fixture.clock, "p09-purge-repeated")));
    REQUIRE(repeated.verified);
    REQUIRE(repeated.notesRemoved == 0U);
    fixture.close();

    SqliteDatabase database{directory.path() / L"store.sqlite", false};
    REQUIRE(database.queryInteger("SELECT COUNT(*) FROM memory_notes") == 0);
    REQUIRE(database.queryInteger("SELECT COUNT(*) FROM audit_events") == 3);
    REQUIRE(database.queryText(
                "SELECT tool FROM audit_events ORDER BY id DESC LIMIT 1") ==
            "purge_legacy_memory");
    REQUIRE(database.queryText(
                "SELECT status FROM audit_events ORDER BY id DESC LIMIT 1") ==
            "ok");
    REQUIRE(database.queryInteger(
                "SELECT mutating FROM audit_events ORDER BY id DESC LIMIT 1") == 1);
    const auto auditTimestamp = database.queryText(
        "SELECT timestamp FROM audit_events ORDER BY id DESC LIMIT 1");
    REQUIRE(auditTimestamp == "2026-01-02T03:04:05Z");
    const auto arguments = database.queryText(
        "SELECT arguments_json FROM audit_events ORDER BY id DESC LIMIT 1");
    REQUIRE(arguments == "{\"scope\":\"legacy-global-memory\"}");
    REQUIRE(arguments.find("PURGE LEGACY GLOBAL MEMORY") == std::string::npos);
    REQUIRE(arguments.find("PURGE-SENSITIVE-BODY") == std::string::npos);
    REQUIRE(arguments.find("purge/secret") == std::string::npos);
    REQUIRE(database.queryText(
                "SELECT args_json FROM audit_events ORDER BY id DESC LIMIT 1") ==
            arguments);

    REQUIRE(database.queryText(
                "SELECT summary FROM agent_sessions "
                "WHERE id='legacy-session-1'") ==
            "central-v5-agent-session-sentinel");
    REQUIRE(database.queryText(
                "SELECT cwd FROM presence WHERE client_id='legacy-client-1'") ==
            "legacy-root");
    REQUIRE(database.queryInteger(
                "SELECT write_sequence FROM context_handoffs "
                "WHERE id='legacy-handoff-1'") == 41);
    REQUIRE(database.queryText(
                "SELECT args_json FROM audit_events WHERE id=7") ==
            "{\"key\":\"legacy/note\",\"value\":\"central-v5-preservation-sentinel\"}");
}

void concurrentOperationsRemainSerialized()
{
    Support::ScopedTestDirectory directory{L"legacy-memory-concurrent"};
    RepositoryFixture fixture{directory.path()};
    constexpr std::size_t ThreadCount = 4U;
    constexpr std::size_t NotesPerThread = 6U;
    std::barrier start{static_cast<std::ptrdiff_t>(ThreadCount)};
    std::array<std::string, ThreadCount> failures{};
    std::vector<std::thread> threads;
    threads.reserve(ThreadCount);

    for (std::size_t threadIndex = 0U;
         threadIndex < ThreadCount;
         ++threadIndex) {
        threads.emplace_back([&, threadIndex] {
            try {
                start.arrive_and_wait();
                for (std::size_t noteIndex = 0U;
                     noteIndex < NotesPerThread;
                     ++noteIndex) {
                    const std::string key =
                        "concurrent/" + std::to_string(threadIndex) + "/" +
                        std::to_string(noteIndex);
                    const std::string correlation =
                        "p09-concurrent-" + std::to_string(threadIndex) + "-" +
                        std::to_string(noteIndex);
                    const auto stored = take(fixture.repository->upsert(
                        Domain::LegacyMemoryUpsert{
                            key, "thread body", {"concurrent"}},
                        activeContext(*fixture.clock, correlation)));
                    if (stored.note.key != key) {
                        throw std::runtime_error{"Concurrent upsert returned the wrong key."};
                    }
                    const auto searched = take(fixture.repository->search(
                        Domain::LegacyMemorySearchQuery{key, false, false, 1U},
                        activeContext(*fixture.clock, correlation + "-search")));
                    if (searched.notes.size() != 1U ||
                        searched.notes.front().key != key) {
                        throw std::runtime_error{"Concurrent search lost its stored key."};
                    }
                }
            } catch (const std::exception& error) {
                failures[threadIndex] = error.what();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto& failure : failures) {
        REQUIRE(failure.empty());
    }

    const auto listed = take(fixture.repository->list(
        Domain::LegacyMemoryListQuery{
            std::string{"concurrent/"}, std::nullopt, false, false, 200U},
        activeContext(*fixture.clock, "p09-concurrent-final-list")));
    REQUIRE(listed.visibleTotal == ThreadCount * NotesPerThread);
    REQUIRE(listed.notes.size() == ThreadCount * NotesPerThread);
    REQUIRE(std::ranges::is_sorted(
        listed.notes,
        {},
        &Domain::LegacyMemoryNoteProjection::key));
    take(fixture.repository->quickCheck(
        activeContext(*fixture.clock, "p09-concurrent-quick-check")));
}

} // namespace

int main(const int argumentCount, const char* const* const arguments)
{
    try {
        if (argumentCount != 2) {
            throw std::runtime_error{
                "Expected the persistence fixture directory as argv[1]."};
        }
        const std::filesystem::path fixtureDirectory{arguments[1]};

        freshRoundTripAndRestartPersistence();
        std::cout << "PASS legacy_memory_repository.roundtrip_restart\n";
        upsertPreservesCreationTimestamp();
        std::cout << "PASS legacy_memory_repository.upsert_timestamps\n";
        listCompatibilityFilteringAndOrdering();
        std::cout << "PASS legacy_memory_repository.list_compatibility\n";
        searchCompatibilityAndBodyProjection();
        std::cout << "PASS legacy_memory_repository.search_compatibility\n";
        centralVersion5MigrationCompatibility(fixtureDirectory);
        std::cout << "PASS legacy_memory_repository.central_v5_migration\n";
        malformedAndOversizedPersistedTags();
        std::cout << "PASS legacy_memory_repository.persisted_tags\n";
        cancellationDeadlineCloseAndQuickCheck();
        std::cout << "PASS legacy_memory_repository.context_close_quick_check\n";
        purgeConfirmationAuditAndIsolation(fixtureDirectory);
        std::cout << "PASS legacy_memory_repository.purge_audit_isolation\n";
        concurrentOperationsRemainSerialized();
        std::cout << "PASS legacy_memory_repository.concurrent_serialization\n";
        std::cout << "SUMMARY passed=9 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
