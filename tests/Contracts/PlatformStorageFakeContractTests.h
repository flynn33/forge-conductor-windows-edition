#pragma once

#include "PlatformBoundaryFakeTestSupport.h"
#include "Fakes/AtomicFileStoreFake.h"
#include "Fakes/ConfigurationStoreFake.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/FileSystemFake.h"
#include "Fakes/PlatformPathFakes.h"
#include "Fakes/SecureStorageFake.h"

#include <cstddef>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <vector>

namespace ForgeConductor::Tests {

inline void runPlatformStorageFakeContractTests()
{
    namespace Support = PlatformBoundaryTestSupport;
    namespace Fakes = ForgeConductor::Tests::Fakes;

    static_assert(std::is_final_v<Fakes::RecordingApplicationPathsFake>);
    static_assert(std::is_final_v<Fakes::RecordingAtomicFileStoreFake>);
    static_assert(std::is_final_v<Fakes::RecordingFileSystemFake>);
    static_assert(std::is_final_v<Fakes::RecordingSecureStorageFake>);
    static_assert(std::is_final_v<Fakes::RecordingConfigurationStoreFake>);

    const Domain::MonotonicTimePoint now{};
    const auto context = Support::activeContext(now);
    const auto root = Support::path("C:/platform-boundary");
    const auto file = Support::path("C:/platform-boundary/file.bin");

    Fakes::DeterministicWorkspaceAuthority issuer{
        Support::authorityId(),
        Support::clientId(),
        {root},
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write},
        {Domain::FileAccess::Delete},
        false,
        1};
    issuer.setNow(now);
    const auto authority = Support::take(
        issuer.authorityFor(Support::projectId(), context));
    const auto readPath = Support::take(issuer.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            file,
            std::optional<Domain::PathText>{root},
            Domain::FileAccess::Read,
            false},
        context));
    const auto writePath = Support::take(issuer.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            file,
            std::optional<Domain::PathText>{root},
            Domain::FileAccess::Write,
            false},
        context));

    Fakes::RecordingApplicationPathsFake paths;
    paths.setNow(now);
    paths.dataRootResult.set(
        Domain::Result<Domain::PathText>::success(root));
    paths.projectRootResult.set(
        Domain::Result<Domain::PathText>::success(root));
    Support::require(
        Support::take(paths.dataRoot(context)) == root,
        "application paths did not return the scripted root");
    Support::require(
        Support::take(paths.projectRoot(Support::projectId(), context)) == root &&
            paths.lastProjectId() &&
            paths.lastProjectId().value() == Support::projectId(),
        "application paths did not retain project binding");

    std::stop_source cancellation;
    cancellation.request_stop();
    Support::requireError(
        paths.dataRoot(
            Support::cancelledContext(now, cancellation.get_token())),
        Domain::ErrorCodes::Cancelled,
        "application paths accepted cancellation");

    Fakes::RecordingAtomicFileStoreFake atomic{4};
    atomic.setNow(now);
    atomic.readResult.set(
        Domain::Result<std::vector<std::byte>>::success(
            {std::byte{1}, std::byte{2}}));
    atomic.replaceResult.set(Domain::Result<void>::success());
    Support::require(
        Support::take(atomic.read(readPath, 4, context)).size() == 2,
        "atomic store did not return scripted bytes");
    Support::require(
        atomic.lastRead() &&
            atomic.lastRead()->path.authorityId() == authority.authorityId(),
        "atomic store did not retain authority binding");

    const std::vector<std::byte> oversized{
        std::byte{1},
        std::byte{2},
        std::byte{3},
        std::byte{4},
        std::byte{5}};
    Support::requireError(
        atomic.replace(writePath, oversized, true, context),
        Domain::ErrorCodes::PayloadTooLarge,
        "atomic store exceeded its capture bound");
    Support::require(
        atomic.lastReplace() &&
            atomic.lastReplace()->capturedContent.size() == 4 &&
            atomic.lastReplace()->requestedBytes == 5,
        "atomic store did not retain a bounded replacement capture");

    Fakes::RecordingFileSystemFake filesystem{4, 2};
    filesystem.setNow(now);
    filesystem.writeFileResult.set(Domain::Result<void>::success());
    filesystem.listResult.set(
        Domain::Result<std::vector<Domain::PathText>>::success(
            {file, file, file}));
    const std::vector<std::byte> content{std::byte{7}, std::byte{8}};
    Support::require(
        filesystem.writeFile(writePath, content, context).hasValue(),
        "filesystem rejected a bounded write");
    Support::require(
        filesystem.lastCapture() &&
            filesystem.lastCapture()->primary.authorityId() ==
                authority.authorityId() &&
            filesystem.lastCapture()->capturedContent.size() == 2,
        "filesystem did not retain bounded path/content binding");
    Support::requireError(
        filesystem.list(readPath, 8, context),
        Domain::ErrorCodes::LimitExceeded,
        "filesystem returned an over-cap directory listing");

    Fakes::RecordingSecureStorageFake secure{4, 16};
    secure.setNow(now);
    secure.putResult.set(Domain::Result<void>::success());
    secure.getResult.set(
        Domain::Result<std::optional<std::vector<std::byte>>>::success(
            std::vector<std::byte>{
                std::byte{1},
                std::byte{2},
                std::byte{3},
                std::byte{4},
                std::byte{5}}));
    const std::vector<std::byte> secret{std::byte{3}, std::byte{4}};
    Support::require(
        secure.put("token", secret, context).hasValue(),
        "secure storage rejected a bounded secret");
    Support::requireError(
        secure.get("token", 8, context),
        Domain::ErrorCodes::PayloadTooLarge,
        "secure storage returned an over-cap secret");
    secure.shutdown();
    Support::requireError(
        secure.remove("token", context),
        Domain::ErrorCodes::Cancelled,
        "secure storage accepted work after shutdown");

    Fakes::RecordingConfigurationStoreFake configuration{2, 128};
    configuration.setNow(now);
    Domain::AppConfig config{};
    config.allowedRoots = {root};
    config.dashboard.host = "127.0.0.1";
    configuration.loadResult.set(
        Domain::Result<Domain::AppConfig>::success(config));
    configuration.updateResult.set(
        Domain::Result<Domain::AppConfig>::success(config));
    configuration.reloadResult.set(
        Domain::Result<Domain::AppConfig>::success(config));
    Support::require(
        Support::take(configuration.load(context)).allowedRoots.size() == 1,
        "configuration store did not return its script");
    Domain::AppConfigPatch configPatch{};
    configPatch.allowedRoots = std::vector<Domain::PathText>{root};
    configPatch.dashboardPort = Domain::DefaultManagerDashboardPort;
    Support::require(
        configuration.update(configPatch, context).hasValue(),
        "configuration store rejected a bounded update");
    Support::require(
        configuration.lastCapture() &&
            configuration.lastCapture()->call ==
                Fakes::ConfigurationStoreCall::Update &&
            configuration.lastCapture()->patch &&
            configuration.lastCapture()->patch->allowedRoots ==
                configPatch.allowedRoots &&
            configuration.lastCapture()->patch->dashboardPort ==
                configPatch.dashboardPort,
        "configuration store did not retain the bounded typed update");
    Support::requireError(
        configuration.reload(Support::expiredContext(now)),
        Domain::ErrorCodes::DeadlineExceeded,
        "configuration store accepted an expired context");
    configuration.shutdown();
    Support::requireError(
        configuration.reload(context),
        Domain::ErrorCodes::Cancelled,
        "configuration store accepted work after shutdown");
}

} // namespace ForgeConductor::Tests
