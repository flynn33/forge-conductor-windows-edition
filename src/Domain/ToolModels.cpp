#include "ForgeConductor/Domain/ToolModels.h"

#include <algorithm>
#include <cctype>

namespace ForgeConductor::Domain {

Result<void> validateToolDescriptor(const ToolDescriptor& descriptor)
{
    if (descriptor.name.empty() || descriptor.name.size() > 128 ||
        !std::all_of(descriptor.name.begin(), descriptor.name.end(), [](const unsigned char value) {
            return std::isalnum(value) != 0 || value == '_' || value == '-' || value == '.';
        }) ||
        descriptor.description.empty() || descriptor.pack.empty()) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Tool descriptor requires a safe name, description, and pack."));
    }
    return Result<void>::success();
}

Result<void> validateMcpFrame(const McpFrame& frame, const ResourceBudgets& budgets)
{
    if (frame.utf8Json.empty() || frame.utf8Json.size() > budgets.mcpInputLineBytesMaximum ||
        frame.utf8Json.find('\0') != std::string::npos ||
        frame.utf8Json.find('\n') != std::string::npos ||
        frame.utf8Json.find('\r') != std::string::npos) {
        return Result<void>::failure(makeError(
            ErrorCodes::MalformedMessage,
            "MCP frame must be one nonempty bounded UTF-8 JSON line."));
    }
    return Result<void>::success();
}

} // namespace ForgeConductor::Domain
