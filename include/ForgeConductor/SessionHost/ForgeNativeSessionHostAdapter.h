#pragma once

#include "ForgeConductor/Contracts/IContinuityDocumentCodec.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/INativeSessionHostServices.h"
#include "ForgeConductor/Contracts/ISessionHostAdapter.h"

#include <memory>

namespace ForgeConductor::SessionHost {

class ForgeNativeSessionHostAdapter final
    : public Contracts::ISessionHostAdapter {
public:
    static constexpr std::string_view AdapterIdentifier =
        "forge.native-session-host";
    static constexpr std::string_view AdapterVersion = "1.0.0";
    static constexpr std::uint32_t ProtocolVersion = 1U;
    static constexpr std::size_t MaximumRetries = 3U;

    ForgeNativeSessionHostAdapter(
        Domain::AdapterId adapterId,
        Contracts::INativeSessionLedger& ledger,
        Contracts::INativeSessionTransport& transport,
        Contracts::IContinuityDocumentCodec& codec,
        Contracts::IUuidGenerator& uuidGenerator,
        Contracts::IClock& clock);
    ~ForgeNativeSessionHostAdapter() noexcept override;

    ForgeNativeSessionHostAdapter(
        const ForgeNativeSessionHostAdapter&) = delete;
    ForgeNativeSessionHostAdapter& operator=(
        const ForgeNativeSessionHostAdapter&) = delete;
    ForgeNativeSessionHostAdapter(
        ForgeNativeSessionHostAdapter&&) = delete;
    ForgeNativeSessionHostAdapter& operator=(
        ForgeNativeSessionHostAdapter&&) = delete;

    [[nodiscard]] const Domain::AdapterId& identifier() const noexcept override;
    [[nodiscard]] std::string_view version() const noexcept override;

    [[nodiscard]] Domain::Result<Domain::HostCapabilities> capabilities(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::HostSession> createSession(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::optional<Domain::HostSession>>
    queryByIdempotencyKey(
        const Domain::ProjectId& projectId,
        const Domain::IdempotencyKey& key,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> bootstrap(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::HandoffAcknowledgement>
    awaitAcknowledgement(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::Sha256Digest& handoffSha256,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::HostRecoveryReport> recover(
        const Domain::HostRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::NativeSessionHostHealth> health(
        const Domain::OperationContext& context) noexcept;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

[[nodiscard]] Domain::Result<Domain::HostPluginManifest>
nativeSessionHostPluginManifest(const Domain::PathText& executable);

} // namespace ForgeConductor::SessionHost
