#pragma once

#include "ForgeConductor/Contracts/IToolServices.h"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace ForgeConductor::Mcp {

// Owns the immutable, source-compatible MCP descriptor snapshot advertised to
// both server roles. Routing and transport are intentionally separate.
class McpToolCatalog final : public Contracts::IToolCatalog {
public:
    static constexpr std::size_t ExpectedToolCount = 53U;
    static constexpr std::size_t MaximumProjectMemoryKinds = 100U;

    [[nodiscard]] static Domain::Result<std::unique_ptr<McpToolCatalog>>
    create() noexcept;

    [[nodiscard]] static Domain::Result<void> validateDescriptors(
        std::span<const Domain::McpToolDescriptor> descriptors,
        std::size_t expectedCount = ExpectedToolCount) noexcept;

    ~McpToolCatalog() noexcept override = default;

    McpToolCatalog(const McpToolCatalog&) = delete;
    McpToolCatalog& operator=(const McpToolCatalog&) = delete;
    McpToolCatalog(McpToolCatalog&&) = delete;
    McpToolCatalog& operator=(McpToolCatalog&&) = delete;

    [[nodiscard]] std::span<const Domain::McpToolDescriptor>
    tools() const noexcept override;

private:
    explicit McpToolCatalog(
        std::vector<Domain::McpToolDescriptor> descriptors) noexcept;

    const std::vector<Domain::McpToolDescriptor> descriptors_;
};

} // namespace ForgeConductor::Mcp
