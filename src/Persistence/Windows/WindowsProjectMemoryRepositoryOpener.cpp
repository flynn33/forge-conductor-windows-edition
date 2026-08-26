#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepositoryOpener.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace ForgeConductor::Persistence::Windows {
namespace {

[[nodiscard]] Domain::Error closedError()
{
    return Domain::makeError(
        Domain::ErrorCodes::TransportClosed,
        "The Windows project-memory repository opener is shutting down.");
}

} // namespace

WindowsProjectMemoryRepositoryOpener::WindowsProjectMemoryRepositoryOpener(
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
    std::shared_ptr<Contracts::IProjectMemoryArtifactStore> artifactStore,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    std::shared_ptr<Contracts::IRedactor> redactor,
    std::shared_ptr<Contracts::IHasher> hasher,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<Contracts::IClock> clock,
    WindowsProjectMemoryRepositoryOptions options)
    : applicationPaths_{std::move(applicationPaths)},
      artifactStore_{std::move(artifactStore)},
      runtimeDiagnostics_{std::move(runtimeDiagnostics)},
      redactor_{std::move(redactor)},
      hasher_{std::move(hasher)},
      uuidGenerator_{std::move(uuidGenerator)},
      clock_{std::move(clock)},
      options_{std::move(options)}
{
    if (!applicationPaths_ || !artifactStore_ || !runtimeDiagnostics_ ||
        !redactor_ || !hasher_ || !uuidGenerator_ || !clock_) {
        throw std::invalid_argument(
            "The Windows project-memory opener requires every owned dependency.");
    }
}

WindowsProjectMemoryRepositoryOpener::~WindowsProjectMemoryRepositoryOpener() noexcept
{
    shutdown();
}

Domain::Result<std::shared_ptr<Contracts::IProjectRepository>>
WindowsProjectMemoryRepositoryOpener::openUncached(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (shutdownRequested_.load(std::memory_order_acquire)) {
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::failure(
                closedError());
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The Windows project-memory repository open was cancelled."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The Windows project-memory repository open exceeded its deadline."));
        }
        auto repository = WindowsProjectMemoryRepository::open(
            projectId,
            applicationPaths_,
            artifactStore_,
            runtimeDiagnostics_,
            redactor_,
            hasher_,
            uuidGenerator_,
            clock_,
            options_,
            context);
        if (!repository) {
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::failure(
                std::move(repository).error());
        }
        if (shutdownRequested_.load(std::memory_order_acquire)) {
            auto opened = std::move(repository).value();
            opened->close();
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::failure(
                closedError());
        }
        std::shared_ptr<Contracts::IProjectRepository> result =
            std::move(repository).value();
        return Domain::Result<
            std::shared_ptr<Contracts::IProjectRepository>>::success(
            std::move(result));
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<Contracts::IProjectRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Windows project-memory repository opener failed at its boundary."));
    }
}

void WindowsProjectMemoryRepositoryOpener::shutdown() noexcept
{
    shutdownRequested_.store(true, std::memory_order_release);
}

} // namespace ForgeConductor::Persistence::Windows
