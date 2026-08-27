#pragma once

#include "ForgeConductor/Dashboard/DashboardHttpModels.h"
#include "ForgeConductor/Domain/Identifiers.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Dashboard {

class DashboardRequestPolicy final {
public:
    [[nodiscard]] static Domain::Result<DashboardRequestPolicy> create(
        std::string bindHost,
        std::uint16_t bindPort,
        Domain::Sha256Digest bearerToken) noexcept;

    [[nodiscard]] std::optional<DashboardHttpRejection> rejectionFor(
        const DashboardHttpRequest& request) const noexcept;

    [[nodiscard]] static bool isConfiguredLoopbackHost(
        std::string_view host) noexcept;

    [[nodiscard]] const std::string& bindHost() const noexcept { return bindHost_; }
    [[nodiscard]] std::uint16_t bindPort() const noexcept { return bindPort_; }

private:
    DashboardRequestPolicy(
        std::string bindHost,
        std::uint16_t bindPort,
        Domain::Sha256Digest bearerToken)
        : bindHost_{std::move(bindHost)},
          bindPort_{bindPort},
          bearerToken_{std::move(bearerToken)}
    {
    }

    std::string bindHost_;
    std::uint16_t bindPort_{};
    Domain::Sha256Digest bearerToken_;
};

} // namespace ForgeConductor::Dashboard
