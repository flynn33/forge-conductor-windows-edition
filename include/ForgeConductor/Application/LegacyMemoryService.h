#pragma once

#include "ForgeConductor/Contracts/ILegacyMemoryRepository.h"
#include "ForgeConductor/Contracts/ILegacyMemoryService.h"
#include "ForgeConductor/Contracts/IUnicodeCanonicalizer.h"

#include <memory>

namespace ForgeConductor::Application {

// The injected repository must outlive this service. The service shares
// ownership of the immutable Unicode canonicalizer, owns no platform or
// database resource directly, and closes the repository only after all
// admitted synchronous operations have left the application boundary.
class LegacyMemoryService final : public Contracts::ILegacyMemoryService {
public:
    LegacyMemoryService(
        Contracts::ILegacyMemoryRepository& repository,
        std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
            unicodeCanonicalizer);
    ~LegacyMemoryService() noexcept override;

    LegacyMemoryService(const LegacyMemoryService&) = delete;
    LegacyMemoryService& operator=(const LegacyMemoryService&) = delete;
    LegacyMemoryService(LegacyMemoryService&&) = delete;
    LegacyMemoryService& operator=(LegacyMemoryService&&) = delete;

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySetOutcome> set(
        const Domain::LegacyMemorySetRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryGetOutcome> get(
        const Domain::LegacyMemoryGetRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryListOutcome> list(
        const Domain::LegacyMemoryListRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(
        const Domain::LegacyMemoryRemoveRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySearchOutcome> search(
        const Domain::LegacyMemorySearchRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryPurgeOutcome> purge(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
