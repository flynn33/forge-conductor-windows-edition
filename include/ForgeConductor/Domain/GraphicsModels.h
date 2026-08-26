#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <cstdint>
#include <optional>
#include <string>

namespace ForgeConductor::Domain {

enum class GraphicsVisibility { Visible, Hidden, Occluded };
enum class GraphicsDeviceState { Ready, Lost, Recovering, Unavailable };

struct GraphicsDeviceStatus final {
    GraphicsDeviceState state{GraphicsDeviceState::Unavailable};
    std::string adapterName;
    std::optional<std::uint64_t> dedicatedMemoryBytes;
    std::uint64_t generation{};
};

struct RenderRequest final {
    std::uint32_t widthPixels{};
    std::uint32_t heightPixels{};
    double scale{1.0};
    GraphicsVisibility visibility{GraphicsVisibility::Visible};
    std::uint64_t contentRevision{};
};

struct RenderOutcome final {
    bool rendered{};
    bool skippedUnchanged{};
    bool skippedHidden{};
    std::uint64_t deviceGeneration{};
};

[[nodiscard]] Result<void> validateRenderRequest(const RenderRequest& request);

} // namespace ForgeConductor::Domain
