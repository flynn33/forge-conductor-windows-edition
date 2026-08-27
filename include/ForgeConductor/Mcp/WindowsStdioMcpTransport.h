#pragma once

#include "ForgeConductor/Contracts/IMcpTransport.h"

#include <cstddef>
#include <memory>

namespace ForgeConductor::Mcp {

// Owns a single newline-delimited JSON-RPC stdio connection. NativeHandle is a
// Win32 HANDLE represented without exposing Windows.h through the public API.
class WindowsStdioMcpTransport final : public Contracts::IMcpTransport {
public:
    using NativeHandle = void*;

    static constexpr std::size_t MaximumFrameBytes = 1'048'576U;

    // Duplicates STD_INPUT_HANDLE and STD_OUTPUT_HANDLE. The returned transport
    // owns only the duplicates and never closes the process-standard handles.
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<WindowsStdioMcpTransport>>
    create() noexcept;

    // Takes ownership of two distinct Win32 HANDLE values, including on
    // failure. This entry point exists for composition and pipe-backed tests.
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<WindowsStdioMcpTransport>>
    createFromOwnedHandles(
        NativeHandle inputHandle,
        NativeHandle outputHandle) noexcept;

    ~WindowsStdioMcpTransport() noexcept override;

    WindowsStdioMcpTransport(const WindowsStdioMcpTransport&) = delete;
    WindowsStdioMcpTransport& operator=(const WindowsStdioMcpTransport&) = delete;
    WindowsStdioMcpTransport(WindowsStdioMcpTransport&&) = delete;
    WindowsStdioMcpTransport& operator=(WindowsStdioMcpTransport&&) = delete;

    [[nodiscard]] Domain::Result<std::optional<Domain::McpFrame>> receive(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> send(
        const Domain::McpFrame& frame,
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;

    explicit WindowsStdioMcpTransport(std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Mcp
