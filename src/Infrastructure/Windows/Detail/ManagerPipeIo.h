#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "UniqueHandle.h"

#include <Windows.h>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {

inline constexpr std::size_t DefaultManagerPipeMaximumPayloadBytes =
    2U * 1024U * 1024U;
inline constexpr std::size_t MaximumManagerPipeNameCharacters = 256U;
inline constexpr std::size_t ManagerPipeResponseReceiptPayloadBytes = 4U;

[[nodiscard]] Domain::Result<UniqueHandle> openManagerPipe(
    std::wstring_view pipeName,
    DWORD desiredAccess,
    const Domain::OperationContext& context,
    HANDLE shutdownEvent) noexcept;

[[nodiscard]] Domain::Result<void> connectManagerPipe(
    HANDLE serverPipe,
    const Domain::OperationContext& context,
    HANDLE shutdownEvent) noexcept;

[[nodiscard]] Domain::Result<std::vector<std::byte>> readManagerPipeFrame(
    HANDLE pipe,
    const Domain::OperationContext& context,
    HANDLE shutdownEvent,
    std::size_t maximumPayloadBytes =
        DefaultManagerPipeMaximumPayloadBytes) noexcept;

[[nodiscard]] Domain::Result<void> writeManagerPipeFrame(
    HANDLE pipe,
    // The codec-owned frame is passed unchanged: four little-endian length
    // bytes followed by exactly that many payload bytes.
    std::span<const std::byte> frame,
    const Domain::OperationContext& context,
    HANDLE shutdownEvent,
    std::size_t maximumPayloadBytes =
        DefaultManagerPipeMaximumPayloadBytes) noexcept;

// A client writes this fixed transport receipt only after it has read one
// complete response frame. The server waits for the receipt with the caller's
// bounded context before disconnecting, preventing Windows from discarding an
// unread response while avoiding FlushFileBuffers' unbounded synchronous wait.
[[nodiscard]] Domain::Result<void> writeManagerPipeResponseReceipt(
    HANDLE pipe,
    const Domain::OperationContext& context,
    HANDLE shutdownEvent) noexcept;

[[nodiscard]] Domain::Result<void> readManagerPipeResponseReceipt(
    HANDLE pipe,
    const Domain::OperationContext& context,
    HANDLE shutdownEvent) noexcept;

} // namespace ForgeConductor::Infrastructure::Windows::Detail
