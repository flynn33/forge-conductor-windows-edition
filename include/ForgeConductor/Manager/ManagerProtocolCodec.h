#pragma once

#include "ForgeConductor/Domain/ManagerModels.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace ForgeConductor::Manager {

inline constexpr std::uint32_t ManagerProtocolVersion = 1U;

struct ManagerStatusRequest final {
    bool operator==(const ManagerStatusRequest&) const = default;
};

struct ManagerSettingsRequest final {
    bool operator==(const ManagerSettingsRequest&) const = default;
};

struct ManagerSettingsUpdateRequest final {
    Domain::ManagerSettingsPatch patch;
    bool applyImmediately{};
};

struct ManagerCancelRequest final {
    Domain::OperationId operationId;
};

struct ManagerShutdownRequest final {
    bool operator==(const ManagerShutdownRequest&) const = default;
};

using ManagerRequestPayload = std::variant<
    ManagerStatusRequest,
    ManagerSettingsRequest,
    Domain::ManagerControlRequest,
    ManagerSettingsUpdateRequest,
    ManagerCancelRequest,
    ManagerShutdownRequest>;

struct ManagerRequest final {
    std::uint32_t version{ManagerProtocolVersion};
    Domain::RequestId requestId;
    Domain::CorrelationId correlationId;
    std::int64_t deadlineUtcMilliseconds{};
    Domain::Sha256Digest nonce;
    ManagerRequestPayload payload;
};

struct ManagerAcknowledgement final {
    bool acknowledged{true};

    bool operator==(const ManagerAcknowledgement&) const = default;
};

using ManagerResult = std::variant<
    Domain::ManagerStatus,
    Domain::ManagerSettings,
    Domain::ManagerSettingsUpdateOutcome,
    ManagerAcknowledgement>;

using ManagerResponseBody = std::variant<ManagerResult, Domain::Error>;

struct ManagerResponse final {
    std::uint32_t version{ManagerProtocolVersion};
    Domain::RequestId requestId;
    Domain::CorrelationId correlationId;
    ManagerResponseBody body;
};

class ManagerProtocolCodec final {
public:
    // The limit applies to the JSON payload represented by the little-endian
    // prefix. The complete encoded frame is four bytes larger.
    static constexpr std::size_t DefaultMaximumFrameBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumJsonNesting = 64U;

    [[nodiscard]] static Domain::Result<std::vector<std::byte>> encodeRequest(
        const ManagerRequest& request,
        std::size_t maximumFrameBytes = DefaultMaximumFrameBytes) noexcept;

    [[nodiscard]] static Domain::Result<ManagerRequest> decodeRequest(
        std::span<const std::byte> frame,
        std::size_t maximumFrameBytes = DefaultMaximumFrameBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::vector<std::byte>> encodeResponse(
        const ManagerResponse& response,
        std::size_t maximumFrameBytes = DefaultMaximumFrameBytes) noexcept;

    [[nodiscard]] static Domain::Result<ManagerResponse> decodeResponse(
        std::span<const std::byte> frame,
        std::size_t maximumFrameBytes = DefaultMaximumFrameBytes) noexcept;
};

} // namespace ForgeConductor::Manager
