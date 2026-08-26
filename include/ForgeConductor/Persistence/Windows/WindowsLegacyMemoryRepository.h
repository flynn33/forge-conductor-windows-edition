#pragma once

#include "ForgeConductor/Contracts/ILegacyMemoryRepository.h"
#include "ForgeConductor/Contracts/IUnicodeCanonicalizer.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"

#include <memory>
#include <string_view>

namespace ForgeConductor::Persistence::Windows {

// Owns the central database used by the global compatibility memory surface.
// Every operation is serialized by that database's bounded connection owner;
// close is idempotent and releases the database after admitted work drains.
class WindowsLegacyMemoryRepository final
    : public Contracts::ILegacyMemoryRepository {
public:
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsLegacyMemoryRepository>> open(
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
            unicodeCanonicalizer,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsLegacyMemoryRepository>> create(
        std::unique_ptr<WindowsCentralDatabase> database,
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
            unicodeCanonicalizer) noexcept;

    // Attaches to a composition-root-owned central database. Closing this
    // repository stops its own admission without closing the shared database.
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsLegacyMemoryRepository>> attach(
        std::shared_ptr<WindowsCentralDatabase> database,
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
            unicodeCanonicalizer) noexcept;

    ~WindowsLegacyMemoryRepository() noexcept override;

    WindowsLegacyMemoryRepository(const WindowsLegacyMemoryRepository&) = delete;
    WindowsLegacyMemoryRepository& operator=(
        const WindowsLegacyMemoryRepository&) = delete;
    WindowsLegacyMemoryRepository(WindowsLegacyMemoryRepository&&) = delete;
    WindowsLegacyMemoryRepository& operator=(
        WindowsLegacyMemoryRepository&&) = delete;

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySetOutcome> upsert(
        const Domain::LegacyMemoryUpsert& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::LegacyMemoryGetOutcome> get(
        std::string_view key,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::LegacyMemoryListOutcome> list(
        const Domain::LegacyMemoryListQuery& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(
        std::string_view key,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::LegacyMemorySearchOutcome> search(
        const Domain::LegacyMemorySearchQuery& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::LegacyMemoryPurgeOutcome> purge(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept override;

    void close() noexcept override;

private:
    struct Impl;

    explicit WindowsLegacyMemoryRepository(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
