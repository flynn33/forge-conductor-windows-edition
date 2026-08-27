#pragma once

#include "ForgeConductor/Contracts/IManagerAuthentication.h"

#include <atomic>
#include <mutex>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsManagerAuthenticationTokenGenerator final
    : public Contracts::IManagerAuthenticationTokenGenerator {
public:
    [[nodiscard]] Domain::Result<Contracts::ManagerAuthenticationSecret> next()
        noexcept override;
};

class WindowsManagerAuthenticationTokenStore final
    : public Contracts::IManagerAuthenticationTokenStore {
public:
    static constexpr std::string_view StorageKey = "manager.ipc.nonce.v1";

    // The injected services are non-owning and must outlive this store.
    WindowsManagerAuthenticationTokenStore(
        Contracts::ISecureStorage& secureStorage,
        Contracts::IManagerAuthenticationTokenGenerator& generator) noexcept;
    ~WindowsManagerAuthenticationTokenStore() override;

    WindowsManagerAuthenticationTokenStore(const WindowsManagerAuthenticationTokenStore&) =
        delete;
    WindowsManagerAuthenticationTokenStore& operator=(
        const WindowsManagerAuthenticationTokenStore&) = delete;

    [[nodiscard]] Domain::Result<std::optional<Domain::Sha256Digest>> load(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::Sha256Digest> loadOrCreate(
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    [[nodiscard]] Domain::Result<std::optional<Domain::Sha256Digest>> loadUnlocked(
        const Domain::OperationContext& context) noexcept;

    Contracts::ISecureStorage& secureStorage_;
    Contracts::IManagerAuthenticationTokenGenerator& generator_;
    std::mutex admissionMutex_;
    std::atomic_bool shutdownRequested_{};
};

} // namespace ForgeConductor::Infrastructure::Windows
