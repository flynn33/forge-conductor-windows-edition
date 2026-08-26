#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsNativeSessionLedger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;

std::atomic_size_t assertionCount{};

void require(const bool condition, const std::string_view expression)
{
    assertionCount.fetch_add(1U, std::memory_order_relaxed);
    if (!condition) {
        throw std::runtime_error{
            "Requirement failed: " + std::string{expression}};
    }
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

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

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text)
{
    std::vector<std::byte> result(text.size());
    std::transform(
        text.begin(), text.end(), result.begin(), [](const char value) {
            return static_cast<std::byte>(static_cast<unsigned char>(value));
        });
    return result;
}

[[nodiscard]] std::string text(const std::span<const std::byte> content)
{
    if (content.empty()) {
        return {};
    }
    return std::string{
        reinterpret_cast<const char*>(content.data()), content.size()};
}

class CapabilityIssuer final : public Contracts::IWorkspaceAuthority {
public:
    [[nodiscard]] static Contracts::AuthorizedPath issue(
        const Domain::PathText& root,
        const Domain::PathText& target,
        const Domain::FileAccess access)
    {
        auto authority = take(issueAuthority(
            parse<Domain::AuthorityId>(
                "10101010-1010-4010-8010-101010101010"),
            parse<Domain::ProjectId>(
                "20202020-2020-4020-8020-202020202020"),
            parse<Domain::ClientId>("native-ledger-tests"),
            std::vector<Domain::PathText>{root},
            Domain::FileAccess::Read,
            std::vector<Domain::FileAccess>{
                Domain::FileAccess::Read,
                Domain::FileAccess::Write,
                Domain::FileAccess::Create},
            {},
            false,
            1U));
        return take(issueAuthorizedPath(authority, target, root, access));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The ledger test issuer does not resolve project authority."));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority&,
        const std::vector<Domain::PathText>&,
        const std::vector<Domain::FileAccess>&,
        const bool,
        const std::uint64_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The ledger test issuer does not narrow authority."));
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority&,
        const Domain::PathAuthorizationRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::AuthorizedPath>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The ledger test issuer does not authorize paths."));
    }
};

struct LedgerPaths final {
    Domain::PathText root{take(Domain::PathText::create(
        "D:\\Forge\\NativeLedgerTests"))};
    Domain::PathText primary{take(Domain::PathText::create(
        "D:\\Forge\\NativeLedgerTests\\native-session-ledger.json"))};
    Domain::PathText backup{take(Domain::PathText::create(
        "D:\\Forge\\NativeLedgerTests\\native-session-ledger.json.bak"))};
    Contracts::AuthorizedPath read{
        CapabilityIssuer::issue(root, primary, Domain::FileAccess::Read)};
    Contracts::AuthorizedPath write{
        CapabilityIssuer::issue(root, primary, Domain::FileAccess::Write)};
    Contracts::AuthorizedPath create{
        CapabilityIssuer::issue(root, primary, Domain::FileAccess::Create)};
    Contracts::AuthorizedPath backupRead{
        CapabilityIssuer::issue(root, backup, Domain::FileAccess::Read)};
};

class MemoryAtomicFileStore final : public Contracts::IAtomicFileStore {
public:
    [[nodiscard]] Domain::Result<std::vector<std::byte>> read(
        const Contracts::AuthorizedPath& path,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++readCalls_;
            lastMaximumBytes_ = maximumBytes;
            if (context.isCancellationRequested()) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The scripted atomic read was cancelled."));
            }
            if (context.isExpired(std::chrono::steady_clock::now())) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::DeadlineExceeded,
                        "The scripted atomic read deadline expired."));
            }

            const bool backup = path.canonicalPath().value().ends_with(".bak");
            const auto& selected = backup ? backup_ : primary_;
            const bool exists = backup ? backupExists_ : primaryExists_;
            if (!exists) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::RecordNotFound,
                        "The scripted atomic file is missing."));
            }
            if (selected.size() > maximumBytes) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "The scripted atomic file exceeds the requested bound."));
            }
            return Domain::Result<std::vector<std::byte>>::success(selected);
        } catch (...) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted atomic read failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> replace(
        const Contracts::AuthorizedPath& path,
        const std::span<const std::byte> content,
        const bool retainBackup,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            if (context.isCancellationRequested()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The scripted atomic replacement was cancelled."));
            }
            if (context.isExpired(std::chrono::steady_clock::now())) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The scripted atomic replacement deadline expired."));
            }
            if (primaryExists_ &&
                path.access() == Domain::FileAccess::Create) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "Create cannot replace the existing scripted atomic file."));
            }
            if (!primaryExists_ &&
                path.access() == Domain::FileAccess::Write) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::RecordNotFound,
                    "Write cannot create the scripted atomic file."));
            }

            ++replaceCalls_;
            lastAccess_ = path.access();
            lastRetainBackup_ = retainBackup;
            if (primaryExists_ && retainBackup) {
                backup_ = primary_;
                backupExists_ = true;
            }
            primary_.assign(content.begin(), content.end());
            primaryExists_ = true;
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The scripted atomic replacement failed."));
        }
    }

    void setPrimary(std::vector<std::byte> content)
    {
        std::lock_guard lock{mutex_};
        primary_ = std::move(content);
        primaryExists_ = true;
    }

    void setBackup(std::vector<std::byte> content)
    {
        std::lock_guard lock{mutex_};
        backup_ = std::move(content);
        backupExists_ = true;
    }

    [[nodiscard]] std::vector<std::byte> primary() const
    {
        std::lock_guard lock{mutex_};
        return primary_;
    }

    [[nodiscard]] bool backupExists() const
    {
        std::lock_guard lock{mutex_};
        return backupExists_;
    }

    [[nodiscard]] std::size_t readCalls() const
    {
        std::lock_guard lock{mutex_};
        return readCalls_;
    }

    [[nodiscard]] std::size_t replaceCalls() const
    {
        std::lock_guard lock{mutex_};
        return replaceCalls_;
    }

    [[nodiscard]] std::size_t lastMaximumBytes() const
    {
        std::lock_guard lock{mutex_};
        return lastMaximumBytes_;
    }

    [[nodiscard]] std::optional<Domain::FileAccess> lastAccess() const
    {
        std::lock_guard lock{mutex_};
        return lastAccess_;
    }

    [[nodiscard]] bool lastRetainBackup() const
    {
        std::lock_guard lock{mutex_};
        return lastRetainBackup_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::byte> primary_;
    std::vector<std::byte> backup_;
    bool primaryExists_{};
    bool backupExists_{};
    std::size_t readCalls_{};
    std::size_t replaceCalls_{};
    std::size_t lastMaximumBytes_{};
    std::optional<Domain::FileAccess> lastAccess_;
    bool lastRetainBackup_{};
};

[[nodiscard]] Domain::OperationContext liveContext(
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "30303030-3030-4030-8030-303030303030"),
        std::chrono::steady_clock::now() + std::chrono::seconds{30},
        cancellation,
        parse<Domain::CorrelationId>("native-ledger-tests")};
}

[[nodiscard]] Domain::OperationContext expiredContext()
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "30303030-3030-4030-8030-303030303030"),
        std::chrono::steady_clock::now() - std::chrono::milliseconds{1},
        {},
        parse<Domain::CorrelationId>("native-ledger-tests-expired")};
}

[[nodiscard]] std::string indexedUuid(std::uint64_t index)
{
    constexpr std::array<char, 16U> Hex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string value{"40404040-4040-4040-8040-000000000000"};
    for (std::size_t offset = 0U; offset < 12U; ++offset) {
        value[value.size() - 1U - offset] =
            Hex[static_cast<std::size_t>(index & 0xFU)];
        index >>= 4U;
    }
    return value;
}

[[nodiscard]] Domain::NativeSessionRecord readyRecord(
    const std::uint64_t index)
{
    return Domain::NativeSessionRecord{
        Domain::HostSession{
            parse<Domain::SessionId>(indexedUuid(index)),
            parse<Domain::ProjectId>(
                "50505050-5050-4050-8050-505050505050"),
            parse<Domain::ContinuityOperationId>(
                "60606060-6060-4060-8060-606060606060"),
            parse<Domain::SessionId>(
                "70707070-7070-4070-8070-707070707070"),
            take(Domain::IdempotencyKey::create(
                "native-ledger-ready-" + std::to_string(index))),
            parse<Domain::ProviderSessionId>(
                "provider-session-" + std::to_string(index)),
            std::string{"forge-model-v1"},
            Domain::HostSessionStatus::Ready},
        parse<Domain::OperationId>(
            "80808080-8080-4080-8080-808080808080"),
        parse<Domain::ContinuityHandoffId>(
            "90909090-9090-4090-8090-909090909090"),
        parse<Domain::Sha256Digest>(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        100U + index,
        200U + index,
        Domain::UtcTimePoint{std::chrono::seconds{1'700'000'000}} +
            std::chrono::microseconds{123'456},
        Domain::UtcTimePoint{std::chrono::seconds{1'700'000'001}} +
            std::chrono::microseconds{654'321}};
}

[[nodiscard]] Domain::NativeSessionRecord boundedCreatingRecord(
    const std::uint64_t index)
{
    std::string key{"native-ledger-bound-" + std::to_string(index)};
    key.append(Domain::IdempotencyKey::MaximumBytes - key.size(), 'k');
    return Domain::NativeSessionRecord{
        Domain::HostSession{
            parse<Domain::SessionId>(indexedUuid(index)),
            parse<Domain::ProjectId>(
                "50505050-5050-4050-8050-505050505050"),
            parse<Domain::ContinuityOperationId>(
                "60606060-6060-4060-8060-606060606060"),
            parse<Domain::SessionId>(
                "70707070-7070-4070-8070-707070707070"),
            take(Domain::IdempotencyKey::create(key)),
            std::nullopt,
            std::string(256U, 'm'),
            Domain::HostSessionStatus::Creating},
        parse<Domain::OperationId>(
            "80808080-8080-4080-8080-808080808080"),
        std::nullopt,
        std::nullopt,
        0U,
        0U,
        Domain::UtcTimePoint{std::chrono::seconds{1'700'000'000}},
        Domain::UtcTimePoint{std::chrono::seconds{1'700'000'000}}};
}

void requireSameRecord(
    const Domain::NativeSessionRecord& actual,
    const Domain::NativeSessionRecord& expected)
{
    REQUIRE(actual.session.id == expected.session.id);
    REQUIRE(actual.session.projectId == expected.session.projectId);
    REQUIRE(actual.session.operationId == expected.session.operationId);
    REQUIRE(actual.session.predecessorSessionId ==
            expected.session.predecessorSessionId);
    REQUIRE(actual.session.idempotencyKey == expected.session.idempotencyKey);
    REQUIRE(actual.session.providerSessionId ==
            expected.session.providerSessionId);
    REQUIRE(actual.session.model == expected.session.model);
    REQUIRE(actual.session.status == expected.session.status);
    REQUIRE(actual.ownerOperationId == expected.ownerOperationId);
    REQUIRE(actual.handoffId == expected.handoffId);
    REQUIRE(actual.handoffSha256 == expected.handoffSha256);
    REQUIRE(actual.inputTokens == expected.inputTokens);
    REQUIRE(actual.outputTokens == expected.outputTokens);
    REQUIRE(actual.createdAt == expected.createdAt);
    REQUIRE(actual.updatedAt == expected.updatedAt);
}

void requireSameLedger(
    const Domain::NativeSessionLedger& actual,
    const Domain::NativeSessionLedger& expected)
{
    REQUIRE(actual.schemaVersion == expected.schemaVersion);
    REQUIRE(actual.revision == expected.revision);
    REQUIRE(actual.contentSha256 == expected.contentSha256);
    REQUIRE(actual.records.size() == expected.records.size());
    for (std::size_t index = 0U; index < actual.records.size(); ++index) {
        requireSameRecord(actual.records[index], expected.records[index]);
    }
}

void roundTripPersistsExactSnapshot()
{
    MemoryAtomicFileStore store;
    InfrastructureWindows::BCryptSha256Hasher hasher;
    LedgerPaths paths;
    Domain::NativeSessionLedger published;

    {
        InfrastructureWindows::WindowsNativeSessionLedger ledger{
            store,
            hasher,
            paths.read,
            paths.write,
            paths.create,
            paths.backupRead};
        const auto initial = take(ledger.load(liveContext()));
        REQUIRE(initial.schemaVersion ==
                InfrastructureWindows::WindowsNativeSessionLedger::SchemaVersion);
        REQUIRE(initial.revision == 0U);
        REQUIRE(initial.records.empty());
        REQUIRE(!initial.contentSha256);

        auto candidate = initial;
        candidate.records.push_back(readyRecord(1U));
        published = take(ledger.commit(candidate, initial.revision, liveContext()));
        REQUIRE(published.revision == 1U);
        REQUIRE(published.contentSha256.has_value());
        REQUIRE(Domain::validateNativeSessionLedger(published));
        REQUIRE(store.lastAccess() == Domain::FileAccess::Create);
        REQUIRE(store.lastRetainBackup());
        REQUIRE(store.replaceCalls() == 1U);
        REQUIRE(!store.backupExists());
        const auto persistedText = text(store.primary());
        REQUIRE(persistedText.starts_with("{\n"));
        REQUIRE(persistedText.ends_with("}\n"));
        REQUIRE(persistedText.find("\"schema_version\": 1") !=
                std::string::npos);
        REQUIRE(persistedText.find(published.contentSha256->value()) !=
                std::string::npos);
    }

    InfrastructureWindows::WindowsNativeSessionLedger reopened{
        store,
        hasher,
        paths.read,
        paths.write,
        paths.create,
        paths.backupRead};
    const auto reloaded = take(reopened.load(liveContext()));
    requireSameLedger(reloaded, published);
    REQUIRE(reloaded.records.front().createdAt.time_since_epoch() ==
            std::chrono::milliseconds{1'700'000'000'123LL});
    REQUIRE(reloaded.records.front().updatedAt.time_since_epoch() ==
            std::chrono::milliseconds{1'700'000'001'654LL});
    REQUIRE(store.lastMaximumBytes() ==
            InfrastructureWindows::WindowsNativeSessionLedger::MaximumDocumentBytes);
}

void revisionCasRejectsStaleWriters()
{
    MemoryAtomicFileStore store;
    InfrastructureWindows::BCryptSha256Hasher hasher;
    LedgerPaths paths;
    InfrastructureWindows::WindowsNativeSessionLedger first{
        store,
        hasher,
        paths.read,
        paths.write,
        paths.create,
        paths.backupRead};
    InfrastructureWindows::WindowsNativeSessionLedger stale{
        store,
        hasher,
        paths.read,
        paths.write,
        paths.create,
        paths.backupRead};

    auto firstCandidate = take(first.load(liveContext()));
    auto staleCandidate = take(stale.load(liveContext()));
    firstCandidate.records.push_back(readyRecord(10U));
    staleCandidate.records.push_back(readyRecord(11U));
    const auto published =
        take(first.commit(firstCandidate, firstCandidate.revision, liveContext()));
    REQUIRE(published.revision == 1U);

    const auto conflict = stale.commit(
        staleCandidate, staleCandidate.revision, liveContext());
    requireError(conflict, Domain::ErrorCodes::Conflict);
    REQUIRE(store.replaceCalls() == 1U);

    auto wrongCandidate = published;
    ++wrongCandidate.revision;
    const auto wrongRevision = first.commit(
        wrongCandidate, published.revision, liveContext());
    requireError(wrongRevision, Domain::ErrorCodes::Conflict);
    REQUIRE(store.replaceCalls() == 1U);

    InfrastructureWindows::WindowsNativeSessionLedger observer{
        store,
        hasher,
        paths.read,
        paths.write,
        paths.create,
        paths.backupRead};
    requireSameLedger(take(observer.load(liveContext())), published);
}

void corruptionRecoversExactSiblingBackup()
{
    MemoryAtomicFileStore store;
    InfrastructureWindows::BCryptSha256Hasher hasher;
    LedgerPaths paths;
    Domain::NativeSessionLedger firstPublished;
    Domain::NativeSessionLedger secondPublished;

    {
        InfrastructureWindows::WindowsNativeSessionLedger writer{
            store,
            hasher,
            paths.read,
            paths.write,
            paths.create,
            paths.backupRead};
        auto firstCandidate = take(writer.load(liveContext()));
        firstCandidate.records.push_back(readyRecord(20U));
        firstPublished = take(writer.commit(
            firstCandidate, firstCandidate.revision, liveContext()));
        auto secondCandidate = firstPublished;
        ++secondCandidate.records.front().inputTokens;
        ++secondCandidate.records.front().outputTokens;
        secondPublished = take(writer.commit(
            secondCandidate, firstPublished.revision, liveContext()));
    }

    REQUIRE(secondPublished.revision == 2U);
    REQUIRE(store.backupExists());
    REQUIRE(store.lastAccess() == Domain::FileAccess::Write);
    REQUIRE(store.replaceCalls() == 2U);

    auto corrupted = text(store.primary());
    const auto marker = corrupted.find("\"input_tokens\": 121");
    REQUIRE(marker != std::string::npos);
    const auto value = corrupted.find("121", marker);
    REQUIRE(value != std::string::npos);
    corrupted[value] = '9';
    store.setPrimary(bytes(corrupted));

    InfrastructureWindows::WindowsNativeSessionLedger recovered{
        store,
        hasher,
        paths.read,
        paths.write,
        paths.create,
        paths.backupRead};
    const auto recoveredSnapshot = take(recovered.load(liveContext()));
    requireSameLedger(recoveredSnapshot, firstPublished);
    REQUIRE(recoveredSnapshot.revision == 1U);

    store.setBackup(bytes("{malformed-backup"));
    InfrastructureWindows::WindowsNativeSessionLedger invalidPair{
        store,
        hasher,
        paths.read,
        paths.write,
        paths.create,
        paths.backupRead};
    requireError(
        invalidPair.load(liveContext()),
        Domain::ErrorCodes::IntegrityFailure);
}

void boundsFailClosedBeforePublication()
{
    {
        MemoryAtomicFileStore store;
        store.setPrimary(std::vector<std::byte>(
            InfrastructureWindows::WindowsNativeSessionLedger::
                    MaximumDocumentBytes +
                1U,
            std::byte{0x78}));
        InfrastructureWindows::BCryptSha256Hasher hasher;
        LedgerPaths paths;
        InfrastructureWindows::WindowsNativeSessionLedger ledger{
            store,
            hasher,
            paths.read,
            paths.write,
            paths.create,
            paths.backupRead};
        requireError(
            ledger.load(liveContext()),
            Domain::ErrorCodes::IntegrityFailure);
        REQUIRE(store.lastMaximumBytes() ==
                InfrastructureWindows::WindowsNativeSessionLedger::
                    MaximumDocumentBytes);
        REQUIRE(store.readCalls() == 2U);
    }

    {
        MemoryAtomicFileStore store;
        InfrastructureWindows::BCryptSha256Hasher hasher;
        LedgerPaths paths;
        InfrastructureWindows::WindowsNativeSessionLedger ledger{
            store,
            hasher,
            paths.read,
            paths.write,
            paths.create,
            paths.backupRead};
        auto tooMany = take(ledger.load(liveContext()));
        tooMany.records.assign(
            Domain::MaximumNativeSessionRecords + 1U,
            readyRecord(30U));
        requireError(
            ledger.commit(tooMany, tooMany.revision, liveContext()),
            Domain::ErrorCodes::IntegrityFailure);
        REQUIRE(store.replaceCalls() == 0U);
    }

    {
        MemoryAtomicFileStore store;
        InfrastructureWindows::BCryptSha256Hasher hasher;
        LedgerPaths paths;
        InfrastructureWindows::WindowsNativeSessionLedger ledger{
            store,
            hasher,
            paths.read,
            paths.write,
            paths.create,
            paths.backupRead};
        auto tooLarge = take(ledger.load(liveContext()));
        tooLarge.records.reserve(Domain::MaximumNativeSessionRecords);
        for (std::size_t index = 0U;
             index < Domain::MaximumNativeSessionRecords;
             ++index) {
            tooLarge.records.push_back(
                boundedCreatingRecord(static_cast<std::uint64_t>(index + 1U)));
        }
        requireError(
            ledger.commit(tooLarge, tooLarge.revision, liveContext()),
            Domain::ErrorCodes::PayloadTooLarge);
        REQUIRE(store.replaceCalls() == 0U);
    }
}

void cancellationDeadlineAndShutdownAreTerminal()
{
    MemoryAtomicFileStore store;
    InfrastructureWindows::BCryptSha256Hasher hasher;
    LedgerPaths paths;
    InfrastructureWindows::WindowsNativeSessionLedger ledger{
        store,
        hasher,
        paths.read,
        paths.write,
        paths.create,
        paths.backupRead};

    std::stop_source cancelled;
    REQUIRE(cancelled.request_stop());
    requireError(
        ledger.load(liveContext(cancelled.get_token())),
        Domain::ErrorCodes::Cancelled);
    requireError(
        ledger.load(expiredContext()),
        Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(store.readCalls() == 0U);

    auto candidate = take(ledger.load(liveContext()));
    candidate.records.push_back(readyRecord(40U));
    requireError(
        ledger.commit(
            candidate, candidate.revision, liveContext(cancelled.get_token())),
        Domain::ErrorCodes::Cancelled);
    REQUIRE(store.replaceCalls() == 0U);

    ledger.shutdown();
    ledger.shutdown();
    requireError(
        ledger.load(liveContext()),
        Domain::ErrorCodes::TransportClosed);
    requireError(
        ledger.commit(candidate, candidate.revision, liveContext()),
        Domain::ErrorCodes::TransportClosed);
    REQUIRE(store.replaceCalls() == 0U);
}

struct TestCase final {
    std::string_view name;
    void (*function)();
};

} // namespace

int main()
{
    constexpr std::array<TestCase, 5U> Tests{{
        {"native_session_ledger.round_trip", roundTripPersistsExactSnapshot},
        {"native_session_ledger.revision_cas", revisionCasRejectsStaleWriters},
        {"native_session_ledger.backup_recovery",
         corruptionRecoversExactSiblingBackup},
        {"native_session_ledger.bounds", boundsFailClosedBeforePublication},
        {"native_session_ledger.context_shutdown",
         cancellationDeadlineAndShutdownAreTerminal},
    }};

    std::size_t passed{};
    for (const auto& test : Tests) {
        try {
            test.function();
            ++passed;
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            std::cerr << "FAIL " << test.name << ": unknown exception\n";
        }
    }

    std::cout << "SUMMARY passed=" << passed
              << " failed=" << (Tests.size() - passed)
              << " assertions="
              << assertionCount.load(std::memory_order_relaxed) << '\n';
    return passed == Tests.size() ? 0 : 1;
}
