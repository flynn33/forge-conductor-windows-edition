#pragma once

#include "ForgeConductor/Domain/Domain.h"

#include <optional>
#include <string>
#include <string_view>

namespace ForgeConductor::Contracts {

class IMcpTransport {
public:
    virtual ~IMcpTransport() = default;

    // A successful empty value is clean EOF. Diagnostics never use this channel.
    [[nodiscard]] virtual Domain::Result<std::optional<Domain::McpFrame>> receive(
        const Domain::OperationContext& context) noexcept = 0;

    // The frame is compact UTF-8 JSON without a trailing transport newline.
    [[nodiscard]] virtual Domain::Result<void> send(
        const Domain::McpFrame& frame,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
