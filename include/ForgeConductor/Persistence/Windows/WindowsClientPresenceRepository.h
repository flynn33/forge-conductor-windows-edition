#pragma once

#include "ForgeConductor/Contracts/IClientPresenceRepository.h"

#include <memory>

namespace ForgeConductor::Persistence::Windows {

class WindowsCentralDatabase;

// Attach-only lifecycle persistence over the composition-owned central
// database. No timer or thread is owned by this repository; the serve owner
// supplies heartbeat times and closes admission before database shutdown.
class WindowsClientPresenceRepository final
    : public Contracts::IClientPresenceRepository {
public:
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsClientPresenceRepository>> attach(
        std::shared_ptr<WindowsCentralDatabase> database) noexcept;

    ~WindowsClientPresenceRepository() noexcept override;

    WindowsClientPresenceRepository(
        const WindowsClientPresenceRepository&) = delete;
    WindowsClientPresenceRepository& operator=(
        const WindowsClientPresenceRepository&) = delete;
    WindowsClientPresenceRepository(
        WindowsClientPresenceRepository&&) = delete;
    WindowsClientPresenceRepository& operator=(
        WindowsClientPresenceRepository&&) = delete;

    [[nodiscard]] Domain::Result<void> upsert(
        const Domain::ClientPresenceRegistration& registration,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<bool> heartbeat(
        const Domain::ClientPresenceIdentity& identity,
        Domain::UtcTimePoint observedAt,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<bool> remove(
        const Domain::ClientPresenceIdentity& identity,
        const Domain::OperationContext& context) noexcept override;

    void close() noexcept override;

private:
    struct Impl;

    explicit WindowsClientPresenceRepository(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
