#pragma once

#include "ForgeConductor/Domain/EnvironmentModels.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {

inline constexpr char LMStudioPrimaryServerId[] = "forge-conductor";
inline constexpr char LMStudioFallbackServerId[] = "forge-conductor-fallback";

struct LMStudioRoleConfigurationStatus final {
    Domain::LMStudioConnectorRole role{Domain::LMStudioConnectorRole::Primary};
    bool present{};
    bool valid{};
    std::optional<Domain::DeploymentId> deploymentId;
    std::string detail;
};

struct LMStudioConfigurationInspection final {
    std::vector<LMStudioRoleConfigurationStatus> roles;
    bool registered{};
    std::optional<Domain::DeploymentId> deploymentId;
    std::string detail;
};

class LMStudioConfigurationDocument final {
public:
    LMStudioConfigurationDocument(const LMStudioConfigurationDocument&) = default;
    LMStudioConfigurationDocument(LMStudioConfigurationDocument&&) = default;
    LMStudioConfigurationDocument& operator=(const LMStudioConfigurationDocument&) = default;
    LMStudioConfigurationDocument& operator=(LMStudioConfigurationDocument&&) = default;

    [[nodiscard]] const std::string& sourceUtf8() const noexcept { return sourceUtf8_; }

private:
    friend class LMStudioConfigurationCodec;

    explicit LMStudioConfigurationDocument(std::string sourceUtf8)
        : sourceUtf8_{std::move(sourceUtf8)}
    {
    }

    std::string sourceUtf8_;
};

class LMStudioConfigurationCodec final {
public:
    static constexpr std::size_t MaximumDocumentBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumJsonDepth = 32U;
    static constexpr std::size_t MaximumJsonNodes = 65'536U;

    [[nodiscard]] static LMStudioConfigurationDocument empty();

    [[nodiscard]] static Domain::Result<LMStudioConfigurationDocument> parse(
        std::span<const std::byte> content) noexcept;

    [[nodiscard]] static Domain::Result<LMStudioConfigurationInspection> inspect(
        const LMStudioConfigurationDocument& document,
        const Domain::PathText& expectedBinary,
        const Domain::PathText& expectedForgeHome) noexcept;

    [[nodiscard]] static Domain::Result<std::vector<std::byte>> mergeForgeServers(
        const LMStudioConfigurationDocument& document,
        const Domain::PathText& binary,
        const Domain::PathText& forgeHome,
        const Domain::DeploymentId& deploymentId) noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows
