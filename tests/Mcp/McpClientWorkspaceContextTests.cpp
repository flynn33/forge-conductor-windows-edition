#include "ForgeConductor/Mcp/McpClientWorkspaceContext.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Mcp = ForgeConductor::Mcp;

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        if (!(condition)) {                                                      \
            throw std::runtime_error{                                            \
                std::string{"Requirement failed: "} + #condition};             \
        }                                                                        \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T id(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

class FixedClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{1s};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return Domain::MonotonicTimePoint{1s};
    }
};

class RegistryFake final : public Contracts::IProjectRegistryRepository {
public:
    explicit RegistryFake(
        std::vector<Domain::ProjectMemoryDescriptor> descriptors)
        : descriptors_{std::move(descriptors)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::ProjectInitialization>::failure(
            unsupported());
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryDescriptor> descriptor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            const auto found = std::find_if(
                descriptors_.begin(),
                descriptors_.end(),
                [&](const Domain::ProjectMemoryDescriptor& descriptorValue) {
                    return descriptorValue.id == projectId;
                });
            if (found == descriptors_.end()) {
                return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProjectNotFound,
                        "The deterministic project was not found."));
            }
            return Domain::Result<Domain::ProjectMemoryDescriptor>::success(
                *found);
        } catch (...) {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                internalFailure());
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>
    list(
        const std::size_t maximumCount,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::function<void()> callback;
            std::vector<Domain::ProjectMemoryDescriptor> result;
            {
                std::unique_lock lock{mutex_};
                const auto invocation = ++listCalls_;
                if (blockFirstList_ && invocation == 1U) {
                    firstListBlocked_ = true;
                    stateChanged_.notify_all();
                    stateChanged_.wait(lock, [&]() {
                        return releaseFirstList_;
                    });
                }
                result = descriptors_;
                callback = callback_;
            }
            if (callback) {
                callback();
            }
            if (result.size() > maximumCount) {
                result.erase(
                    result.begin() +
                        static_cast<std::ptrdiff_t>(maximumCount),
                    result.end());
            }
            return Domain::Result<
                std::vector<Domain::ProjectMemoryDescriptor>>::success(
                    std::move(result));
        } catch (...) {
            return Domain::Result<
                std::vector<Domain::ProjectMemoryDescriptor>>::failure(
                    internalFailure());
        }
    }

    [[nodiscard]] Domain::Result<void> detachAlias(
        const Domain::ProjectId&,
        const Domain::PathText&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::failure(unsupported());
    }

    void blockFirstList() noexcept
    {
        std::lock_guard lock{mutex_};
        blockFirstList_ = true;
    }

    void waitUntilFirstListBlocked()
    {
        std::unique_lock lock{mutex_};
        stateChanged_.wait(lock, [&]() { return firstListBlocked_; });
    }

    void releaseBlockedList() noexcept
    {
        std::lock_guard lock{mutex_};
        releaseFirstList_ = true;
        stateChanged_.notify_all();
    }

    void setListCallback(std::function<void()> callback)
    {
        std::lock_guard lock{mutex_};
        callback_ = std::move(callback);
    }

private:
    [[nodiscard]] static Domain::Error unsupported()
    {
        return Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "This deterministic registry operation is not used by the test.");
    }

    [[nodiscard]] static Domain::Error internalFailure()
    {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The deterministic registry failed internally.");
    }

    std::mutex mutex_;
    std::condition_variable stateChanged_;
    std::vector<Domain::ProjectMemoryDescriptor> descriptors_;
    std::function<void()> callback_;
    std::size_t listCalls_{};
    bool blockFirstList_{};
    bool firstListBlocked_{};
    bool releaseFirstList_{};
};

class WorkspaceAuthorityFake final : public Contracts::IWorkspaceAuthority {
public:
    struct Binding final {
        Domain::ProjectId projectId;
        Domain::AuthorityId authorityId;
        Domain::PathText root;
    };

    explicit WorkspaceAuthorityFake(std::vector<Binding> bindings)
        : bindings_{std::move(bindings)}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext&) noexcept override
    {
        try {
            const auto found = find(projectId);
            if (found == bindings_.end()) {
                return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                    notFound());
            }
            return issueAuthority(
                found->authorityId,
                found->projectId,
                id<Domain::ClientId>("registered-workspace-authority"),
                {found->root},
                Domain::FileAccess::Read,
                {Domain::FileAccess::Read},
                {},
                false,
                1U);
        } catch (...) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                internalFailure());
        }
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority&,
        const std::vector<Domain::PathText>&,
        const std::vector<Domain::FileAccess>&,
        bool,
        std::uint64_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The deterministic authority does not narrow capabilities."));
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathAuthorizationRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        try {
            const auto found = find(authority.projectId());
            if (found == bindings_.end() ||
                request.access != Domain::FileAccess::Read ||
                !request.basePath || request.basePath.value() != found->root ||
                !isWithin(request.requestedPath, found->root)) {
                return Domain::Result<Contracts::AuthorizedPath>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::PathOutsideAuthority,
                        "The deterministic path is outside its authority root."));
            }
            return issueAuthorizedPath(
                authority,
                request.requestedPath,
                found->root,
                request.access);
        } catch (...) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                internalFailure());
        }
    }

private:
    using Iterator = std::vector<Binding>::const_iterator;

    [[nodiscard]] Iterator find(const Domain::ProjectId& projectId) const noexcept
    {
        return std::find_if(
            bindings_.begin(),
            bindings_.end(),
            [&](const Binding& binding) {
                return binding.projectId == projectId;
            });
    }

    [[nodiscard]] static bool isWithin(
        const Domain::PathText& candidate,
        const Domain::PathText& root) noexcept
    {
        const auto& value = candidate.value();
        const auto& rootValue = root.value();
        if (value == rootValue) {
            return true;
        }
        return value.starts_with(rootValue) && value.size() > rootValue.size() &&
            (rootValue.ends_with('\\') || rootValue.ends_with('/') ||
             value[rootValue.size()] == '\\' ||
             value[rootValue.size()] == '/');
    }

    [[nodiscard]] static Domain::Error notFound()
    {
        return Domain::makeError(
            Domain::ErrorCodes::ProjectNotFound,
            "The deterministic workspace authority was not found.");
    }

    [[nodiscard]] static Domain::Error internalFailure()
    {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The deterministic authority failed internally.");
    }

    std::vector<Binding> bindings_;
};

[[nodiscard]] Domain::ProjectId projectA()
{
    return id<Domain::ProjectId>(
        "11111111-1111-4111-8111-111111111111");
}

[[nodiscard]] Domain::ProjectId projectB()
{
    return id<Domain::ProjectId>(
        "22222222-2222-4222-8222-222222222222");
}

[[nodiscard]] Domain::ClientId client(const std::string_view value)
{
    return id<Domain::ClientId>(value);
}

[[nodiscard]] Domain::OperationContext context()
{
    return Domain::OperationContext{
        id<Domain::OperationId>(
            "33333333-3333-4333-8333-333333333333"),
        Domain::MonotonicTimePoint{10s},
        std::stop_token{},
        id<Domain::CorrelationId>("client-workspace-context-test")};
}

[[nodiscard]] Domain::LegacyContinuityRecord record(
    const std::string_view handoffId,
    const std::uint64_t writeSequence,
    std::optional<std::string> workingDirectory,
    std::vector<std::string> keyFiles = {})
{
    return Domain::LegacyContinuityRecord{
        Domain::LegacyHandoffPacket{
            id<Domain::LegacyHandoffId>(handoffId),
            Domain::LegacyContinuityLimits::SchemaVersion,
            Domain::UtcTimePoint{1s},
            Domain::UtcTimePoint{2s},
            Domain::LegacyHandoffSource::Model,
            true,
            std::nullopt,
            client("packet-author"),
            "Recover the registered workspace",
            "handoff_ready",
            std::nullopt,
            std::move(workingDirectory),
            {},
            {},
            std::move(keyFiles),
            {},
            {},
            "Recovered continuity narrative",
            "Continue from recovered continuity",
            true},
        writeSequence,
        Domain::LegacyContinuityDocuments{}};
}

struct Fixture final {
    Domain::PathText rootA{path("C:\\workspace\\alpha")};
    Domain::PathText rootB{path("D:\\workspace\\beta")};
    RegistryFake registry{{
        Domain::ProjectMemoryDescriptor{
            projectA(), "Alpha", std::nullopt, {rootA}},
        Domain::ProjectMemoryDescriptor{
            projectB(), "Beta", std::nullopt, {rootB}}}};
    WorkspaceAuthorityFake authority{{
        WorkspaceAuthorityFake::Binding{
            projectA(),
            id<Domain::AuthorityId>(
                "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
            rootA},
        WorkspaceAuthorityFake::Binding{
            projectB(),
            id<Domain::AuthorityId>(
                "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
            rootB}}};
    FixedClock clock;
    Mcp::McpClientWorkspaceContext subject{registry, authority, clock};
};

void adoptsCanonicalRootAndIsolatesClients()
{
    Fixture fixture;
    const auto firstClient = client("first-client");
    const auto adopted = take(fixture.subject.adopt(
        firstClient,
        record(
            "handoff-alpha",
            7U,
            std::string{"C:\\workspace\\alpha\\src"}),
        context()));

    REQUIRE(adopted.snapshot.has_value());
    REQUIRE(!adopted.warning.has_value());
    REQUIRE(!adopted.superseded);
    REQUIRE(adopted.snapshot->clientId == firstClient);
    REQUIRE(adopted.snapshot->projectId == projectA());
    REQUIRE(adopted.snapshot->authorityRoot == fixture.rootA);
    REQUIRE(adopted.snapshot->authorityRoot.value() !=
            "C:\\workspace\\alpha\\src");
    REQUIRE(adopted.snapshot->handoffId ==
            id<Domain::LegacyHandoffId>("handoff-alpha"));
    REQUIRE(adopted.snapshot->writeSequence == 7U);
    REQUIRE(adopted.snapshot->generation != 0U);

    const auto retained = take(fixture.subject.snapshot(firstClient, context()));
    REQUIRE(retained == adopted.snapshot);
    REQUIRE(!take(fixture.subject.snapshot(
        client("unrelated-client"), context())).has_value());
}

void usesAbsoluteKeyFileFallbackAndClearsOnNoMatch()
{
    Fixture fixture;
    const auto caller = client("fallback-client");
    REQUIRE(take(fixture.subject.adopt(
        caller,
        record(
            "initial-handoff",
            1U,
            std::string{"C:\\workspace\\alpha"}),
        context())).snapshot.has_value());

    const auto fallback = take(fixture.subject.adopt(
        caller,
        record(
            "key-file-handoff",
            2U,
            std::string{"relative-directory"},
            {"relative.cpp", "D:\\workspace\\beta\\src\\main.cpp"}),
        context()));
    REQUIRE(fallback.snapshot.has_value());
    REQUIRE(fallback.snapshot->projectId == projectB());
    REQUIRE(fallback.snapshot->authorityRoot == fixture.rootB);

    const auto outside = take(fixture.subject.adopt(
        caller,
        record(
            "outside-handoff",
            3U,
            std::string{"E:\\unregistered"},
            {"relative-only.cpp"}),
        context()));
    REQUIRE(!outside.snapshot.has_value());
    REQUIRE(outside.warning.has_value());
    REQUIRE(outside.warning->code == Domain::ErrorCodes::PathOutsideAuthority);
    REQUIRE(!take(fixture.subject.snapshot(caller, context())).has_value());
}

void newerReservationSupersedesSlowerAdoption()
{
    Fixture fixture;
    const auto caller = client("concurrent-client");
    fixture.registry.blockFirstList();
    std::optional<Domain::Result<Domain::ClientWorkspaceAdoption>> slowResult;
    std::jthread slowWorker{[&]() {
        slowResult.emplace(fixture.subject.adopt(
            caller,
            record(
                "slow-handoff",
                10U,
                std::string{"C:\\workspace\\alpha"}),
            context()));
    }};
    fixture.registry.waitUntilFirstListBlocked();

    const auto newer = take(fixture.subject.adopt(
        caller,
        record(
            "newer-handoff",
            11U,
            std::string{"D:\\workspace\\beta"}),
        context()));
    fixture.registry.releaseBlockedList();
    slowWorker.join();

    REQUIRE(newer.snapshot.has_value());
    REQUIRE(newer.snapshot->projectId == projectB());
    REQUIRE(slowResult.has_value());
    const auto slower = take(std::move(slowResult).value());
    REQUIRE(slower.superseded);
    REQUIRE(slower.warning.has_value());
    REQUIRE(slower.warning->code == Domain::ErrorCodes::Conflict);
    REQUIRE(slower.warning->retryable);
    REQUIRE(slower.snapshot.has_value());
    REQUIRE(slower.snapshot->projectId == projectB());
    REQUIRE(take(fixture.subject.snapshot(caller, context()))->projectId ==
            projectB());
}

void dependenciesRunOutsideStateLockAndStateIsBounded()
{
    Fixture fixture;
    const auto callbackClient = client("callback-client");
    bool callbackCompleted{};
    fixture.registry.setListCallback([&]() {
        const auto observed = fixture.subject.snapshot(
            callbackClient, context());
        REQUIRE(observed.hasValue());
        callbackCompleted = true;
    });
    REQUIRE(take(fixture.subject.adopt(
        callbackClient,
        record(
            "callback-handoff",
            20U,
            std::string{"C:\\workspace\\alpha"}),
        context())).snapshot.has_value());
    REQUIRE(callbackCompleted);
    fixture.registry.setListCallback({});

    for (std::size_t index{};
         index < Mcp::McpClientWorkspaceContext::MaximumTrackedClients + 4U;
         ++index) {
        const auto boundedClient = client(
            "bounded-client-" + std::to_string(index));
        REQUIRE(take(fixture.subject.adopt(
            boundedClient,
            record(
                "bounded-handoff-" + std::to_string(index),
                100U + index,
                std::string{"C:\\workspace\\alpha"}),
            context())).snapshot.has_value());
        REQUIRE(fixture.subject.trackedClientCount() <=
                Mcp::McpClientWorkspaceContext::MaximumTrackedClients);
    }
    REQUIRE(fixture.subject.trackedClientCount() ==
            Mcp::McpClientWorkspaceContext::MaximumTrackedClients);

    fixture.subject.shutdown();
    const auto afterShutdown = fixture.subject.snapshot(
        callbackClient, context());
    REQUIRE(!afterShutdown);
    REQUIRE(afterShutdown.error().code == Domain::ErrorCodes::TransportClosed);
}

} // namespace

int main()
{
    try {
        static_assert(std::is_final_v<Mcp::McpClientWorkspaceContext>);
        static_assert(std::is_base_of_v<
            Contracts::IMcpClientWorkspaceContext,
            Mcp::McpClientWorkspaceContext>);
        adoptsCanonicalRootAndIsolatesClients();
        usesAbsoluteKeyFileFallbackAndClearsOnNoMatch();
        newerReservationSupersedesSlowerAdoption();
        dependenciesRunOutsideStateLockAndStateIsBounded();
        std::cout << "MCP client workspace context tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
