#pragma once

#include "ForgeConductor/Contracts/IDashboardBearerToken.h"
#include "ForgeConductor/Contracts/ISecureStorage.h"

#include <atomic>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsDashboardBearerTokenGenerator final
    : public Contracts::IDashboardBearerTokenGenerator {
public:
    [[nodiscard]] Domain::Result<Contracts::DashboardBearerSecret> next()
        noexcept override;
};

class WindowsDashboardBearerTokenStore final
    : public Contracts::IDashboardBearerTokenStore {
public:
    static constexpr std::string_view StorageKey = "manager.dashboard.bearer.v1";

    // Both dependencies are non-owning. The store and both dependencies must
    // outlive every admitted call. shutdown() is nonblocking and does not wait
    // for or take ownership of an in-flight dependency operation.
    WindowsDashboardBearerTokenStore(
        Contracts::ISecureStorage& secureStorage,
        Contracts::IDashboardBearerTokenGenerator& generator) noexcept;
    ~WindowsDashboardBearerTokenStore() override;

    WindowsDashboardBearerTokenStore(const WindowsDashboardBearerTokenStore&) = delete;
    WindowsDashboardBearerTokenStore& operator=(
        const WindowsDashboardBearerTokenStore&) = delete;

    [[nodiscard]] Domain::Result<std::optional<Domain::Sha256Digest>> load(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::Sha256Digest> loadOrCreate(
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    [[nodiscard]] Domain::Result<std::optional<Domain::Sha256Digest>> loadUnlocked(
        const Domain::OperationContext& context) noexcept;

    Contracts::ISecureStorage& secureStorage_;
    Contracts::IDashboardBearerTokenGenerator& generator_;
    std::atomic_flag operationAdmitted_ = ATOMIC_FLAG_INIT;
    std::atomic_flag shutdownRequested_ = ATOMIC_FLAG_INIT;
};

} // namespace ForgeConductor::Infrastructure::Windows
