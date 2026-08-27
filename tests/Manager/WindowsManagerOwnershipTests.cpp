#include "Infrastructure/TestSupport.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::WindowsCurrentUserIdentity;
using Infrastructure::Windows::WindowsManagerInstanceLease;
using Infrastructure::Windows::WindowsManagerInstanceNames;
using Infrastructure::Windows::WindowsManagerInstanceLeaseOptions;

static_assert(std::is_final_v<WindowsCurrentUserIdentity>);
static_assert(std::is_final_v<WindowsManagerInstanceLease>);
static_assert(std::is_final_v<WindowsManagerInstanceNames>);
static_assert(!std::is_copy_constructible_v<WindowsManagerInstanceLease>);
static_assert(!std::is_copy_assignable_v<WindowsManagerInstanceLease>);
static_assert(std::is_nothrow_move_constructible_v<WindowsManagerInstanceLease>);
static_assert(std::is_nothrow_move_assignable_v<WindowsManagerInstanceLease>);

[[nodiscard]] WindowsManagerInstanceLeaseOptions options(
    const std::string_view label)
{
    return WindowsManagerInstanceLeaseOptions{
        std::string{label} + "-" + std::to_string(::GetCurrentProcessId())};
}

[[nodiscard]] bool isLowerHex(const std::string_view value) noexcept
{
    return std::ranges::all_of(value, [](const char character) noexcept {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f');
    });
}

void currentUserIdentityIsStableAndCanonical()
{
    const WindowsCurrentUserIdentity first =
        take(WindowsCurrentUserIdentity::load());
    const WindowsCurrentUserIdentity second =
        take(WindowsCurrentUserIdentity::load());

    require(first.sidText() == second.sidText(),
            "reloading the current identity changed its canonical SID text");
    require(first.stableKey() == second.stableKey(),
            "reloading the current identity changed its stable key");
    require(std::ranges::equal(first.sidBytes(), second.sidBytes()),
            "reloading the current identity changed its exact SID bytes");

    require(first.sidText().starts_with("S-1-"),
            "the current identity did not expose canonical SID text");
    require(first.stableKey().size() ==
                WindowsCurrentUserIdentity::StableKeyCharacters,
            "the current identity stable key was not a SHA-256 digest");
    require(isLowerHex(first.stableKey()),
            "the current identity stable key was not lowercase hexadecimal");

    const std::span<const std::byte> sidBytes = first.sidBytes();
    require(!sidBytes.empty() &&
                sidBytes.size() <= WindowsCurrentUserIdentity::MaximumSidBytes,
            "the current identity SID bytes exceeded their bound");
    const PSID sid = reinterpret_cast<PSID>(
        const_cast<std::byte*>(sidBytes.data()));
    require(::IsValidSid(sid) != FALSE,
            "the current identity did not retain a valid SID");
    require(::GetLengthSid(sid) == static_cast<DWORD>(sidBytes.size()),
            "the current identity did not retain the exact SID byte length");
}

void firstOwnerRejectsSecondOwner()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    const auto leaseOptions = options("owner-conflict");
    auto first = take(WindowsManagerInstanceLease::acquire(identity, leaseOptions));
    require(first.owns(), "the first manager instance did not retain its lease handle");

    const auto second =
        WindowsManagerInstanceLease::acquire(identity, leaseOptions);
    requireError(second, Domain::ErrorCodes::OwnershipConflict,
                 "a second manager instance acquired the same current-user mutex");
}

void releasedLeaseCanBeReacquired()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    const auto leaseOptions = options("release-reacquire");
    {
        auto first = take(
            WindowsManagerInstanceLease::acquire(identity, leaseOptions));
        require(first.owns(), "the scoped manager lease was not owned");
    }

    auto second = take(
        WindowsManagerInstanceLease::acquire(identity, leaseOptions));
    require(second.owns(),
            "a manager lease could not be reacquired after its handle closed");
}

void distinctPurposeSuffixesAreIsolated()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    auto first = take(WindowsManagerInstanceLease::acquire(
        identity, options("suffix-isolation-a")));
    auto second = take(WindowsManagerInstanceLease::acquire(
        identity, options("suffix-isolation-b")));

    require(first.owns() && second.owns(),
            "distinct purpose suffixes did not isolate manager leases");
    require(first.mutexName() != second.mutexName(),
            "distinct purpose suffixes produced the same mutex name");
    require(first.pipeName() != second.pipeName(),
            "distinct purpose suffixes produced the same pipe name");
}

void namesCanBeDerivedWithoutTakingOwnership()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    const auto leaseOptions = options("name-derivation");
    const auto firstNames = take(
        WindowsManagerInstanceLease::namesFor(identity, leaseOptions));
    const auto secondNames = take(
        WindowsManagerInstanceLease::namesFor(identity, leaseOptions));

    require(firstNames.mutexName() == secondNames.mutexName() &&
                firstNames.pipeName() == secondNames.pipeName(),
            "repeated manager endpoint-name derivation was not deterministic");

    auto lease = take(
        WindowsManagerInstanceLease::acquire(identity, leaseOptions));
    require(lease.owns(),
            "deriving manager endpoint names unexpectedly acquired ownership");
    require(lease.mutexName() == firstNames.mutexName() &&
                lease.pipeName() == firstNames.pipeName(),
            "the manager lease did not reuse the canonical derived names");
}

void moveTransfersSoleOwnership()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    const auto sourceOptions = options("move-source");
    const auto destinationOptions = options("move-destination");
    auto source = take(
        WindowsManagerInstanceLease::acquire(identity, sourceOptions));
    const std::wstring sourceMutex{source.mutexName()};

    auto moved = std::move(source);
    require(!source.owns() && source.mutexName().empty() && source.pipeName().empty(),
            "a moved-from manager lease retained logical ownership");
    require(moved.owns() && moved.mutexName() == sourceMutex,
            "manager lease move construction did not transfer ownership");

    auto destination = take(
        WindowsManagerInstanceLease::acquire(identity, destinationOptions));
    destination = std::move(moved);
    require(!moved.owns() && destination.owns() &&
                destination.mutexName() == sourceMutex,
            "manager lease move assignment did not transfer sole ownership");

    auto releasedDestination = take(
        WindowsManagerInstanceLease::acquire(identity, destinationOptions));
    require(releasedDestination.owns(),
            "move assignment did not close the destination's prior lease first");
    requireError(
        WindowsManagerInstanceLease::acquire(identity, sourceOptions),
        Domain::ErrorCodes::OwnershipConflict,
        "move assignment released the transferred manager mutex");
}

void namesAreBoundedAndSuffixesAreValidated()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    const auto canonicalOptions = options("name-shape");
    const auto canonicalNames = take(
        WindowsManagerInstanceLease::namesFor(identity, canonicalOptions));
    auto canonical = take(WindowsManagerInstanceLease::acquire(
        identity, canonicalOptions));
    const std::wstring canonicalSuffix{
        canonicalOptions.purposeSuffix.begin(),
        canonicalOptions.purposeSuffix.end()};
    const std::wstring expectedMutex =
        L"Global\\ForgeConductor.Manager.v1." +
        std::wstring{identity.stableKey().begin(), identity.stableKey().end()} +
        L'.' + canonicalSuffix;
    const std::wstring expectedPipe =
        L"\\\\.\\pipe\\ForgeConductor.Manager.v1." +
        std::wstring{identity.stableKey().begin(), identity.stableKey().end()} +
        L'.' + canonicalSuffix;
    require(canonicalNames.mutexName() == expectedMutex &&
                canonical.mutexName() == canonicalNames.mutexName(),
            "the manager mutex name was not canonical");
    require(canonicalNames.pipeName() == expectedPipe &&
                canonical.pipeName() == canonicalNames.pipeName(),
            "the manager pipe name was not canonical");
    require(canonical.mutexName().size() <=
                WindowsManagerInstanceLease::MaximumMutexNameCharacters,
            "the manager mutex name exceeded its bound");
    require(canonical.mutexName().starts_with(L"Global\\"),
            "the manager mutex was scoped to one interactive session");
    require(canonical.pipeName().size() <=
                WindowsManagerInstanceLease::MaximumPipeNameCharacters,
            "the manager pipe name exceeded its bound");

    std::string maximumSuffix =
        "max-" + std::to_string(::GetCurrentProcessId()) + '-';
    maximumSuffix.append(
        WindowsManagerInstanceLease::MaximumPurposeSuffixCharacters -
            maximumSuffix.size(),
        'z');
    auto bounded = take(WindowsManagerInstanceLease::acquire(
        identity, WindowsManagerInstanceLeaseOptions{maximumSuffix}));
    require(bounded.mutexName().ends_with(
                std::wstring{L'.'} +
                std::wstring{maximumSuffix.begin(), maximumSuffix.end()}),
            "the maximum safe manager purpose suffix was changed");
    require(bounded.mutexName().size() <=
                WindowsManagerInstanceLease::MaximumMutexNameCharacters &&
                bounded.pipeName().size() <=
                    WindowsManagerInstanceLease::MaximumPipeNameCharacters,
            "the maximum safe suffix produced an unbounded object name");

    const WindowsManagerInstanceLeaseOptions overlongOptions{
        std::string(
            WindowsManagerInstanceLease::MaximumPurposeSuffixCharacters + 1U,
            'a')};
    requireError(
        WindowsManagerInstanceLease::namesFor(identity, overlongOptions),
        Domain::ErrorCodes::InvalidRequest,
        "endpoint-name derivation accepted an overlong manager purpose suffix");
    requireError(
        WindowsManagerInstanceLease::acquire(identity, overlongOptions),
        Domain::ErrorCodes::InvalidRequest,
        "an overlong manager purpose suffix was accepted");
    for (const std::string_view unsafe :
         {"contains.dot", "contains slash", "contains\\backslash", "nonascii-\xC3\xA9"}) {
        const WindowsManagerInstanceLeaseOptions unsafeOptions{
            std::string{unsafe}};
        requireError(
            WindowsManagerInstanceLease::namesFor(identity, unsafeOptions),
            Domain::ErrorCodes::InvalidRequest,
            "endpoint-name derivation accepted an unsafe manager purpose suffix");
        requireError(
            WindowsManagerInstanceLease::acquire(identity, unsafeOptions),
            Domain::ErrorCodes::InvalidRequest,
            "an unsafe manager purpose suffix was accepted");
    }
}

} // namespace

void registerWindowsManagerOwnershipTests(TestRegistry& tests)
{
    addTest(tests, "manager_ownership.current_user_identity_is_stable_and_canonical",
            currentUserIdentityIsStableAndCanonical);
    addTest(tests, "manager_ownership.first_owner_rejects_second_owner",
            firstOwnerRejectsSecondOwner);
    addTest(tests, "manager_ownership.released_lease_can_be_reacquired",
            releasedLeaseCanBeReacquired);
    addTest(tests, "manager_ownership.distinct_purpose_suffixes_are_isolated",
            distinctPurposeSuffixesAreIsolated);
    addTest(tests, "manager_ownership.names_can_be_derived_without_taking_ownership",
            namesCanBeDerivedWithoutTakingOwnership);
    addTest(tests, "manager_ownership.move_transfers_sole_ownership",
            moveTransfersSoleOwnership);
    addTest(tests, "manager_ownership.names_are_bounded_and_suffixes_are_validated",
            namesAreBoundedAndSuffixesAreValidated);
}

} // namespace ForgeConductor::Tests
