// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

namespace Forge::Runtime {

enum class ToolbarAction { None, OpenOverlay, PublishEvent };
enum class OverlayPresentation { Sheet, Modal, Inline };
enum class OverlayDestination { Route, View };

struct ToolbarItemDescriptor final {
    std::string itemID;
    std::string title;
    ToolbarAction action{ToolbarAction::None};
    std::string actionPayload;
};

struct ViewInjectionDescriptor final {
    std::string viewID;
    std::string slotID;
};

struct OverlaySchema final {
    std::string overlayID;
    OverlayPresentation presentation{OverlayPresentation::Sheet};
    OverlayDestination destination{OverlayDestination::View};
    std::string targetID;
};

struct UIContributions final {
    std::vector<ToolbarItemDescriptor> toolbarItems;
    std::vector<ViewInjectionDescriptor> viewInjections;
    std::vector<OverlaySchema> overlays;
    std::vector<std::string> themeIDs;
};

struct SurfaceStateSnapshot final {
    std::string activeModuleID;
    UIContributions contributions;
};

} // namespace Forge::Runtime
