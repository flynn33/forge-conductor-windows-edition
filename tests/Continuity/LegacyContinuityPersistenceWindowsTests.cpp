#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLegacyContinuityProjectionStore.h"
#include "ForgeConductor/Persistence/Windows/WindowsLegacyContinuityRepository.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Persistence/PersistenceTestSupport.h"

#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace Support = ForgeConductor::Tests::PersistenceSupport;

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            throw std::runtime_error{                                             \
                std::string{"Requirement failed: "} + #condition};              \
        }                                                                         \
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

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == expectedCode);
}

[[nodiscard]] Domain::UtcTimePoint utcTime(
    const int second = 0)
{
    const auto date = std::chrono::sys_days{
        std::chrono::year{2026} / std::chrono::January / 2};
    return Domain::UtcTimePoint{
        date.time_since_epoch() + std::chrono::hours{3} +
        std::chrono::minutes{4} + std::chrono::seconds{5 + second}};
}

[[nodiscard]] Domain::OperationContext context(
    const std::string_view operationId,
    const std::string_view correlation)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(operationId),
        std::chrono::steady_clock::now() + std::chrono::minutes{2},
        {},
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] Domain::LegacyHandoffPacket packet(
    const std::string_view id,
    const std::string_view goal,
    const bool resumeReady,
    const int second = 0)
{
    return Domain::LegacyHandoffPacket{
        parse<Domain::LegacyHandoffId>(id),
        Domain::LegacyContinuityLimits::SchemaVersion,
        utcTime(),
        utcTime(second),
        Domain::LegacyHandoffSource::Model,
        resumeReady,
        std::string{"alpha"},
        parse<Domain::ClientId>("legacy-test-client"),
        std::string{goal},
        "in_progress",
        std::string{"forge-windows"},
        std::string{"D:\\workspace"},
        {},
        {"Continue"},
        {"README.md"},
        {},
        {},
        "Durable legacy continuity test.",
        "Continue the durable legacy continuity test.",
        true};
}

[[nodiscard]] Domain::DestructiveConfirmation resetConfirmation()
{
    return Domain::DestructiveConfirmation{
        "reset_legacy_continuity",
        "legacy-context-continuity",
        "RESET LEGACY CONTINUITY"};
}

struct RepositoryFixture final {
    explicit RepositoryFixture(const std::filesystem::path& directory)
        : paths{std::make_shared<Fakes::RecordingApplicationPathsFake>()},
          clock{std::make_shared<Support::FixedClock>(
              utcTime(), std::chrono::steady_clock::now())},
          diagnostics{std::make_shared<Fakes::RuntimeDiagnosticsFake>(
              clock->monotonicNow())},
          hasher{std::make_shared<InfrastructureWindows::BCryptSha256Hasher>()}
    {
        paths->setNow(clock->monotonicNow());
        paths->dataRootResult.set(Domain::Result<Domain::PathText>::success(
            Support::pathText(directory)));
        reopen();
    }

    ~RepositoryFixture() noexcept { close(); }

    void reopen()
    {
        auto databaseResult = PersistenceWindows::WindowsCentralDatabase::open(
            paths,
            diagnostics,
            clock,
            context("10101010-1010-4010-8010-101010101010", "legacy-open"));
        database = std::shared_ptr<PersistenceWindows::WindowsCentralDatabase>{
            take(std::move(databaseResult))};
        first = take(PersistenceWindows::WindowsLegacyContinuityRepository::attach(
            database, clock, hasher));
        second = take(PersistenceWindows::WindowsLegacyContinuityRepository::attach(
            database, clock, hasher));
    }

    void close() noexcept
    {
        if (first) first->close();
        if (second) second->close();
        first.reset();
        second.reset();
        if (database) {
            static_cast<void>(database->close(context(
                "20202020-2020-4020-8020-202020202020", "legacy-close")));
        }
        database.reset();
    }

    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Support::FixedClock> clock;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<InfrastructureWindows::BCryptSha256Hasher> hasher;
    std::shared_ptr<PersistenceWindows::WindowsCentralDatabase> database;
    std::shared_ptr<PersistenceWindows::WindowsLegacyContinuityRepository> first;
    std::shared_ptr<PersistenceWindows::WindowsLegacyContinuityRepository> second;
};

[[nodiscard]] std::int64_t queryInteger(
    const std::filesystem::path& databasePath,
    const std::string_view sql)
{
    sqlite3* database{};
    const auto encoded = Support::pathText(databasePath).value();
    REQUIRE(sqlite3_open_v2(
        encoded.c_str(), &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr) == SQLITE_OK);
    sqlite3_stmt* statement{};
    REQUIRE(sqlite3_prepare_v3(
        database, std::string{sql}.c_str(), -1, 0U, &statement, nullptr) ==
        SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const auto value = sqlite3_column_int64(statement, 0);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    REQUIRE(sqlite3_close_v2(database) == SQLITE_OK);
    return value;
}

void executeSql(
    const std::filesystem::path& databasePath,
    const std::string_view sql)
{
    sqlite3* database{};
    const auto encoded = Support::pathText(databasePath).value();
    REQUIRE(sqlite3_open_v2(
        encoded.c_str(), &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr) == SQLITE_OK);
    char* error{};
    const int result = sqlite3_exec(
        database, std::string{sql}.c_str(), nullptr, nullptr, &error);
    if (error) sqlite3_free(error);
    REQUIRE(result == SQLITE_OK);
    REQUIRE(sqlite3_close_v2(database) == SQLITE_OK);
}

void repositoryCasMergeRestartAndReset()
{
    Support::ScopedTestDirectory directory{L"P11-LegacyContinuity"};
    RepositoryFixture fixture{directory.path()};
    auto initialPacket = packet("legacy-handoff-a", "Initial goal", true);
    auto initial = take(fixture.first->compareExchange(
        Domain::LegacyContinuityCompareExchange{initialPacket, std::nullopt},
        context("30303030-3030-4030-8030-303030303030", "legacy-insert")));
    REQUIRE(initial.writeSequence == 1U);
    REQUIRE(initial.documents.packetJson);
    REQUIRE(initial.documents.payloadJson);
    REQUIRE(initial.documents.contentSha256);
    REQUIRE(take(fixture.second->get(
        initial.packet.id,
        context("31313131-3131-4131-8131-313131313131", "legacy-get"))) == initial);
    REQUIRE(take(fixture.first->latest(
        initial.packet.clientId,
        true,
        context("32323232-3232-4232-8232-323232323232", "legacy-latest"))) == initial);

    std::barrier start{2};
    std::exception_ptr firstFailure;
    std::exception_ptr secondFailure;
    const auto update = [&](const bool goalWriter,
                            Contracts::ILegacyContinuityRepository& repository,
                            std::exception_ptr& failure,
                            const std::string_view operationId,
                            const std::string_view correlation) {
        try {
            auto current = take(repository.get(
                initial.packet.id, context(operationId, correlation)));
            REQUIRE(current);
            start.arrive_and_wait();
            for (std::size_t attempt{};
                 attempt < Domain::LegacyContinuityLimits::MaximumConflictRetries;
                 ++attempt) {
                auto candidate = current->packet;
                candidate.updatedAt = utcTime(goalWriter ? 1 : 2);
                if (goalWriter) {
                    candidate.goal = "Merged goal";
                } else {
                    candidate.decisions = {"Preserve disjoint edit"};
                }
                auto result = repository.compareExchange(
                    Domain::LegacyContinuityCompareExchange{
                        candidate, current->writeSequence},
                    context(operationId, correlation));
                if (result) return;
                REQUIRE(result.error().code == Domain::ErrorCodes::Conflict);
                current = take(repository.get(
                    initial.packet.id, context(operationId, correlation)));
                REQUIRE(current);
            }
            throw std::runtime_error{"CAS retry limit was exhausted"};
        } catch (...) {
            failure = std::current_exception();
        }
    };
    std::thread goalThread{
        update,
        true,
        std::ref(*fixture.first),
        std::ref(firstFailure),
        "40404040-4040-4040-8040-404040404040",
        "legacy-goal-writer"};
    std::thread decisionThread{
        update,
        false,
        std::ref(*fixture.second),
        std::ref(secondFailure),
        "41414141-4141-4141-8141-414141414141",
        "legacy-decision-writer"};
    goalThread.join();
    decisionThread.join();
    if (firstFailure) std::rethrow_exception(firstFailure);
    if (secondFailure) std::rethrow_exception(secondFailure);

    const auto merged = take(fixture.first->get(
        initial.packet.id,
        context("42424242-4242-4242-8242-424242424242", "legacy-merged")));
    REQUIRE(merged);
    REQUIRE(merged->packet.goal == "Merged goal");
    REQUIRE(merged->packet.decisions ==
            std::vector<std::string>{"Preserve disjoint edit"});
    REQUIRE(merged->writeSequence == 3U);
    const auto pointers = take(fixture.first->repairPointers(
        context("43434343-4343-4343-8343-434343434343", "legacy-repair")));
    REQUIRE(pointers.latestId == merged->packet.id);
    REQUIRE(pointers.resumeReadyId == merged->packet.id);

    fixture.close();
    fixture.reopen();
    const auto restarted = take(fixture.first->get(
        initial.packet.id,
        context("44444444-4444-4044-8044-444444444444", "legacy-restart")));
    REQUIRE(restarted == merged);

    const auto databasePath = directory.path() / L"store.sqlite";
    executeSql(
        databasePath,
        "UPDATE context_handoffs SET content_sha256="
        "'0000000000000000000000000000000000000000000000000000000000000000'");
    requireError(
        fixture.first->get(
            initial.packet.id,
            context("45454545-4545-4545-8545-454545454545", "legacy-bad-sha")),
        Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(merged->documents.contentSha256);
    executeSql(
        databasePath,
        "UPDATE context_handoffs SET content_sha256='" +
            merged->documents.contentSha256->value() + "'");
    executeSql(
        databasePath,
        "INSERT INTO memory_notes(key,body,tags_json,created_at,updated_at) "
        "VALUES('ordinary-note','keep me','[]','2026-01-02T03:04:05Z',"
        "'2026-01-02T03:04:05Z')");

    const auto reset = take(fixture.first->reset(
        resetConfirmation(),
        context("46464646-4646-4646-8646-464646464646", "legacy-reset")));
    REQUIRE(reset.handoffsRemoved == 1U);
    REQUIRE(reset.pointerNotesRemoved == 2U);
    REQUIRE(reset.authoritativeScopeCommitted);
    REQUIRE(reset.verified);
    REQUIRE(!take(fixture.first->get(
        initial.packet.id,
        context("47474747-4747-4747-8747-474747474747", "legacy-empty"))));
    take(fixture.first->quickCheck(
        context("48484848-4848-4848-8848-484848484848", "legacy-check")));
    REQUIRE(queryInteger(
        databasePath,
        "SELECT COUNT(*) FROM context_handoffs") == 0);
    REQUIRE(queryInteger(
        databasePath,
        "SELECT COUNT(*) FROM memory_notes WHERE key='ordinary-note'") == 1);
}

struct MemoryStorage final {
    std::mutex mutex;
    std::map<std::string, std::vector<std::byte>> files;
};

class MemoryAtomicFileStore final : public Contracts::IAtomicFileStore {
public:
    explicit MemoryAtomicFileStore(std::shared_ptr<MemoryStorage> storage)
        : storage_{std::move(storage)}
    {
    }

    [[nodiscard]] Domain::Result<std::vector<std::byte>> read(
        const Contracts::AuthorizedPath& path,
        const std::size_t maximumBytes,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{storage_->mutex};
            const auto found = storage_->files.find(path.canonicalPath().value());
            if (found == storage_->files.end()) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::RecordNotFound, "Missing projection."));
            }
            if (found->second.size() > maximumBytes) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge, "Projection too large."));
            }
            return Domain::Result<std::vector<std::byte>>::success(found->second);
        } catch (...) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure, "Memory read failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> replace(
        const Contracts::AuthorizedPath& path,
        const std::span<const std::byte> content,
        const bool,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{storage_->mutex};
            const auto found = storage_->files.find(path.canonicalPath().value());
            if (path.access() == Domain::FileAccess::Write &&
                found == storage_->files.end()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::RecordNotFound, "Missing projection."));
            }
            if (path.access() == Domain::FileAccess::Create &&
                found != storage_->files.end()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict, "Projection already exists."));
            }
            storage_->files.insert_or_assign(
                path.canonicalPath().value(),
                std::vector<std::byte>{content.begin(), content.end()});
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure, "Memory replace failed."));
        }
    }

private:
    std::shared_ptr<MemoryStorage> storage_;
};

class MemoryFileSystem final : public Contracts::IFileSystem {
public:
    explicit MemoryFileSystem(std::shared_ptr<MemoryStorage> storage)
        : storage_{std::move(storage)}
    {
    }

    [[nodiscard]] Domain::Result<std::vector<std::byte>> readFile(
        const Contracts::AuthorizedPath&,
        std::size_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable, "Unused."));
    }

    [[nodiscard]] Domain::Result<void> writeFile(
        const Contracts::AuthorizedPath&,
        std::span<const std::byte>,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable, "Unused."));
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::PathText>> list(
        const Contracts::AuthorizedPath& directory,
        const std::size_t maximumEntries,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{storage_->mutex};
            std::vector<Domain::PathText> paths;
            const auto prefix = directory.canonicalPath().value() + "\\";
            for (const auto& [path, content] : storage_->files) {
                static_cast<void>(content);
                if (path.starts_with(prefix)) {
                    paths.push_back(take(Domain::PathText::create(path)));
                }
            }
            if (paths.size() > maximumEntries) {
                return Domain::Result<std::vector<Domain::PathText>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::LimitExceeded, "Too many projections."));
            }
            return Domain::Result<std::vector<Domain::PathText>>::success(
                std::move(paths));
        } catch (...) {
            return Domain::Result<std::vector<Domain::PathText>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure, "Memory list failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> createDirectory(
        const Contracts::AuthorizedPath&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> remove(
        const Contracts::AuthorizedPath& path,
        const bool,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{storage_->mutex};
            if (storage_->files.erase(path.canonicalPath().value()) == 0U) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::RecordNotFound, "Missing projection."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure, "Memory remove failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> move(
        const Contracts::AuthorizedPath&,
        const Contracts::AuthorizedPath&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable, "Unused."));
    }

private:
    std::shared_ptr<MemoryStorage> storage_;
};

[[nodiscard]] Domain::LegacyContinuityRecord projectionRecord(
    const std::string_view id,
    const std::uint64_t sequence,
    const bool resumeReady)
{
    return Domain::LegacyContinuityRecord{
        packet(id, std::string{"Projection "} + std::string{id}, resumeReady),
        sequence,
        Domain::LegacyContinuityDocuments{
            std::string{"{\"projection\":true}"}, std::nullopt, std::nullopt}};
}

void projectionOrderingRepairAndReset()
{
    const auto memoryRoot = take(Domain::PathText::create("C:\\forge\\memory"));
    const auto handoffsRoot = take(Domain::PathText::create(
        "C:\\forge\\memory\\handoffs"));
    const auto now = std::chrono::steady_clock::now();
    auto authority = std::make_shared<Fakes::DeterministicWorkspaceAuthority>(
        parse<Domain::AuthorityId>("50505050-5050-4050-8050-505050505050"),
        parse<Domain::ClientId>("projection-test-client"),
        std::vector<Domain::PathText>{memoryRoot},
        Domain::FileAccess::Write,
        std::vector<Domain::FileAccess>{
            Domain::FileAccess::Read,
            Domain::FileAccess::Write,
            Domain::FileAccess::Create,
            Domain::FileAccess::Delete},
        std::vector<Domain::FileAccess>{}, false, 1U);
    authority->setNow(now);
    auto issued = take(authority->authorityFor(
        parse<Domain::ProjectId>("51515151-5151-4151-8151-515151515151"),
        context("52525252-5252-4252-8252-525252525252", "projection-authority")));
    auto storage = std::make_shared<MemoryStorage>();
    auto atomic = std::make_shared<MemoryAtomicFileStore>(storage);
    auto files = std::make_shared<MemoryFileSystem>(storage);
    auto clock = std::make_shared<Support::FixedClock>(utcTime(), now);
    auto projections = take(
        InfrastructureWindows::WindowsLegacyContinuityProjectionStore::create(
            memoryRoot, handoffsRoot, std::move(issued), authority,
            atomic, files, clock));

    const auto newer = projectionRecord("projection-newer", 9U, true);
    const auto older = projectionRecord("projection-older", 4U, false);
    const auto newerReceipt = take(projections->write(
        newer,
        context("53535353-5353-4353-8353-535353535353", "projection-newer")));
    REQUIRE(newerReceipt.packetPath);
    REQUIRE(newerReceipt.currentTaskPath);
    static_cast<void>(take(projections->write(
        older,
        context("54545454-5454-4454-8454-545454545454", "projection-older"))));
    {
        std::lock_guard lock{storage->mutex};
        const auto latest = storage->files.at(
            "C:\\forge\\memory\\handoffs\\LATEST");
        const std::string latestText{
            reinterpret_cast<const char*>(latest.data()), latest.size()};
        REQUIRE(latestText == newer.packet.id.value());
    }

    const std::vector<Domain::LegacyContinuityRecord> repairRecords{
        newer, older};
    const auto repaired = take(projections->repair(
        repairRecords,
        Domain::LegacyContinuityPointerRepairOutcome{
            newer.packet.id, newer.packet.id, 0U},
        context("55555555-5555-4555-8555-555555555555", "projection-repair")));
    REQUIRE(repaired.packetFilesWritten == 2U);
    REQUIRE(repaired.latestWritten);
    REQUIRE(repaired.currentTaskWritten);

    const auto removed = take(projections->reset(
        resetConfirmation(),
        context("56565656-5656-4656-8656-565656565656", "projection-reset")));
    REQUIRE(removed >= 7U);
    {
        std::lock_guard lock{storage->mutex};
        REQUIRE(storage->files.empty());
    }
    projections->close();
    requireError(
        projections->write(
            newer,
            context("57575757-5757-4757-8757-575757575757", "projection-closed")),
        Domain::ErrorCodes::InvalidRequest);
}

using TestFunction = void (*)();

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, TestFunction>> tests{
        {"legacy_continuity.repository_cas_merge_restart_reset",
         repositoryCasMergeRestartAndReset},
        {"legacy_continuity.projection_ordering_repair_reset",
         projectionOrderingRepairAndReset}};
    std::size_t passed{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    std::cout << "Passed " << passed << '/' << tests.size()
              << " legacy continuity persistence tests.\n";
    return EXIT_SUCCESS;
}
