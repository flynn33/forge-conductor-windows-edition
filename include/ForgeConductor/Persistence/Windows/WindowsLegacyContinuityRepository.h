#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ILegacyContinuityRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"

#include <memory>

namespace ForgeConductor::Persistence::Windows {

// Central-store implementation of the source-compatible context continuity
// surface. A repository attached to a shared database owns only its admission;
// an owning repository also closes the database after admitted calls drain.
class WindowsLegacyContinuityRepository final
    : public Contracts::ILegacyContinuityRepository {
public:
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsLegacyContinuityRepository>> open(
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IHasher> hasher,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsLegacyContinuityRepository>> create(
        std::unique_ptr<WindowsCentralDatabase> database,
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IHasher> hasher) noexcept;

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsLegacyContinuityRepository>> attach(
        std::shared_ptr<WindowsCentralDatabase> database,
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IHasher> hasher) noexcept;

    ~WindowsLegacyContinuityRepository() noexcept override;

    WindowsLegacyContinuityRepository(
        const WindowsLegacyContinuityRepository&) = delete;
    WindowsLegacyContinuityRepository& operator=(
        const WindowsLegacyContinuityRepository&) = delete;
    WindowsLegacyContinuityRepository(
        WindowsLegacyContinuityRepository&&) = delete;
    WindowsLegacyContinuityRepository& operator=(
        WindowsLegacyContinuityRepository&&) = delete;

    [[nodiscard]] Domain::Result<
        std::optional<Domain::LegacyContinuityRecord>> get(
        const Domain::LegacyHandoffId& handoffId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<
        std::optional<Domain::LegacyContinuityRecord>> latest(
        const std::optional<Domain::ClientId>& clientId,
        bool resumeReadyOnly,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<
        std::vector<Domain::LegacyContinuityRecord>> list(
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<
        std::vector<Domain::LegacyContinuityRecord>> listAll(
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::LegacyContinuityRecord>
    compareExchange(
        const Domain::LegacyContinuityCompareExchange& mutation,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<
        Domain::LegacyContinuityPointerRepairOutcome> repairPointers(
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::LegacyContinuityResetOutcome> reset(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept override;

    void close() noexcept override;

private:
    struct Impl;

    explicit WindowsLegacyContinuityRepository(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
