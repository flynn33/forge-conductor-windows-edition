#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Tests::McpContract {

enum class DependencyMode {
    Happy,
    DatabaseBusy,
    ProcessNonzero
};

struct InvocationResult final {
    bool hasOutcome{};
    bool receiptOk{};
    std::string errorCode;
    bool errorRetryable{};
    std::string receiptErrorCode;
    nlohmann::json payload;
    nlohmann::json observations;
    std::vector<std::string> effects;
};

class McpToolContractFixture final {
public:
    explicit McpToolContractFixture(DependencyMode mode);
    ~McpToolContractFixture() noexcept;

    McpToolContractFixture(const McpToolContractFixture&) = delete;
    McpToolContractFixture& operator=(const McpToolContractFixture&) = delete;
    McpToolContractFixture(McpToolContractFixture&&) = delete;
    McpToolContractFixture& operator=(McpToolContractFixture&&) = delete;

    [[nodiscard]] InvocationResult invoke(
        std::string_view toolName,
        const nlohmann::json& arguments);

    [[nodiscard]] std::vector<std::string> catalogToolNames() const;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Tests::McpContract
