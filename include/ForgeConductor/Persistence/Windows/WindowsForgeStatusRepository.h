#pragma once

#include "ForgeConductor/Contracts/IForgeStatusRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"

#include <memory>

namespace ForgeConductor::Persistence::Windows {

// Bounded read-only projection over central client-presence and agent-session
// state. Closing an attached repository does not close the shared database.
class WindowsForgeStatusRepository final
    : public Contracts::IForgeStatusRepository {
public:
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsForgeStatusRepository>> attach(
        std::shared_ptr<WindowsCentralDatabase> database) noexcept;

    ~WindowsForgeStatusRepository() noexcept override;

    WindowsForgeStatusRepository(const WindowsForgeStatusRepository&) = delete;
    WindowsForgeStatusRepository& operator=(
        const WindowsForgeStatusRepository&) = delete;
    WindowsForgeStatusRepository(WindowsForgeStatusRepository&&) = delete;
    WindowsForgeStatusRepository& operator=(
        WindowsForgeStatusRepository&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ForgeStatusProjection> snapshot(
        const Domain::OperationContext& context) noexcept override;

    void close() noexcept override;

private:
    struct Impl;

    explicit WindowsForgeStatusRepository(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
