#pragma once

#include "ForgeConductor/Domain/ManagerModels.h"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace ForgeConductor::Dashboard {

class DashboardManagerSettingsMutation final {
public:
    DashboardManagerSettingsMutation(
        Domain::ManagerSettingsPatch patch,
        const bool apply)
        : patch_{std::move(patch)}, apply_{apply}
    {
    }

    [[nodiscard]] const Domain::ManagerSettingsPatch& patch() const noexcept
    {
        return patch_;
    }

    [[nodiscard]] bool apply() const noexcept { return apply_; }

private:
    Domain::ManagerSettingsPatch patch_;
    bool apply_{true};
};

class DashboardManagerJsonCodec final {
public:
    static constexpr std::size_t MaximumMutationBytes = 1024U * 1024U;
    static constexpr std::size_t MaximumResponseBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumJsonNesting = 16U;

    [[nodiscard]] static Domain::Result<std::vector<std::byte>> encodeStatus(
        const Domain::ManagerStatus& status,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::vector<std::byte>> encodeSettings(
        const Domain::ManagerSettings& settings,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::vector<std::byte>>
    encodeSettingsUpdateOutcome(
        const Domain::ManagerSettingsUpdateOutcome& outcome,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<DashboardManagerSettingsMutation>
    decodeSettingsMutation(
        std::span<const std::byte> body,
        std::size_t maximumBytes = MaximumMutationBytes) noexcept;
};

} // namespace ForgeConductor::Dashboard
