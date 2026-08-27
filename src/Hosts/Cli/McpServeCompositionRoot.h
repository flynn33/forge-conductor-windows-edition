#pragma once

#include "ForgeConductor/Domain/Domain.h"

#include <memory>
#include <optional>

namespace ForgeConductor::Hosts::Cli {

struct McpServeOptions final {
    std::optional<Domain::PathText> explicitHome;
};

class McpServeCompositionRoot final {
public:
    explicit McpServeCompositionRoot(McpServeOptions options = {});
    ~McpServeCompositionRoot() noexcept;

    McpServeCompositionRoot(const McpServeCompositionRoot&) = delete;
    McpServeCompositionRoot& operator=(const McpServeCompositionRoot&) = delete;
    McpServeCompositionRoot(McpServeCompositionRoot&&) = delete;
    McpServeCompositionRoot& operator=(McpServeCompositionRoot&&) = delete;

    [[nodiscard]] int run() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Hosts::Cli
