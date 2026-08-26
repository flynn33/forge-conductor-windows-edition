#include "ForgeConductor/Infrastructure/Windows/WindowsLegacyContinuityProjectionStore.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::string_view ResetAction = "reset_legacy_continuity";
constexpr std::string_view ResetScope = "legacy-context-continuity";
constexpr std::string_view ResetToken = "RESET LEGACY CONTINUITY";
constexpr std::size_t MaximumSequenceMarkerBytes = 512U;
constexpr wchar_t ProjectionMutexName[] =
    L"Local\\ForgeConductor.LegacyContinuityProjection.v1";

struct ProjectionFailure final {
    Domain::Error error;
};

[[noreturn]] void fail(Domain::Error error)
{
    throw ProjectionFailure{std::move(error)};
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) fail(std::move(result).error());
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) fail(std::move(result).error());
}

template <typename T, typename Callable>
[[nodiscard]] Domain::Result<T> guarded(Callable&& callable) noexcept
{
    try {
        return Domain::Result<T>::success(std::forward<Callable>(callable)());
    } catch (ProjectionFailure& failure) {
        return Domain::Result<T>::failure(std::move(failure.error));
    } catch (...) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The legacy continuity projection operation failed safely."));
    }
}

[[nodiscard]] Domain::PathText childPath(
    const Domain::PathText& parent,
    const std::string_view name)
{
    if (name.empty() || name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos ||
        !Domain::isValidUtf8(name)) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "A legacy continuity projection file name is invalid."));
    }
    std::string path = parent.value();
    if (!path.ends_with('\\') && !path.ends_with('/')) path.push_back('\\');
    path.append(name);
    return take(Domain::PathText::create(path));
}

[[nodiscard]] std::span<const std::byte> bytesOf(
    const std::string_view text) noexcept
{
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

[[nodiscard]] std::string textOf(const std::vector<std::byte>& bytes)
{
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    std::string text{data, data + bytes.size()};
    if (text.find('\0') != std::string::npos || !Domain::isValidUtf8(text)) {
        fail(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A continuity projection marker is not valid UTF-8."));
    }
    return text;
}

struct LatestMarker final {
    std::uint64_t sequence{};
    std::string id;
};

[[nodiscard]] std::string encodeLatestMarker(const LatestMarker& marker)
{
    return std::to_string(marker.sequence) + "\n" + marker.id;
}

[[nodiscard]] LatestMarker decodeLatestMarker(const std::string_view text)
{
    const auto newline = text.find('\n');
    if (newline == std::string_view::npos || newline == 0U ||
        newline + 1U >= text.size() ||
        text.find('\n', newline + 1U) != std::string_view::npos) {
        fail(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The latest continuity projection marker has an invalid shape."));
    }
    std::uint64_t sequence{};
    const auto parsed = std::from_chars(
        text.data(), text.data() + newline, sequence);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + newline ||
        sequence == 0U) {
        fail(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The latest continuity projection marker has an invalid sequence."));
    }
    auto id = take(Domain::LegacyHandoffId::parse(text.substr(newline + 1U)));
    return LatestMarker{sequence, id.value()};
}

[[nodiscard]] bool isNewer(
    const LatestMarker& candidate,
    const LatestMarker& current) noexcept
{
    return candidate.sequence > current.sequence ||
           (candidate.sequence == current.sequence &&
            candidate.id < current.id);
}

[[nodiscard]] std::string markdown(
    const Domain::LegacyHandoffPacket& packet)
{
    std::string value = "# Current Work\n\n**Status:** " + packet.status +
        "\n**Handoff id:** `" + packet.id.value() +
        "`\n**Source:** " + std::string{Domain::wireName(packet.source)} +
        "\n**Resume ready:** " + (packet.resumeReady ? "true" : "false");
    if (packet.projectSlug) value += "\n**Project slug:** " + *packet.projectSlug;
    if (packet.workingDirectory) {
        value += "\n**Workspace / cwd:** " + *packet.workingDirectory;
    }
    value += "\n\n## Goal\n\n";
    value += packet.goal.empty() ? "_(not set)_\n" : packet.goal + "\n";
    if (!packet.nextActions.empty()) {
        value += "\n## Next actions\n\n";
        for (const auto& action : packet.nextActions) {
            value += "- [ ] " + action + "\n";
        }
    }
    if (!packet.blockers.empty()) {
        value += "\n## Blockers\n\n";
        for (const auto& blocker : packet.blockers) {
            value += "- " + blocker + "\n";
        }
    }
    if (!packet.agents.empty()) {
        value += "\n## Open agents\n\n";
        for (const auto& agent : packet.agents) {
            value += "- **" + agent.agentId.value() + "** `" +
                agent.sessionId.value() + "` — " + agent.status;
            if (!agent.goal.empty()) value += " — " + agent.goal;
            value += '\n';
        }
    }
    if (!packet.narrative.empty()) {
        value += "\n## Narrative\n\n" + packet.narrative + "\n";
    }
    value += "\n## Last updated\n\n";
    // The canonical packet JSON remains the source for exact timestamps. The
    // readable projection intentionally avoids a second timestamp formatter.
    value += "See the matching handoff JSON metadata.\n";
    return value;
}

class NamedMutexLease final {
public:
    NamedMutexLease(HANDLE handle, const DWORD waitResult) noexcept
        : handle_{handle}, owned_{waitResult == WAIT_OBJECT_0 ||
                                 waitResult == WAIT_ABANDONED}
    {
    }

    ~NamedMutexLease() noexcept
    {
        if (owned_) static_cast<void>(::ReleaseMutex(handle_));
    }

    NamedMutexLease(const NamedMutexLease&) = delete;
    NamedMutexLease& operator=(const NamedMutexLease&) = delete;

    NamedMutexLease(NamedMutexLease&& other) noexcept
        : handle_{other.handle_}, owned_{std::exchange(other.owned_, false)}
    {
    }

    NamedMutexLease& operator=(NamedMutexLease&&) = delete;

    [[nodiscard]] bool owns() const noexcept { return owned_; }

private:
    HANDLE handle_{};
    bool owned_{};
};

} // namespace

struct WindowsLegacyContinuityProjectionStore::Impl final {
    Impl(
        Domain::PathText ownedMemoryRoot,
        Domain::PathText ownedHandoffsRoot,
        Contracts::WorkspaceAuthority ownedAuthority,
        std::shared_ptr<Contracts::IWorkspaceAuthority> ownedWorkspaceAuthority,
        std::shared_ptr<Contracts::IAtomicFileStore> ownedAtomicFileStore,
        std::shared_ptr<Contracts::IFileSystem> ownedFileSystem,
        std::shared_ptr<Contracts::IClock> ownedClock,
        HANDLE ownedMutex) noexcept
        : memoryRoot{std::move(ownedMemoryRoot)},
          handoffsRoot{std::move(ownedHandoffsRoot)},
          authority{std::move(ownedAuthority)},
          workspaceAuthority{std::move(ownedWorkspaceAuthority)},
          atomicFileStore{std::move(ownedAtomicFileStore)},
          fileSystem{std::move(ownedFileSystem)},
          clock{std::move(ownedClock)},
          crossProcessMutex{ownedMutex}
    {
    }

    ~Impl() noexcept
    {
        if (crossProcessMutex) static_cast<void>(::CloseHandle(crossProcessMutex));
    }

    [[nodiscard]] Contracts::AuthorizedPath authorize(
        const Domain::PathText& path,
        const Domain::PathText& base,
        const Domain::FileAccess access,
        const Domain::OperationContext& context)
    {
        return take(workspaceAuthority->authorize(
            authority,
            Domain::PathAuthorizationRequest{path, base, access, false},
            context));
    }

    [[nodiscard]] NamedMutexLease acquire(
        const Domain::OperationContext& context)
    {
        if (closed.load(std::memory_order_acquire) || !crossProcessMutex) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The continuity projection store is closed."));
        }
        if (context.isCancellationRequested()) {
            fail(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The continuity projection operation was cancelled."));
        }
        const auto now = clock->monotonicNow();
        if (context.isExpired(now)) {
            fail(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The continuity projection deadline expired."));
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            context.deadline - now);
        const auto bounded = std::clamp<std::int64_t>(
            remaining.count(), 1,
            static_cast<std::int64_t>((std::numeric_limits<DWORD>::max)() - 1U));
        const DWORD result = ::WaitForSingleObject(
            crossProcessMutex, static_cast<DWORD>(bounded));
        NamedMutexLease lease{crossProcessMutex, result};
        if (!lease.owns()) {
            fail(Domain::makeError(
                result == WAIT_TIMEOUT
                    ? Domain::ErrorCodes::DeadlineExceeded
                    : Domain::ErrorCodes::InternalFailure,
                "The continuity projection lock could not be acquired.",
                result == WAIT_TIMEOUT));
        }
        if (context.isCancellationRequested()) {
            fail(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The continuity projection operation was cancelled."));
        }
        return lease;
    }

    void replace(
        const Domain::PathText& path,
        const Domain::PathText& base,
        const std::string_view content,
        const Domain::OperationContext& context)
    {
        auto writeAuthorization = workspaceAuthority->authorize(
            authority,
            Domain::PathAuthorizationRequest{
                path, base, Domain::FileAccess::Write, false},
            context);
        if (writeAuthorization) {
            auto result = atomicFileStore->replace(
                writeAuthorization.value(), bytesOf(content), false, context);
            if (result) return;
            if (result.error().code != Domain::ErrorCodes::RecordNotFound) {
                fail(std::move(result).error());
            }
        }
        auto createAuthorization = authorize(
            path, base, Domain::FileAccess::Create, context);
        take(atomicFileStore->replace(
            createAuthorization, bytesOf(content), false, context));
    }

    [[nodiscard]] std::optional<std::string> read(
        const Domain::PathText& path,
        const Domain::PathText& base,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context)
    {
        auto readAuthorization = workspaceAuthority->authorize(
            authority,
            Domain::PathAuthorizationRequest{
                path, base, Domain::FileAccess::Read, false},
            context);
        if (!readAuthorization) {
            if (readAuthorization.error().code == Domain::ErrorCodes::RecordNotFound) {
                return std::nullopt;
            }
            fail(std::move(readAuthorization).error());
        }
        auto result = atomicFileStore->read(
            readAuthorization.value(), maximumBytes, context);
        if (!result) {
            if (result.error().code == Domain::ErrorCodes::RecordNotFound) {
                return std::nullopt;
            }
            fail(std::move(result).error());
        }
        return textOf(result.value());
    }

    [[nodiscard]] std::optional<std::uint64_t> packetSequence(
        const Domain::LegacyHandoffId& id,
        const Domain::OperationContext& context)
    {
        const auto marker = childPath(
            handoffsRoot, "." + id.value() + ".sequence");
        const auto encoded = read(
            marker, memoryRoot, MaximumSequenceMarkerBytes, context);
        if (!encoded) return std::nullopt;
        std::uint64_t value{};
        const auto parsed = std::from_chars(
            encoded->data(), encoded->data() + encoded->size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != encoded->data() + encoded->size() || value == 0U) {
            fail(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A packet projection sequence marker is invalid."));
        }
        return value;
    }

    [[nodiscard]] std::optional<LatestMarker> latestMarker(
        const Domain::OperationContext& context)
    {
        const auto path = childPath(memoryRoot, ".LATEST.sequence");
        auto encoded = read(path, memoryRoot, MaximumSequenceMarkerBytes, context);
        return encoded
            ? std::optional<LatestMarker>{decodeLatestMarker(*encoded)}
            : std::nullopt;
    }

    void writePacket(
        const Domain::LegacyContinuityRecord& record,
        const bool force,
        const Domain::OperationContext& context)
    {
        const auto packetPath = childPath(
            handoffsRoot, record.packet.id.value() + ".json");
        const auto markerPath = childPath(
            handoffsRoot, "." + record.packet.id.value() + ".sequence");
        const auto previous = packetSequence(record.packet.id, context);
        if (!force && previous && *previous > record.writeSequence) return;
        const auto* document = record.documents.payloadJson
            ? &*record.documents.payloadJson
            : (record.documents.packetJson ? &*record.documents.packetJson : nullptr);
        if (!document) {
            fail(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A continuity record has no document to project."));
        }
        // Publish the sequence first. A crash can leave an old/missing document,
        // but it can never permit a lower sequence to replace a newer one; the
        // startup repair replays the authoritative row.
        replace(
            markerPath, memoryRoot,
            std::to_string(record.writeSequence), context);
        replace(packetPath, memoryRoot, *document, context);
    }

    [[nodiscard]] bool writeLatest(
        const Domain::LegacyContinuityRecord& record,
        const bool force,
        const Domain::OperationContext& context)
    {
        const LatestMarker candidate{
            record.writeSequence, record.packet.id.value()};
        const auto previous = latestMarker(context);
        if (!force && previous && !isNewer(candidate, *previous) &&
            !(candidate.sequence == previous->sequence &&
              candidate.id == previous->id)) {
            return false;
        }
        const auto markerPath = childPath(memoryRoot, ".LATEST.sequence");
        const auto latestPath = childPath(handoffsRoot, "LATEST");
        const auto currentPath = childPath(memoryRoot, "current-task.md");
        replace(markerPath, memoryRoot, encodeLatestMarker(candidate), context);
        replace(latestPath, memoryRoot, record.packet.id.value(), context);
        const auto readable = markdown(record.packet);
        replace(currentPath, memoryRoot, readable, context);
        return true;
    }

    [[nodiscard]] std::size_t removePath(
        const Domain::PathText& path,
        const Domain::PathText& base,
        const Domain::OperationContext& context)
    {
        auto deletion = workspaceAuthority->authorize(
            authority,
            Domain::PathAuthorizationRequest{
                path, base, Domain::FileAccess::Delete, false},
            context);
        if (!deletion) {
            if (deletion.error().code == Domain::ErrorCodes::RecordNotFound) return 0U;
            fail(std::move(deletion).error());
        }
        auto removed = fileSystem->remove(deletion.value(), false, context);
        if (!removed) {
            if (removed.error().code == Domain::ErrorCodes::RecordNotFound) return 0U;
            fail(std::move(removed).error());
        }
        return 1U;
    }

    const Domain::PathText memoryRoot;
    const Domain::PathText handoffsRoot;
    const Contracts::WorkspaceAuthority authority;
    const std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority;
    const std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore;
    const std::shared_ptr<Contracts::IFileSystem> fileSystem;
    const std::shared_ptr<Contracts::IClock> clock;
    HANDLE crossProcessMutex{};
    std::mutex admission;
    std::atomic_bool closed{};
};

WindowsLegacyContinuityProjectionStore::WindowsLegacyContinuityProjectionStore(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsLegacyContinuityProjectionStore::~WindowsLegacyContinuityProjectionStore() noexcept
{
    close();
}

Domain::Result<std::shared_ptr<WindowsLegacyContinuityProjectionStore>>
WindowsLegacyContinuityProjectionStore::create(
    Domain::PathText memoryRoot,
    Domain::PathText handoffsRoot,
    Contracts::WorkspaceAuthority authority,
    std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority,
    std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore,
    std::shared_ptr<Contracts::IFileSystem> fileSystem,
    std::shared_ptr<Contracts::IClock> clock) noexcept
{
    try {
        if (!workspaceAuthority || !atomicFileStore || !fileSystem || !clock) {
            return Domain::Result<
                std::shared_ptr<WindowsLegacyContinuityProjectionStore>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The continuity projection store has missing dependencies."));
        }
        const bool memoryTrusted = std::find(
            authority.trustedRoots().begin(), authority.trustedRoots().end(),
            memoryRoot) != authority.trustedRoots().end();
        if (!memoryTrusted ||
            !handoffsRoot.value().starts_with(memoryRoot.value())) {
            return Domain::Result<
                std::shared_ptr<WindowsLegacyContinuityProjectionStore>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "The continuity projection roots are outside the supplied authority."));
        }
        HANDLE mutex = ::CreateMutexW(nullptr, FALSE, ProjectionMutexName);
        if (!mutex) {
            return Domain::Result<
                std::shared_ptr<WindowsLegacyContinuityProjectionStore>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The continuity projection mutex could not be created."));
        }
        auto implementation = std::make_unique<Impl>(
            std::move(memoryRoot), std::move(handoffsRoot),
            std::move(authority), std::move(workspaceAuthority),
            std::move(atomicFileStore), std::move(fileSystem),
            std::move(clock), mutex);
        return Domain::Result<
            std::shared_ptr<WindowsLegacyContinuityProjectionStore>>::success(
            std::shared_ptr<WindowsLegacyContinuityProjectionStore>{
                new WindowsLegacyContinuityProjectionStore{
                    std::move(implementation)}});
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<WindowsLegacyContinuityProjectionStore>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The continuity projection store could not be constructed."));
    }
}

Domain::Result<Domain::LegacyContinuityProjectionReceipt>
WindowsLegacyContinuityProjectionStore::write(
    const Domain::LegacyContinuityRecord& record,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyContinuityProjectionReceipt>([&]() {
        take(Domain::validateLegacyContinuityRecord(record));
        if (!implementation_) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The continuity projection store has no implementation."));
        }
        std::lock_guard admission{implementation_->admission};
        auto lease = implementation_->acquire(context);
        implementation_->writePacket(record, false, context);
        static_cast<void>(implementation_->writeLatest(record, false, context));
        return Domain::LegacyContinuityProjectionReceipt{
            childPath(
                implementation_->handoffsRoot,
                record.packet.id.value() + ".json").value(),
            childPath(
                implementation_->memoryRoot, "current-task.md").value()};
    });
}

Domain::Result<Domain::LegacyContinuityProjectionRepairOutcome>
WindowsLegacyContinuityProjectionStore::repair(
    const std::vector<Domain::LegacyContinuityRecord>& records,
    const Domain::LegacyContinuityPointerRepairOutcome& pointers,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyContinuityProjectionRepairOutcome>([&]() {
        take(Domain::validateLegacyContinuityList(
            records, Domain::LegacyContinuityLimits::MaximumRepairRows));
        if (!implementation_) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The continuity projection store has no implementation."));
        }
        std::lock_guard admission{implementation_->admission};
        auto lease = implementation_->acquire(context);
        for (const auto& record : records) {
            implementation_->writePacket(record, true, context);
        }
        bool latestWritten{};
        bool currentWritten{};
        if (pointers.latestId) {
            const auto found = std::find_if(
                records.begin(), records.end(), [&](const auto& record) {
                    return record.packet.id == *pointers.latestId;
                });
            if (found == records.end()) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The repaired latest pointer does not name a supplied row."));
            }
            latestWritten = implementation_->writeLatest(*found, true, context);
            currentWritten = latestWritten;
        }
        if (pointers.resumeReadyId) {
            const auto found = std::find_if(
                records.begin(), records.end(), [&](const auto& record) {
                    return record.packet.id == *pointers.resumeReadyId &&
                           record.packet.resumeReady;
                });
            if (found == records.end()) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The repaired resume pointer does not name a resume-ready row."));
            }
        }
        return Domain::LegacyContinuityProjectionRepairOutcome{
            records.size(), latestWritten, currentWritten};
    });
}

Domain::Result<std::size_t> WindowsLegacyContinuityProjectionStore::reset(
    const Domain::DestructiveConfirmation& confirmation,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::size_t>([&]() {
        take(Domain::validateDestructiveConfirmation(
            confirmation, ResetAction, ResetScope, ResetToken));
        if (!implementation_) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The continuity projection store has no implementation."));
        }
        std::lock_guard admission{implementation_->admission};
        auto lease = implementation_->acquire(context);
        auto directory = implementation_->authorize(
            implementation_->handoffsRoot,
            implementation_->memoryRoot,
            Domain::FileAccess::Read,
            context);
        auto listing = take(implementation_->fileSystem->list(
            directory,
            Domain::LegacyContinuityLimits::MaximumRepairRows + 8U,
            context));
        if (listing.truncated) {
            fail(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The continuity projection directory exceeded its reset bound."));
        }
        std::size_t removed{};
        for (const auto& entry : listing.entries) {
            removed += implementation_->removePath(
                entry, implementation_->memoryRoot, context);
        }
        for (const auto name : {".LATEST.sequence", "current-task.md"}) {
            removed += implementation_->removePath(
                childPath(implementation_->memoryRoot, name),
                implementation_->memoryRoot,
                context);
        }
        return removed;
    });
}

void WindowsLegacyContinuityProjectionStore::close() noexcept
{
    try {
        if (!implementation_) return;
        std::lock_guard admission{implementation_->admission};
        implementation_->closed.store(true, std::memory_order_release);
    } catch (...) {
        // noexcept shutdown; the pimpl destructor retains native RAII cleanup.
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
