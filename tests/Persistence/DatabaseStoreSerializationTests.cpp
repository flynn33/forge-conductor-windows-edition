#include "PersistenceTestSupport.h"

#include "Fakes/DiagnosticsFakes.h"
#include "Persistence/Windows/Detail/WindowsDatabaseStore.h"

#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Persistence::Windows::Detail {

class WindowsDatabaseStoreTestAccess final {
public:
    [[nodiscard]] static Domain::Result<
        Infrastructure::Windows::Detail::BoundedSerialExecutor::Lease>
    acquireFacadeAdmission(
        WindowsDatabaseStore& store,
        const Domain::OperationContext& context) noexcept
    {
        return store.facadeAdmission_.acquire(context, "Hold test database admission");
    }
};

} // namespace ForgeConductor::Persistence::Windows::Detail

namespace ForgeConductor::Tests {
namespace {

namespace PersistenceDetail = ForgeConductor::Persistence::Windows::Detail;
namespace TestFakes = ForgeConductor::Tests::Fakes;

using PersistenceSupport::FixedClock;
using PersistenceSupport::ScopedTestDirectory;
using PersistenceSupport::activeContext;

struct StoreDependencies final {
    explicit StoreDependencies()
    {
        const auto now = std::chrono::steady_clock::now();
        diagnostics = std::make_shared<TestFakes::RuntimeDiagnosticsFake>(now);
        clock = std::make_shared<FixedClock>(
            Domain::UtcTimePoint{std::chrono::seconds{1'767'326'645}}, now);
    }

    std::shared_ptr<TestFakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<FixedClock> clock;
};

[[nodiscard]] std::unique_ptr<PersistenceDetail::WindowsDatabaseStore> openStore(
    const std::filesystem::path& directory,
    StoreDependencies& dependencies,
    const Domain::OperationContext& context)
{
    return take(PersistenceDetail::WindowsDatabaseStore::open(
        PersistenceSupport::pathText(directory),
        L"store.sqlite",
        L"store.sqlite.migration.lock",
        dependencies.diagnostics,
        dependencies.clock,
        PersistenceDetail::WindowsDatabaseStoreOptions{
            Persistence::Windows::Migrations::DatabaseKind::Central,
            PersistenceDetail::WinsqliteSynchronousMode::Full,
            false},
        context));
}

[[nodiscard]] Domain::OperationContext boundedContext(
    const std::chrono::milliseconds duration,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("22222222-2222-4222-8222-222222222222"),
        std::chrono::steady_clock::now() + duration,
        cancellation,
        parse<Domain::CorrelationId>("p07-store-serialization")};
}

void testCloseAdmissionHonorsDeadlineAndCancellation()
{
    ScopedTestDirectory directory{L"store-admission-context"};
    StoreDependencies dependencies;
    auto store = openStore(
        directory.path(), dependencies, activeContext("p07-store-admission-open"));

    {
        auto held = take(PersistenceDetail::WindowsDatabaseStoreTestAccess::
            acquireFacadeAdmission(
                *store, activeContext("p07-store-admission-holder")));
        static_cast<void>(held);

        std::barrier started{2};
        std::optional<Domain::Result<void>> closeResult;
        std::jthread closer{[&]() noexcept {
            started.arrive_and_wait();
            closeResult.emplace(store->close(
                boundedContext(std::chrono::milliseconds{150})));
        }};
        started.arrive_and_wait();
        closer.join();

        require(closeResult.has_value(),
                "deadline-bounded close did not produce a result");
        requireError(
            closeResult.value(),
            Domain::ErrorCodes::DeadlineExceeded,
            "close did not honor its deadline while facade admission was owned");
    }

    take(store->quickCheck(activeContext("p07-store-after-close-timeout")));

    {
        auto held = take(PersistenceDetail::WindowsDatabaseStoreTestAccess::
            acquireFacadeAdmission(
                *store, activeContext("p07-store-cancellation-holder")));
        static_cast<void>(held);

        std::stop_source cancellation;
        std::barrier started{2};
        std::optional<Domain::Result<
            Persistence::Windows::DatabaseSchemaSnapshot>> snapshotResult;
        std::jthread reader{[&]() noexcept {
            started.arrive_and_wait();
            snapshotResult.emplace(store->schemaSnapshot(
                boundedContext(std::chrono::seconds{5}, cancellation.get_token())));
        }};
        started.arrive_and_wait();
        cancellation.request_stop();
        reader.join();

        require(snapshotResult.has_value(),
                "cancelled schema admission did not produce a result");
        requireError(
            snapshotResult.value(),
            Domain::ErrorCodes::Cancelled,
            "schema snapshot did not honor cancellation while admission was owned");
    }

    take(store->close(activeContext("p07-store-admission-final-close")));
    require(dependencies.diagnostics->activeOwnership(
                Contracts::RuntimeOwnerKind::OpenDatabase) == 0U,
            "serialized close retained runtime database ownership");
}

struct OperationOutcome final {
    bool success{};
    std::string errorCode;
};

void testConcurrentOperationsSerializeAgainstClose()
{
    ScopedTestDirectory directory{L"store-operation-close-race"};
    StoreDependencies dependencies;
    auto store = openStore(
        directory.path(), dependencies, activeContext("p07-store-race-open"));

    constexpr std::size_t OperationCount = 10U;
    std::array<OperationOutcome, OperationCount> outcomes{};
    std::optional<Domain::Result<void>> closeResult;
    std::barrier start{static_cast<std::ptrdiff_t>(OperationCount + 2U)};
    std::vector<std::jthread> workers;
    workers.reserve(OperationCount + 1U);

    for (std::size_t index = 0U; index < OperationCount; ++index) {
        workers.emplace_back([&, index]() noexcept {
            start.arrive_and_wait();
            if ((index % 2U) == 0U) {
                auto result = store->quickCheck(
                    boundedContext(std::chrono::seconds{10}));
                outcomes[index].success = result.hasValue();
                if (!result) {
                    outcomes[index].errorCode = result.error().code;
                }
            } else {
                auto result = store->schemaSnapshot(
                    boundedContext(std::chrono::seconds{10}));
                outcomes[index].success = result.hasValue();
                if (!result) {
                    outcomes[index].errorCode = result.error().code;
                }
            }
        });
    }
    workers.emplace_back([&]() noexcept {
        start.arrive_and_wait();
        closeResult.emplace(store->close(
            boundedContext(std::chrono::seconds{10})));
    });

    start.arrive_and_wait();
    for (auto& worker : workers) {
        worker.join();
    }

    require(closeResult.has_value() && closeResult.value().hasValue(),
            "close failed while racing admitted database operations");
    for (const auto& outcome : outcomes) {
        require(
            outcome.success || outcome.errorCode == Domain::ErrorCodes::InvalidRequest,
            "an operation racing close escaped the serialized success-or-closed contract");
    }
    requireError(
        store->quickCheck(activeContext("p07-store-race-closed-check")),
        Domain::ErrorCodes::InvalidRequest,
        "the serialized store accepted work after close completed");
    require(dependencies.diagnostics->activeOwnership(
                Contracts::RuntimeOwnerKind::OpenDatabase) == 0U,
            "operation-close serialization retained runtime database ownership");
}

} // namespace

void registerDatabaseStoreSerializationTests(TestRegistry& tests)
{
    addTest(tests, "persistence.store.admission-context",
            testCloseAdmissionHonorsDeadlineAndCancellation);
    addTest(tests, "persistence.store.operation-close-serialization",
            testConcurrentOperationsSerializeAgainstClose);
}

} // namespace ForgeConductor::Tests
