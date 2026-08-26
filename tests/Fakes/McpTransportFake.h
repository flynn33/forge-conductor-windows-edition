#pragma once

#include "ForgeConductor/Contracts/IMcpTransport.h"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

class McpTransportFake final : public Contracts::IMcpTransport {
public:
    static constexpr std::size_t DefaultFrameBytesMaximum = 1'048'576;
    static constexpr std::size_t DefaultFrameCountMaximum = 64;

    explicit McpTransportFake(
        std::vector<std::string> inbound = {},
        const std::size_t frameBytesMaximum = DefaultFrameBytesMaximum,
        const std::size_t frameCountMaximum = DefaultFrameCountMaximum)
        : McpTransportFake{
              std::move(inbound),
              budgetsWithFrameLimit(frameBytesMaximum),
              Domain::MonotonicTimePoint{},
              frameCountMaximum}
    {
    }

    McpTransportFake(
        std::vector<std::string> inbound,
        Domain::ResourceBudgets budgets,
        const Domain::MonotonicTimePoint now,
        const std::size_t frameCountMaximum = DefaultFrameCountMaximum)
        : inbound_{std::move(inbound)},
          budgets_{std::move(budgets)},
          now_{now},
          frameCountMaximum_{frameCountMaximum}
    {
        if (budgets_.mcpInputLineBytesMaximum == 0 ||
            frameCountMaximum_ == 0 ||
            inbound_.size() > frameCountMaximum_) {
            throw std::invalid_argument(
                "MCP fake budgets or queue limits are invalid.");
        }
        outbound_.reserve(frameCountMaximum_);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::McpFrame>> receive(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto ready = operationReady(context);
            if (!ready) {
                return Domain::Result<std::optional<Domain::McpFrame>>::failure(
                    std::move(ready).error());
            }
            if (nextInbound_ >= inbound_.size()) {
                return Domain::Result<std::optional<Domain::McpFrame>>::success(
                    std::nullopt);
            }

            Domain::McpFrame frame{inbound_[nextInbound_]};
            ++nextInbound_;
            auto valid = validateFrame(frame, "input");
            if (!valid) {
                return Domain::Result<std::optional<Domain::McpFrame>>::failure(
                    std::move(valid).error());
            }
            return Domain::Result<std::optional<Domain::McpFrame>>::success(
                std::optional<Domain::McpFrame>{std::move(frame)});
        } catch (...) {
            return Domain::Result<std::optional<Domain::McpFrame>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "MCP fake input could not be received."));
        }
    }

    [[nodiscard]] Domain::Result<void> send(
        const Domain::McpFrame& frame,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto ready = operationReady(context);
            if (!ready) {
                return ready;
            }
            auto valid = validateFrame(frame, "output");
            if (!valid) {
                return valid;
            }
            if (outbound_.size() >= frameCountMaximum_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "MCP fake output queue is full."));
            }
            outbound_.emplace_back(frame.utf8Json);
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "MCP fake output could not be retained."));
        }
    }

    void shutdown() noexcept override { shutdown_ = true; }

    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }

    [[nodiscard]] Domain::MonotonicTimePoint now() const noexcept { return now_; }

    [[nodiscard]] const Domain::ResourceBudgets& budgets() const noexcept
    {
        return budgets_;
    }

    [[nodiscard]] std::size_t remainingInboundCount() const noexcept
    {
        return inbound_.size() - nextInbound_;
    }

    [[nodiscard]] const std::vector<std::string>& outbound() const noexcept
    {
        return outbound_;
    }

private:
    [[nodiscard]] static Domain::ResourceBudgets budgetsWithFrameLimit(
        const std::size_t frameBytesMaximum) noexcept
    {
        Domain::ResourceBudgets budgets{};
        budgets.mcpInputLineBytesMaximum = frameBytesMaximum;
        return budgets;
    }

    [[nodiscard]] Domain::Result<void> operationReady(
        const Domain::OperationContext& context) const
    {
        if (shutdown_) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "MCP transport is closed."));
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "MCP transport operation was cancelled."));
        }
        if (context.isExpired(now_)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "MCP transport operation deadline expired."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> validateFrame(
        const Domain::McpFrame& frame,
        const char* const direction) const
    {
        auto valid = Domain::validateMcpFrame(frame, budgets_);
        if (valid) {
            return valid;
        }
        if (frame.utf8Json.size() > budgets_.mcpInputLineBytesMaximum) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                std::string{"MCP "} + direction +
                    " exceeds the configured frame limit."));
        }
        return valid;
    }

    std::vector<std::string> inbound_;
    std::vector<std::string> outbound_;
    Domain::ResourceBudgets budgets_;
    Domain::MonotonicTimePoint now_{};
    std::size_t frameCountMaximum_{};
    std::size_t nextInbound_{};
    bool shutdown_{};
};

} // namespace ForgeConductor::Tests::Fakes
