#pragma once

#include "ForgeConductor/Contracts/IConfigurationStore.h"
#include "BoundaryFakeSupport.h"
#include "DeterministicResult.h"

#include <cstddef>
#include <optional>
#include <utility>

namespace ForgeConductor::Tests::Fakes {

enum class ConfigurationStoreCall {
    Load,
    Update,
    Reload
};

struct ConfigurationStoreCapture final {
    ConfigurationStoreCall call;
    std::optional<Domain::AppConfigPatch> patch;
    std::size_t requestedRoots{};
    std::size_t requestedTextBytes{};
};

class RecordingConfigurationStoreFake final
    : public Contracts::IConfigurationStore {
public:
    explicit RecordingConfigurationStoreFake(
        const std::size_t captureItemsMaximum =
            DefaultBoundaryCaptureItemsMaximum,
        const std::size_t captureTextBytesMaximum =
            DefaultBoundaryCaptureTextBytesMaximum) noexcept
        : captureItemsMaximum_{captureItemsMaximum},
          captureTextBytesMaximum_{captureTextBytesMaximum}
    {
    }

    DeterministicResult<Domain::AppConfig> loadResult;
    DeterministicResult<Domain::AppConfig> updateResult;
    DeterministicResult<Domain::AppConfig> reloadResult;

    [[nodiscard]] Domain::Result<Domain::AppConfig> load(
        const Domain::OperationContext& context) noexcept override
    {
        return noPatchCall(
            ConfigurationStoreCall::Load,
            loadResult,
            context);
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig> update(
        const Domain::AppConfigPatch& patch,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::AppConfig>::failure(
                    std::move(gate).error());
            }
            const auto roots = patch.allowedRoots
                ? patch.allowedRoots->size()
                : 0;
            const auto textBytes = patchTextBytes(patch);
            const auto bounded =
                roots <= captureItemsMaximum_ &&
                textBytes <= captureTextBytesMaximum_;
            lastCapture_.emplace(ConfigurationStoreCapture{
                ConfigurationStoreCall::Update,
                bounded
                    ? std::optional<Domain::AppConfigPatch>{patch}
                    : std::nullopt,
                roots,
                textBytes});
            if (!bounded) {
                return Domain::Result<Domain::AppConfig>::failure(
                    boundaryLimitExceeded(
                        "The configuration patch exceeds its capture bound."));
            }
            return boundedScript(updateResult);
        } catch (...) {
            return configFailure(
                "The deterministic configuration update could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig> reload(
        const Domain::OperationContext& context) noexcept override
    {
        return noPatchCall(
            ConfigurationStoreCall::Reload,
            reloadResult,
            context);
    }

    void shutdown() noexcept override { state_.shutdown(); }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] bool isShutdown() const noexcept { return state_.isShutdown(); }
    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<ConfigurationStoreCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

private:
    [[nodiscard]] Domain::Result<Domain::AppConfig> noPatchCall(
        const ConfigurationStoreCall call,
        const DeterministicResult<Domain::AppConfig>& scripted,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::AppConfig>::failure(
                    std::move(gate).error());
            }
            lastCapture_.emplace(
                ConfigurationStoreCapture{call, std::nullopt, 0, 0});
            return boundedScript(scripted);
        } catch (...) {
            return configFailure(
                "The deterministic configuration request could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig> boundedScript(
        const DeterministicResult<Domain::AppConfig>& scripted) const
    {
        auto result = scripted.get();
        if (result &&
            (result.value().allowedRoots.size() > captureItemsMaximum_ ||
             configTextBytes(result.value()) > captureTextBytesMaximum_)) {
            return Domain::Result<Domain::AppConfig>::failure(
                boundaryLimitExceeded(
                    "The scripted configuration exceeds its output bound."));
        }
        return result;
    }

    [[nodiscard]] static std::size_t patchTextBytes(
        const Domain::AppConfigPatch& patch) noexcept
    {
        std::size_t bytes = patch.dashboardHost
            ? patch.dashboardHost->size()
            : 0;
        if (patch.allowedRoots) {
            for (const auto& root : patch.allowedRoots.value()) {
                bytes += root.value().size();
            }
        }
        return bytes;
    }

    [[nodiscard]] static std::size_t configTextBytes(
        const Domain::AppConfig& config) noexcept
    {
        std::size_t bytes = config.dashboard.host.size();
        for (const auto& root : config.allowedRoots) {
            bytes += root.value().size();
        }
        return bytes;
    }

    [[nodiscard]] static Domain::Result<Domain::AppConfig> configFailure(
        const char* message)
    {
        return Domain::Result<Domain::AppConfig>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, message));
    }

    DeterministicBoundaryState state_;
    std::size_t captureItemsMaximum_;
    std::size_t captureTextBytesMaximum_;
    std::optional<ConfigurationStoreCapture> lastCapture_;
};

} // namespace ForgeConductor::Tests::Fakes
