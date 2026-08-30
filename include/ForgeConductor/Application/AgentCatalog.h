#pragma once

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace ForgeConductor::Application {

enum class AgentDefinitionOrigin {
    BuiltIn,
    Custom
};

struct AgentDefinitionDocument final {
    std::string stableName;
    std::string markdown;
    AgentDefinitionOrigin origin{AgentDefinitionOrigin::BuiltIn};
};

struct AgentCatalogReloadSummary final {
    std::size_t acceptedDocuments{};
    std::size_t rejectedDocuments{};
    std::size_t retainedDefinitions{};
    std::size_t truncatedDefinitions{};
};

// AgentCatalog owns one immutable, atomically replaceable catalog snapshot.
// Source documents are caller-owned and are consumed synchronously by create
// and reload. The injected clock is shared for the catalog lifetime.
class AgentCatalog final : public Contracts::IAgentCatalog {
public:
    static constexpr std::size_t MaximumEntries = 256U;
    static constexpr std::size_t MandatoryEntryCount = 10U;
    static constexpr std::array<std::string_view, MandatoryEntryCount>
        MandatoryIds{
            "debug",
            "docs",
            "explore",
            "implement",
            "plan",
            "precommit-audit",
            "research",
            "review",
            "security",
            "test"};
    static constexpr std::size_t MaximumDefinitionDocuments = 1024U;
    static constexpr std::size_t MaximumDefinitionBytes = 64U * 1024U;
    static constexpr std::size_t MaximumAggregateDefinitionBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t MaximumStableNameBytes = 512U;
    static constexpr std::size_t MaximumRecommendationTaskBytes = 4U * 1024U;
    static constexpr std::size_t MaximumListItems = 64U;
    static constexpr std::size_t MaximumSpecItems = 256U;
    static constexpr std::size_t MaximumFieldBytes = 4U * 1024U;
    static constexpr std::size_t MaximumSpecTextBytes = 64U * 1024U;

    [[nodiscard]] static Domain::Result<std::unique_ptr<AgentCatalog>> create(
        std::shared_ptr<Contracts::IClock> clock,
        std::span<const AgentDefinitionDocument> definitions,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<Domain::AgentSpec> parseDefinition(
        const AgentDefinitionDocument& definition,
        const Contracts::IClock& clock,
        const Domain::OperationContext& context) noexcept;

    ~AgentCatalog() noexcept override;

    AgentCatalog(const AgentCatalog&) = delete;
    AgentCatalog& operator=(const AgentCatalog&) = delete;
    AgentCatalog(AgentCatalog&&) = delete;
    AgentCatalog& operator=(AgentCatalog&&) = delete;

    [[nodiscard]] Domain::Result<AgentCatalogReloadSummary> reload(
        std::span<const AgentDefinitionDocument> definitions,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSpec>> all(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentSpec>> get(
        const Domain::AgentId& agentId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AgentSpec> recommend(
        std::string_view task,
        const Domain::OperationContext& context) noexcept override;

private:
    class Impl;
    explicit AgentCatalog(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
