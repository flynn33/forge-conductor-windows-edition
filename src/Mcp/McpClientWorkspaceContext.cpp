#include "ForgeConductor/Mcp/McpClientWorkspaceContext.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Mcp {
namespace {

constexpr std::size_t MaximumRegistryAliasBytes = 4U * 1024U * 1024U;
constexpr std::size_t MaximumRecoveryComparisonBytes = 8U * 1024U * 1024U;

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    const char* const message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, message, retryable));
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock) noexcept
{
    if (context.isCancellationRequested()) {
        return failure<void>(
            Domain::ErrorCodes::Cancelled,
            "The MCP client-workspace operation was cancelled.");
    }
    if (context.isExpired(clock.monotonicNow())) {
        return failure<void>(
            Domain::ErrorCodes::DeadlineExceeded,
            "The MCP client-workspace operation exceeded its deadline.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] bool isExpectedNonMatch(
    const std::string_view code) noexcept
{
    return code == Domain::ErrorCodes::InvalidRequest ||
        code == Domain::ErrorCodes::ProjectNotFound ||
        code == Domain::ErrorCodes::ProjectScopeMismatch ||
        code == Domain::ErrorCodes::RecordNotFound ||
        code == Domain::ErrorCodes::Unauthorized ||
        code == Domain::ErrorCodes::PathOutsideAuthority;
}

[[nodiscard]] bool isAsciiLetter(const char value) noexcept
{
    return (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z');
}

[[nodiscard]] bool isSeparator(const char value) noexcept
{
    return value == '\\' || value == '/';
}

[[nodiscard]] bool isAbsoluteLocalPath(
    const std::string_view value) noexcept
{
    if (value.size() >= 3U && isAsciiLetter(value[0]) &&
        value[1] == ':' && isSeparator(value[2])) {
        return true;
    }
    if (value.size() < 5U || !isSeparator(value[0]) ||
        !isSeparator(value[1]) || isSeparator(value[2])) {
        return false;
    }
    return std::any_of(
        value.begin() + 3,
        value.end(),
        [](const char character) { return isSeparator(character); });
}

[[nodiscard]] char foldedPathCharacter(const char value) noexcept
{
    if (isSeparator(value)) {
        return '\\';
    }
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

// This is only a bounded prefilter. IWorkspaceAuthority performs the actual
// canonical containment and reparse-point validation before a root is retained.
[[nodiscard]] bool canBeWithinAlias(
    const std::string_view candidate,
    const std::string_view alias) noexcept
{
    if (candidate.size() < alias.size() || alias.empty()) {
        return false;
    }
    for (std::size_t index{}; index < alias.size(); ++index) {
        if (foldedPathCharacter(candidate[index]) !=
            foldedPathCharacter(alias[index])) {
            return false;
        }
    }
    if (candidate.size() == alias.size() ||
        isSeparator(alias.back())) {
        return true;
    }
    return isSeparator(candidate[alias.size()]);
}

struct WorkspaceMatch final {
    Domain::ProjectId projectId;
    Domain::PathText authorityRoot;
};

struct CandidateResolution final {
    std::optional<WorkspaceMatch> match;
    bool ambiguous{};
};

struct WorkspaceResolution final {
    std::optional<WorkspaceMatch> match;
    std::optional<Domain::Error> warning;
};

[[nodiscard]] Domain::Result<CandidateResolution> resolveCandidate(
    const Domain::PathText& candidate,
    const std::vector<Domain::ProjectMemoryDescriptor>& projects,
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    const Contracts::IClock& clock,
    std::size_t& remainingComparisonBytes,
    const Domain::OperationContext& context)
{
    try {
        std::optional<WorkspaceMatch> selected;
        for (const auto& project : projects) {
            auto current = validateContext(context, clock);
            if (!current) {
                return Domain::Result<CandidateResolution>::failure(
                    std::move(current).error());
            }
            if (project.aliases.size() >
                McpClientWorkspaceContext::MaximumAliasesPerProject) {
                return failure<CandidateResolution>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "A registered project exceeded the workspace-alias bound.");
            }

            std::optional<Contracts::WorkspaceAuthority> authority;
            std::size_t aliasIndex{};
            for (const auto& alias : project.aliases) {
                if (context.isCancellationRequested()) {
                    return failure<CandidateResolution>(
                        Domain::ErrorCodes::Cancelled,
                        "The MCP client-workspace operation was cancelled.");
                }
                if ((aliasIndex % 8U) == 0U) {
                    current = validateContext(context, clock);
                    if (!current) {
                        return Domain::Result<CandidateResolution>::failure(
                            std::move(current).error());
                    }
                }
                ++aliasIndex;
                const auto comparisonBytes = (std::min)(
                    candidate.value().size(), alias.value().size());
                if (comparisonBytes > remainingComparisonBytes) {
                    return failure<CandidateResolution>(
                        Domain::ErrorCodes::LimitExceeded,
                        "Recovered workspace matching exceeded its bounded comparison budget.",
                        true);
                }
                remainingComparisonBytes -= comparisonBytes;
                if (!canBeWithinAlias(candidate.value(), alias.value())) {
                    continue;
                }
                if (!authority) {
                    auto issued = workspaceAuthority.authorityFor(
                        project.id, context);
                    if (!issued) {
                        if (isExpectedNonMatch(issued.error().code)) {
                            break;
                        }
                        return Domain::Result<CandidateResolution>::failure(
                            std::move(issued).error());
                    }
                    authority.emplace(std::move(issued).value());
                }
                auto authorized = workspaceAuthority.authorize(
                    *authority,
                    Domain::PathAuthorizationRequest{
                        candidate,
                        alias,
                        Domain::FileAccess::Read,
                        false},
                    context);
                if (!authorized) {
                    if (isExpectedNonMatch(authorized.error().code)) {
                        continue;
                    }
                    return Domain::Result<CandidateResolution>::failure(
                        std::move(authorized).error());
                }

                WorkspaceMatch match{
                    project.id,
                    authorized.value().authorityRoot()};
                if (!selected) {
                    selected.emplace(std::move(match));
                    continue;
                }
                if (selected->projectId != match.projectId ||
                    selected->authorityRoot != match.authorityRoot) {
                    return Domain::Result<CandidateResolution>::success(
                        CandidateResolution{std::nullopt, true});
                }
            }
        }
        return Domain::Result<CandidateResolution>::success(
            CandidateResolution{std::move(selected), false});
    } catch (...) {
        return failure<CandidateResolution>(
            Domain::ErrorCodes::InternalFailure,
            "The recovered workspace candidate could not be resolved.");
    }
}

[[nodiscard]] Domain::Result<WorkspaceResolution> resolveWorkspace(
    const Domain::LegacyContinuityRecord& record,
    Contracts::IProjectRegistryRepository& projectRegistry,
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    const Contracts::IClock& clock,
    const Domain::OperationContext& context)
{
    try {
        auto listed = projectRegistry.list(
            McpClientWorkspaceContext::MaximumRegisteredProjects,
            context);
        if (!listed) {
            return Domain::Result<WorkspaceResolution>::failure(
                std::move(listed).error());
        }
        auto projects = std::move(listed).value();
        if (projects.size() >
            McpClientWorkspaceContext::MaximumRegisteredProjects) {
            return failure<WorkspaceResolution>(
                Domain::ErrorCodes::IntegrityFailure,
                "The project registry exceeded the requested recovery bound.");
        }
        std::size_t registryAliasBytes{};
        for (const auto& project : projects) {
            if (project.aliases.size() >
                McpClientWorkspaceContext::MaximumAliasesPerProject) {
                return failure<WorkspaceResolution>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "A registered project exceeded the workspace-alias bound.");
            }
            for (const auto& alias : project.aliases) {
                if (alias.value().size() >
                    MaximumRegistryAliasBytes - registryAliasBytes) {
                    return failure<WorkspaceResolution>(
                        Domain::ErrorCodes::LimitExceeded,
                        "The project registry exceeded the workspace-alias byte bound.");
                }
                registryAliasBytes += alias.value().size();
            }
        }

        std::size_t attempted{};
        std::size_t remainingComparisonBytes{
            MaximumRecoveryComparisonBytes};
        auto examine = [&](const std::string_view encoded)
            -> Domain::Result<std::optional<WorkspaceResolution>> {
            if (attempted >=
                McpClientWorkspaceContext::MaximumRecoveryCandidates) {
                return Domain::Result<
                    std::optional<WorkspaceResolution>>::success(std::nullopt);
            }
            ++attempted;
            if (!isAbsoluteLocalPath(encoded)) {
                return Domain::Result<
                    std::optional<WorkspaceResolution>>::success(std::nullopt);
            }
            auto candidate = Domain::PathText::create(encoded);
            if (!candidate) {
                return Domain::Result<
                    std::optional<WorkspaceResolution>>::success(std::nullopt);
            }
            auto resolved = resolveCandidate(
                candidate.value(),
                projects,
                workspaceAuthority,
                clock,
                remainingComparisonBytes,
                context);
            if (!resolved) {
                return Domain::Result<
                    std::optional<WorkspaceResolution>>::failure(
                        std::move(resolved).error());
            }
            if (resolved.value().ambiguous) {
                return Domain::Result<
                    std::optional<WorkspaceResolution>>::success(
                        WorkspaceResolution{
                            std::nullopt,
                            Domain::makeError(
                                Domain::ErrorCodes::ProjectScopeMismatch,
                                "The recovered workspace ambiguously matched multiple registered authority roots.")});
            }
            if (resolved.value().match) {
                return Domain::Result<
                    std::optional<WorkspaceResolution>>::success(
                        WorkspaceResolution{
                            std::move(resolved).value().match,
                            std::nullopt});
            }
            return Domain::Result<
                std::optional<WorkspaceResolution>>::success(std::nullopt);
        };

        if (record.packet.workingDirectory &&
            !record.packet.workingDirectory->empty()) {
            auto resolved = examine(*record.packet.workingDirectory);
            if (!resolved) {
                return Domain::Result<WorkspaceResolution>::failure(
                    std::move(resolved).error());
            }
            if (resolved.value()) {
                return Domain::Result<WorkspaceResolution>::success(
                    std::move(resolved).value().value());
            }
        }

        for (const auto& keyFile : record.packet.keyFiles) {
            if (attempted >=
                McpClientWorkspaceContext::MaximumRecoveryCandidates) {
                break;
            }
            auto resolved = examine(keyFile);
            if (!resolved) {
                return Domain::Result<WorkspaceResolution>::failure(
                    std::move(resolved).error());
            }
            if (resolved.value()) {
                return Domain::Result<WorkspaceResolution>::success(
                    std::move(resolved).value().value());
            }
        }

        return Domain::Result<WorkspaceResolution>::success(
            WorkspaceResolution{
                std::nullopt,
                Domain::makeError(
                    Domain::ErrorCodes::PathOutsideAuthority,
                    "The recovered continuity workspace did not match a registered authorized project root.")});
    } catch (...) {
        return failure<WorkspaceResolution>(
            Domain::ErrorCodes::InternalFailure,
            "The recovered workspace could not be matched to the project registry.");
    }
}

} // namespace

class McpClientWorkspaceContext::Implementation final {
public:
    Implementation(
        Contracts::IProjectRegistryRepository& projectRegistry,
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        const Contracts::IClock& clock) noexcept
        : projectRegistry_{projectRegistry},
          workspaceAuthority_{workspaceAuthority},
          clock_{clock}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ClientWorkspaceAdoption> adopt(
        const Domain::ClientId& clientId,
        const Domain::LegacyContinuityRecord& record,
        const Domain::OperationContext& context) noexcept
    {
        std::optional<Reservation> reservation;
        try {
            auto valid = validateContext(context, clock_);
            if (!valid) {
                return Domain::Result<
                    Domain::ClientWorkspaceAdoption>::failure(
                        std::move(valid).error());
            }
            valid = Domain::validateLegacyContinuityRecord(record);
            if (!valid) {
                return Domain::Result<
                    Domain::ClientWorkspaceAdoption>::failure(
                        std::move(valid).error());
            }

            auto reserved = reserve(clientId);
            if (!reserved) {
                return Domain::Result<
                    Domain::ClientWorkspaceAdoption>::failure(
                        std::move(reserved).error());
            }
            reservation.emplace(std::move(reserved).value());

            auto resolved = resolveWorkspace(
                record,
                projectRegistry_,
                workspaceAuthority_,
                clock_,
                context);
            if (!resolved) {
                release(*reservation);
                reservation.reset();
                return Domain::Result<
                    Domain::ClientWorkspaceAdoption>::failure(
                        std::move(resolved).error());
            }
            valid = validateContext(context, clock_);
            if (!valid) {
                release(*reservation);
                reservation.reset();
                return Domain::Result<
                    Domain::ClientWorkspaceAdoption>::failure(
                        std::move(valid).error());
            }

            auto committed = commit(
                *reservation,
                clientId,
                record,
                std::move(resolved).value());
            reservation.reset();
            return committed;
        } catch (...) {
            if (reservation) {
                release(*reservation);
            }
            return failure<Domain::ClientWorkspaceAdoption>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP client workspace could not be adopted.");
        }
    }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::ClientWorkspaceSnapshot>>
    snapshot(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, clock_);
            if (!valid) {
                return Domain::Result<std::optional<
                    Domain::ClientWorkspaceSnapshot>>::failure(
                        std::move(valid).error());
            }

            std::optional<Domain::ClientWorkspaceSnapshot> result;
            {
                std::lock_guard lock{mutex_};
                if (stopping_) {
                    return failure<std::optional<
                        Domain::ClientWorkspaceSnapshot>>(
                            Domain::ErrorCodes::TransportClosed,
                            "The MCP client-workspace context has shut down.");
                }
                ++activeOperations_;
                try {
                    const auto found = entries_.find(clientId.value());
                    if (found != entries_.end()) {
                        result = found->second.snapshot;
                    }
                } catch (...) {
                    --activeOperations_;
                    stateChanged_.notify_all();
                    throw;
                }
            }

            valid = validateContext(context, clock_);
            finishActiveOperation();
            if (!valid) {
                return Domain::Result<std::optional<
                    Domain::ClientWorkspaceSnapshot>>::failure(
                        std::move(valid).error());
            }
            return Domain::Result<std::optional<
                Domain::ClientWorkspaceSnapshot>>::success(std::move(result));
        } catch (...) {
            return failure<std::optional<Domain::ClientWorkspaceSnapshot>>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP client workspace snapshot could not be read.");
        }
    }

    void clear(const Domain::ClientId& clientId) noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            if (stopping_) {
                return;
            }
            const auto found = entries_.find(clientId.value());
            if (found == entries_.end()) {
                return;
            }
            found->second.snapshot.reset();
            found->second.latestReservation = advanceGenerationLocked();
            if (found->second.reservations.empty()) {
                entries_.erase(found);
            }
        } catch (...) {
        }
    }

    void shutdown() noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            stopping_ = true;
            stateChanged_.wait(lock, [&]() { return activeOperations_ == 0U; });
            entries_.clear();
        } catch (...) {
        }
    }

    [[nodiscard]] std::size_t trackedClientCount() const noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return entries_.size();
        } catch (...) {
            return 0U;
        }
    }

private:
    struct Entry final {
        std::optional<Domain::ClientWorkspaceSnapshot> snapshot;
        std::uint64_t latestReservation{};
        std::set<std::uint64_t> reservations;
    };

    struct Reservation final {
        std::string clientKey;
        std::uint64_t generation{};
    };

    [[nodiscard]] Domain::Result<Reservation> reserve(
        const Domain::ClientId& clientId)
    {
        try {
            std::lock_guard lock{mutex_};
            if (stopping_) {
                return failure<Reservation>(
                    Domain::ErrorCodes::TransportClosed,
                    "The MCP client-workspace context has shut down.");
            }
            if (activeReservations_ >= MaximumTrackedClients) {
                return failure<Reservation>(
                    Domain::ErrorCodes::LimitExceeded,
                    "The bounded MCP client-workspace reservations are in use.",
                    true);
            }
            const auto key = clientId.value();
            if (!entries_.contains(key) &&
                entries_.size() >= MaximumTrackedClients) {
                const auto evictable = std::find_if(
                    entries_.begin(),
                    entries_.end(),
                    [](const auto& item) {
                        return item.second.reservations.empty();
                    });
                if (evictable == entries_.end()) {
                    return failure<Reservation>(
                        Domain::ErrorCodes::LimitExceeded,
                        "All bounded MCP client-workspace entries are in use.",
                        true);
                }
                entries_.erase(evictable);
            }
            if (nextGeneration_ ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                return failure<Reservation>(
                    Domain::ErrorCodes::LimitExceeded,
                    "The MCP client-workspace generation cannot advance.");
            }
            if (activeOperations_ ==
                (std::numeric_limits<std::size_t>::max)()) {
                return failure<Reservation>(
                    Domain::ErrorCodes::LimitExceeded,
                    "The MCP client-workspace operation count cannot advance.",
                    true);
            }

            const auto generation = nextGeneration_ + 1U;
            Reservation reservation{key, generation};
            auto [entry, inserted] = entries_.try_emplace(key);
            static_cast<void>(inserted);
            entry->second.reservations.insert(generation);
            entry->second.latestReservation = generation;
            nextGeneration_ = generation;
            ++activeReservations_;
            ++activeOperations_;
            return Domain::Result<Reservation>::success(
                std::move(reservation));
        } catch (...) {
            return failure<Reservation>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP client-workspace reservation could not be created.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::ClientWorkspaceAdoption> commit(
        const Reservation& reservation,
        const Domain::ClientId& clientId,
        const Domain::LegacyContinuityRecord& record,
        WorkspaceResolution resolution) noexcept
    {
        try {
            std::optional<Domain::ClientWorkspaceSnapshot> proposed;
            if (resolution.match) {
                proposed.emplace(Domain::ClientWorkspaceSnapshot{
                    clientId,
                    resolution.match->projectId,
                    resolution.match->authorityRoot,
                    record.packet.id,
                    record.writeSequence,
                    reservation.generation});
            }

            std::lock_guard lock{mutex_};
            const auto found = entries_.find(reservation.clientKey);
            if (found == entries_.end() ||
                !found->second.reservations.contains(
                    reservation.generation) ||
                activeOperations_ == 0U) {
                if (activeReservations_ != 0U) {
                    --activeReservations_;
                }
                if (activeOperations_ != 0U) {
                    --activeOperations_;
                    stateChanged_.notify_all();
                }
                return failure<Domain::ClientWorkspaceAdoption>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The MCP client-workspace reservation was lost.");
            }

            Domain::ClientWorkspaceAdoption outcome;
            if (stopping_) {
                releaseLocked(found, reservation.generation);
                return failure<Domain::ClientWorkspaceAdoption>(
                    Domain::ErrorCodes::TransportClosed,
                    "The MCP client-workspace context shut down during recovery.");
            }
            if (found->second.latestReservation != reservation.generation) {
                outcome.snapshot = found->second.snapshot;
                outcome.warning = Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "A newer recovered workspace superseded this adoption.",
                    true);
                outcome.superseded = true;
                releaseLocked(found, reservation.generation);
                return Domain::Result<
                    Domain::ClientWorkspaceAdoption>::success(
                        std::move(outcome));
            }

            found->second.snapshot = std::move(proposed);
            outcome.snapshot = found->second.snapshot;
            outcome.warning = std::move(resolution.warning);
            releaseLocked(found, reservation.generation);
            return Domain::Result<Domain::ClientWorkspaceAdoption>::success(
                std::move(outcome));
        } catch (...) {
            release(reservation);
            return failure<Domain::ClientWorkspaceAdoption>(
                Domain::ErrorCodes::InternalFailure,
                "The recovered MCP client workspace could not be committed.");
        }
    }

    void release(const Reservation& reservation) noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            const auto found = entries_.find(reservation.clientKey);
            if (found != entries_.end() &&
                found->second.reservations.contains(
                    reservation.generation) &&
                activeOperations_ != 0U) {
                releaseLocked(found, reservation.generation);
            }
        } catch (...) {
        }
    }

    void releaseLocked(
        const std::map<std::string, Entry>::iterator found,
        const std::uint64_t generation) noexcept
    {
        found->second.reservations.erase(generation);
        --activeReservations_;
        --activeOperations_;
        if (found->second.reservations.empty() &&
            !found->second.snapshot) {
            entries_.erase(found);
        }
        stateChanged_.notify_all();
    }

    void finishActiveOperation() noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            if (activeOperations_ != 0U) {
                --activeOperations_;
            }
            stateChanged_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] std::uint64_t advanceGenerationLocked() noexcept
    {
        if (nextGeneration_ !=
            (std::numeric_limits<std::uint64_t>::max)()) {
            ++nextGeneration_;
        }
        return nextGeneration_;
    }

    Contracts::IProjectRegistryRepository& projectRegistry_;
    Contracts::IWorkspaceAuthority& workspaceAuthority_;
    const Contracts::IClock& clock_;
    mutable std::mutex mutex_;
    std::condition_variable stateChanged_;
    std::map<std::string, Entry> entries_;
    std::uint64_t nextGeneration_{};
    std::size_t activeReservations_{};
    std::size_t activeOperations_{};
    bool stopping_{};
};

McpClientWorkspaceContext::McpClientWorkspaceContext(
    Contracts::IProjectRegistryRepository& projectRegistry,
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    const Contracts::IClock& clock)
    : implementation_{std::make_unique<Implementation>(
          projectRegistry,
          workspaceAuthority,
          clock)}
{
}

McpClientWorkspaceContext::~McpClientWorkspaceContext() noexcept
{
    shutdown();
}

Domain::Result<Domain::ClientWorkspaceAdoption>
McpClientWorkspaceContext::adopt(
    const Domain::ClientId& clientId,
    const Domain::LegacyContinuityRecord& record,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::ClientWorkspaceAdoption>(
            Domain::ErrorCodes::TransportClosed,
            "The MCP client-workspace context is unavailable.");
    }
    return implementation_->adopt(clientId, record, context);
}

Domain::Result<std::optional<Domain::ClientWorkspaceSnapshot>>
McpClientWorkspaceContext::snapshot(
    const Domain::ClientId& clientId,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<std::optional<Domain::ClientWorkspaceSnapshot>>(
            Domain::ErrorCodes::TransportClosed,
            "The MCP client-workspace context is unavailable.");
    }
    return implementation_->snapshot(clientId, context);
}

void McpClientWorkspaceContext::clear(
    const Domain::ClientId& clientId) noexcept
{
    if (implementation_) {
        implementation_->clear(clientId);
    }
}

void McpClientWorkspaceContext::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

std::size_t McpClientWorkspaceContext::trackedClientCount() const noexcept
{
    return implementation_ ? implementation_->trackedClientCount() : 0U;
}

} // namespace ForgeConductor::Mcp
