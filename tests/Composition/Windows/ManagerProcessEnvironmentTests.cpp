#include "ManagerProcessEnvironment.h"

#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Composition = ForgeConductor::Composition::Windows;
namespace Domain = ForgeConductor::Domain;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;

constexpr std::uint64_t GiB = 1'073'741'824ULL;

static_assert(std::is_final_v<Composition::ManagerProcessEnvironment>);
static_assert(std::is_final_v<Composition::ManagerProcessEnvironmentSnapshot>);
static_assert(std::is_final_v<
              Composition::PreparedManagerProcessEnvironment>);
static_assert(std::is_final_v<
              Composition::WindowsManagerProcessEnvironmentPlatformProbe>);
static_assert(std::is_abstract_v<
              Composition::IManagerProcessEnvironmentPlatformProbe>);
static_assert(!std::is_copy_constructible_v<
              Composition::ManagerProcessEnvironment>);
static_assert(!std::is_move_constructible_v<
              Composition::ManagerProcessEnvironment>);
static_assert(!std::is_copy_constructible_v<
              Composition::PreparedManagerProcessEnvironment>);
static_assert(std::is_move_constructible_v<
              Composition::PreparedManagerProcessEnvironment>);

void require(
    const bool condition,
    const std::string_view message,
    const std::source_location location = std::source_location::current())
{
    if (!condition) {
        throw std::runtime_error{
            std::string{message} + " at " + location.file_name() + ':' +
            std::to_string(location.line())};
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view expectedCode,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == expectedCode, message);
}

template <typename Identifier>
[[nodiscard]] Identifier parse(const std::string_view value)
{
    return take(Identifier::parse(value));
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::OperationContext activeContext()
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "88888888-8888-4888-8888-888888888888"),
        std::chrono::steady_clock::now() + std::chrono::minutes{2},
        {},
        parse<Domain::CorrelationId>("manager-process-environment")};
}

[[nodiscard]] std::string processLabel(const std::string_view prefix)
{
    return std::string{prefix} + '-' +
        std::to_string(::GetCurrentProcessId());
}

[[nodiscard]] Domain::PathText dataRoot()
{
    return path("C:\\ForgeConductor-P16-Data-" +
                std::to_string(::GetCurrentProcessId()));
}

[[nodiscard]] Domain::PathText managerImage()
{
    return path("C:\\ForgeConductor-P16-Binaries-" +
                std::to_string(::GetCurrentProcessId()) +
                "\\ForgeConductor.Manager.exe");
}

[[nodiscard]] Domain::PathText cliImage()
{
    return path("C:\\ForgeConductor-P16-Binaries-" +
                std::to_string(::GetCurrentProcessId()) +
                "\\forge-conductor.exe");
}

[[nodiscard]] Composition::ManagerProcessExecutableIdentity makeExecutableIdentity(
    Domain::PathText executable,
    const unsigned char seed)
{
    std::array<std::byte, 16U> identifier{};
    identifier.fill(static_cast<std::byte>(seed));
    return Composition::ManagerProcessExecutableIdentity{
        std::move(executable),
        static_cast<std::uint64_t>(100U + seed),
        identifier};
}

class RecordingPlatformProbe final
    : public Composition::IManagerProcessEnvironmentPlatformProbe {
public:
    explicit RecordingPlatformProbe(
        const std::uint64_t physicalMemoryBytes = 16U * GiB)
        : platformSnapshot_{
              makeExecutableIdentity(managerImage(), 0x11U),
              physicalMemoryBytes},
          cliIdentity_{makeExecutableIdentity(cliImage(), 0x22U)}
    {
    }

    [[nodiscard]] Domain::Result<Composition::ManagerProcessPlatformSnapshot>
    inspect(const Domain::OperationContext&) noexcept override
    {
        try {
            ++inspectCalls;
            auto snapshot = platformSnapshot_;
            if (driftManagerIdentityOnSecondInspection && inspectCalls >= 2U) {
                snapshot.currentManagerImage.fileIdentifier[0] =
                    static_cast<std::byte>(0x7fU);
            }
            return Domain::Result<
                Composition::ManagerProcessPlatformSnapshot>::success(
                std::move(snapshot));
        } catch (...) {
            return Domain::Result<
                Composition::ManagerProcessPlatformSnapshot>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The recording platform inspection failed."));
        }
    }

    [[nodiscard]] Domain::Result<
        Composition::ManagerProcessExecutableIdentity>
    executableIdentity(
        const Domain::PathText& executable,
        const Domain::OperationContext&) noexcept override
    {
        try {
            ++executableIdentityCalls;
            executableRequests.push_back(executable);
            if (executable != cliIdentity_.canonicalPath) {
                return Domain::Result<
                    Composition::ManagerProcessExecutableIdentity>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The environment requested an unexpected executable identity."));
            }
            return Domain::Result<
                Composition::ManagerProcessExecutableIdentity>::success(
                cliIdentity_);
        } catch (...) {
            return Domain::Result<
                Composition::ManagerProcessExecutableIdentity>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The recording executable identity probe failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> ensureRegularDirectory(
        const Domain::PathText& directory,
        const Domain::OperationContext&) noexcept override
    {
        try {
            const std::size_t index = preparedDirectories.size();
            preparedDirectories.push_back(directory);
            if (directoryFailureIndex &&
                index == directoryFailureIndex.value()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::StorageFull,
                    "The recording directory preparation failed."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The recording directory preparation could not retain evidence."));
        }
    }

    void setPhysicalMemoryBytes(const std::uint64_t value) noexcept
    {
        platformSnapshot_.physicalMemoryBytes = value;
    }

    void setCliIdentity(
        Composition::ManagerProcessExecutableIdentity identity) noexcept
    {
        cliIdentity_ = std::move(identity);
    }

    std::size_t inspectCalls{};
    std::size_t executableIdentityCalls{};
    bool driftManagerIdentityOnSecondInspection{};
    std::optional<std::size_t> directoryFailureIndex;
    std::vector<Domain::PathText> executableRequests;
    std::vector<Domain::PathText> preparedDirectories;

private:
    Composition::ManagerProcessPlatformSnapshot platformSnapshot_;
    Composition::ManagerProcessExecutableIdentity cliIdentity_;
};

[[nodiscard]] Composition::ManagerProcessEnvironment environment(
    RecordingPlatformProbe& probe)
{
    return Composition::ManagerProcessEnvironment{
        Composition::ManagerProcessEnvironmentOptions{dataRoot(), false},
        probe};
}

[[nodiscard]] Infrastructure::WindowsManagerInstanceLeaseOptions leaseOptions(
    const std::string_view label)
{
    return Infrastructure::WindowsManagerInstanceLeaseOptions{
        processLabel(label)};
}

[[nodiscard]] Infrastructure::WindowsManagerInstanceLease acquireLease(
    const Infrastructure::WindowsManagerInstanceLeaseOptions& options)
{
    const auto identity = take(Infrastructure::WindowsCurrentUserIdentity::load());
    return take(Infrastructure::WindowsManagerInstanceLease::acquire(
        identity, options));
}

void resolvesExactRootsImagesAndResourceProfileWithoutMutation()
{
    const Composition::ManagerProcessEnvironmentOptions defaults{};
    require(
        !defaults.explicitDataRoot && !defaults.allowEnvironmentOverride,
        "production environment options did not default to the canonical data root policy");

    RecordingPlatformProbe probe;
    auto subject = environment(probe);
    const auto snapshot = take(subject.inspect(activeContext()));
    const std::string root = dataRoot().value();

    require(
        subject.options().explicitDataRoot == dataRoot() &&
            !subject.options().allowEnvironmentOverride,
        "the immutable environment options changed");
    require(snapshot.dataRoot().value() == root, "the explicit data root changed");
    require(
        snapshot.configurationRoot().value() == root + "\\config",
        "the configuration root changed");
    require(
        snapshot.diagnosticsRoot().value() == root + "\\logs",
        "the diagnostics root changed");
    require(
        snapshot.exportRoot().value() == root + "\\exports",
        "the export root changed");
    require(
        snapshot.projectsRoot().value() == root + "\\projects",
        "the projects root changed");
    require(
        snapshot.managerExecutable() == managerImage(),
        "the current Manager executable changed");
    require(
        snapshot.cliExecutable() == cliImage(),
        "the sibling Forge Conductor executable changed");
    require(
        snapshot.physicalMemoryBytes() == 16U * GiB &&
            snapshot.resourceProfile() ==
                Domain::ResourceProfile::Standard16GiB,
        "physical memory did not select the standard resource profile");
    require(
        snapshot.resourceBudgets() == Domain::budgetsForProfile(
                                          Domain::ResourceProfile::Standard16GiB),
        "the selected resource budgets changed");
    require(
        probe.inspectCalls == 1U && probe.executableIdentityCalls == 1U,
        "inspection did not use one bounded platform snapshot and sibling identity probe");
    require(
        probe.executableRequests.size() == 1U &&
            probe.executableRequests.front() == cliImage(),
        "inspection requested a non-sibling executable identity");
    require(
        probe.preparedDirectories.empty(),
        "read-only inspection created a Manager process directory");
}

void selectsAllPhysicalMemoryProfilesAtExactThresholds()
{
    struct ProfileCase final {
        std::uint64_t bytes;
        Domain::ResourceProfile profile;
    };
    constexpr std::array<ProfileCase, 3U> cases{
        ProfileCase{8U * GiB, Domain::ResourceProfile::Constrained8GiB},
        ProfileCase{16U * GiB, Domain::ResourceProfile::Standard16GiB},
        ProfileCase{32U * GiB, Domain::ResourceProfile::Expanded32GiBPlus}};

    RecordingPlatformProbe probe;
    auto subject = environment(probe);
    for (const auto& profileCase : cases) {
        probe.setPhysicalMemoryBytes(profileCase.bytes);
        const auto snapshot = take(subject.inspect(activeContext()));
        require(
            snapshot.resourceProfile() == profileCase.profile,
            "physical memory selected the wrong resource profile");
        require(
            snapshot.resourceBudgets() ==
                Domain::budgetsForProfile(profileCase.profile),
            "the resource profile selected the wrong budgets");
    }
    require(
        probe.preparedDirectories.empty(),
        "resource profile inspection mutated the filesystem");
}

void preparationRequiresAndRetainsTheLiveLease()
{
    RecordingPlatformProbe probe;
    auto subject = environment(probe);
    const auto snapshot = take(subject.inspect(activeContext()));
    const auto options = leaseOptions("environment-owned");
    auto movedFrom = acquireLease(options);
    auto owned = std::move(movedFrom);

    requireError(
        subject.prepareAfterLease(
            snapshot, std::move(movedFrom), activeContext()),
        Domain::ErrorCodes::OwnershipConflict,
        "directory preparation accepted a moved-from Manager lease");
    require(
        probe.inspectCalls == 1U && probe.preparedDirectories.empty(),
        "unowned preparation re-inspected or mutated the environment");

    {
        auto prepared = take(subject.prepareAfterLease(
            snapshot, std::move(owned), activeContext()));
        require(prepared.lease().owns(), "the prepared environment lost its lease");
        require(
            prepared.snapshot() == snapshot,
            "the prepared environment changed its immutable snapshot");
        require(
            probe.inspectCalls == 2U && probe.executableIdentityCalls == 2U,
            "preparation did not re-resolve roots and executable identities");

        const std::array<std::string, 5U> expectedDirectories{
            snapshot.dataRoot().value(),
            snapshot.configurationRoot().value(),
            snapshot.diagnosticsRoot().value(),
            snapshot.exportRoot().value(),
            snapshot.projectsRoot().value()};
        require(
            probe.preparedDirectories.size() == expectedDirectories.size(),
            "preparation did not create the exact required directory count");
        for (std::size_t index = 0U; index < expectedDirectories.size(); ++index) {
            require(
                probe.preparedDirectories[index].value() ==
                    expectedDirectories[index],
                "preparation changed the exact required directory order");
        }

        const auto identity =
            take(Infrastructure::WindowsCurrentUserIdentity::load());
        requireError(
            Infrastructure::WindowsManagerInstanceLease::acquire(
                identity, options),
            Domain::ErrorCodes::OwnershipConflict,
            "the prepared environment did not retain sole Manager ownership");
    }

    auto reacquired = acquireLease(options);
    require(
        reacquired.owns(),
        "destroying the prepared environment did not release its Manager lease");
}

void executableDriftFailsBeforeDirectoryMutationAndReleasesLease()
{
    RecordingPlatformProbe probe;
    auto subject = environment(probe);
    const auto snapshot = take(subject.inspect(activeContext()));
    probe.driftManagerIdentityOnSecondInspection = true;
    const auto options = leaseOptions("environment-drift");

    requireError(
        subject.prepareAfterLease(
            snapshot, acquireLease(options), activeContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "preparation accepted a replaced Manager executable identity");
    require(
        probe.inspectCalls == 2U && probe.executableIdentityCalls == 2U,
        "drift rejection did not revalidate both executable identities");
    require(
        probe.preparedDirectories.empty(),
        "executable drift was detected after directory mutation");

    auto reacquired = acquireLease(options);
    require(
        reacquired.owns(),
        "failed environment revalidation retained the transferred lease");
}

void directoryFailureStopsTheExactSequenceAndReleasesLease()
{
    RecordingPlatformProbe probe;
    auto subject = environment(probe);
    const auto snapshot = take(subject.inspect(activeContext()));
    probe.directoryFailureIndex = 2U;
    const auto options = leaseOptions("environment-directory-failure");

    requireError(
        subject.prepareAfterLease(
            snapshot, acquireLease(options), activeContext()),
        Domain::ErrorCodes::StorageFull,
        "directory preparation suppressed its typed platform failure");
    require(
        probe.preparedDirectories.size() == 3U,
        "directory preparation continued after its first strict failure");
    require(
        probe.preparedDirectories[0] == snapshot.dataRoot() &&
            probe.preparedDirectories[1] == snapshot.configurationRoot() &&
            probe.preparedDirectories[2] == snapshot.diagnosticsRoot(),
        "directory preparation did not stop in the exact required order");

    auto reacquired = acquireLease(options);
    require(
        reacquired.owns(),
        "failed directory preparation retained the transferred lease");
}

void invalidPlatformEvidenceAndCancellationFailStrictly()
{
    RecordingPlatformProbe zeroMemoryProbe{0U};
    auto zeroMemorySubject = environment(zeroMemoryProbe);
    requireError(
        zeroMemorySubject.inspect(activeContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "zero physical memory selected a resource profile");
    require(
        zeroMemoryProbe.executableIdentityCalls == 0U &&
            zeroMemoryProbe.preparedDirectories.empty(),
        "invalid physical-memory evidence advanced to executable or directory work");

    RecordingPlatformProbe wrongSiblingProbe;
    wrongSiblingProbe.setCliIdentity(makeExecutableIdentity(
        path("C:\\Other-Forge-Conductor\\forge-conductor.exe"), 0x33U));
    auto wrongSiblingSubject = environment(wrongSiblingProbe);
    requireError(
        wrongSiblingSubject.inspect(activeContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "a non-sibling Forge Conductor executable was accepted");

    RecordingPlatformProbe cancelledProbe;
    auto cancelledSubject = environment(cancelledProbe);
    std::stop_source cancellation;
    cancellation.request_stop();
    Domain::OperationContext cancelled{
        parse<Domain::OperationId>(
            "99999999-9999-4999-8999-999999999999"),
        std::chrono::steady_clock::now() + std::chrono::minutes{2},
        cancellation.get_token(),
        parse<Domain::CorrelationId>("manager-environment-cancelled")};
    requireError(
        cancelledSubject.inspect(cancelled),
        Domain::ErrorCodes::Cancelled,
        "environment inspection ignored cancellation");
    require(
        cancelledProbe.inspectCalls == 0U,
        "cancelled environment inspection reached the platform probe");
}

} // namespace

int main()
{
    try {
        resolvesExactRootsImagesAndResourceProfileWithoutMutation();
        selectsAllPhysicalMemoryProfilesAtExactThresholds();
        preparationRequiresAndRetainsTheLiveLease();
        executableDriftFailsBeforeDirectoryMutationAndReleasesLease();
        directoryFailureStopsTheExactSequenceAndReleasesLease();
        invalidPlatformEvidenceAndCancellationFailStrictly();
        std::cout << "Manager process environment tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager process environment tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager process environment tests failed with an unknown error.\n";
        return 1;
    }
}
