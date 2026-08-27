#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IContinuityAutomation.h"
#include "ForgeConductor/Contracts/ILegacyContextContinuityService.h"
#include "ForgeConductor/Contracts/IToolServices.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ForgeConductor::Mcp {

struct McpInvocationGuardPolicy final {
    std::uint32_t softIdenticalCallCount{4U};
    std::uint32_t hardIdenticalCallCount{9U};
    std::uint32_t checkpointProgressCount{
        Domain::DefaultCheckpointProgressInterval};
    std::uint32_t handoffProgressCount{
        Domain::DefaultRolloverProgressInterval};
    std::uint32_t checkpointIntervalSeconds{
        Domain::DefaultCheckpointIntervalSeconds};
    std::uint32_t handoffIntervalSeconds{
        Domain::DefaultRolloverIntervalSeconds};
};

// Owns bounded per-client loop and legacy runtime-continuity state. It has no
// worker thread; persistence is synchronous and observes the caller's deadline
// and cancellation token.
class McpInvocationGuard final
    : public Contracts::IToolInvocationGuard,
      public Contracts::IContinuityAutomationStatusSource {
public:
    static constexpr std::size_t MaximumTrackedLoopClients = 256U;
    static constexpr std::size_t MaximumTrackedContinuityClients = 128U;
    static constexpr std::size_t MaximumPendingCalls = 64U;

    [[nodiscard]] static Domain::Result<std::unique_ptr<McpInvocationGuard>>
    create(
        Contracts::ILegacyContextContinuityService& continuity,
        Contracts::IHasher& hasher,
        Contracts::IClock& clock,
        McpInvocationGuardPolicy policy = {}) noexcept;

    ~McpInvocationGuard() noexcept override;

    McpInvocationGuard(const McpInvocationGuard&) = delete;
    McpInvocationGuard& operator=(const McpInvocationGuard&) = delete;
    McpInvocationGuard(McpInvocationGuard&&) = delete;
    McpInvocationGuard& operator=(McpInvocationGuard&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ToolInvocationAdmission>
    beforeInvoke(
        const Domain::ToolCallRequest& request,
        const Domain::ToolDescriptor& descriptor,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> afterInvoke(
        const Domain::ToolCallRequest& request,
        const Domain::ToolDescriptor& descriptor,
        Domain::Result<Domain::ToolCallOutcome> outcome,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

    [[nodiscard]] Domain::ContinuityAutomationStatusSnapshot snapshot(
        const Domain::ClientId& clientId) const noexcept override;

    [[nodiscard]] std::size_t trackedLoopClientCount() const noexcept;
    [[nodiscard]] std::size_t trackedContinuityClientCount() const noexcept;
    [[nodiscard]] std::size_t pendingCallCount() const noexcept;

private:
    class Implementation;

    explicit McpInvocationGuard(
        std::unique_ptr<Implementation> implementation) noexcept;

    std::unique_ptr<Implementation> implementation_;
};

} // namespace ForgeConductor::Mcp
