// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/ModuleModels.h"

#include <stdexcept>
#include <unordered_map>

namespace Forge::Runtime {

std::string toString(Platform) { return "Windows"; }

std::string toString(ModuleType type) {
    switch (type) {
    case ModuleType::Service: return "service";
    case ModuleType::UI: return "ui";
    case ModuleType::App: return "app";
    }
    return "service";
}

std::string toString(Capability capability) {
    switch (capability) {
    case Capability::Networking: return "networking";
    case Capability::Storage: return "storage";
    case Capability::SecureStorage: return "secure_storage";
    case Capability::FileExport: return "file_export";
    case Capability::CryptoUtilities: return "crypto_utilities";
    case Capability::Telemetry: return "telemetry";
    case Capability::RoutingOverlay: return "routing_overlay";
    case Capability::ToolbarItems: return "toolbar_items";
    case Capability::ViewInjection: return "view_injection";
    case Capability::UIThemeMask: return "ui_theme_mask";
    case Capability::EventPublishing: return "event_publishing";
    case Capability::SharedDatabase: return "shared_database";
    case Capability::Authentication: return "authentication";
    case Capability::Diagnostics: return "diagnostics";
    case Capability::API: return "api";
    case Capability::Security: return "security";
    }
    return "storage";
}

Capability capabilityFromString(const std::string& value) {
    static const std::unordered_map<std::string, Capability> map{
        {"networking", Capability::Networking},
        {"storage", Capability::Storage},
        {"secure_storage", Capability::SecureStorage},
        {"file_export", Capability::FileExport},
        {"crypto_utilities", Capability::CryptoUtilities},
        {"telemetry", Capability::Telemetry},
        {"routing_overlay", Capability::RoutingOverlay},
        {"toolbar_items", Capability::ToolbarItems},
        {"view_injection", Capability::ViewInjection},
        {"ui_theme_mask", Capability::UIThemeMask},
        {"event_publishing", Capability::EventPublishing},
        {"shared_database", Capability::SharedDatabase},
        {"authentication", Capability::Authentication},
        {"diagnostics", Capability::Diagnostics},
        {"api", Capability::API},
        {"security", Capability::Security},
    };
    const auto it = map.find(value);
    if (it == map.end()) {
        throw std::invalid_argument("Unknown capability: " + value);
    }
    return it->second;
}

ModuleType moduleTypeFromString(const std::string& value) {
    if (value == "service") return ModuleType::Service;
    if (value == "ui") return ModuleType::UI;
    if (value == "app") return ModuleType::App;
    throw std::invalid_argument("Unknown moduleType: " + value);
}

} // namespace Forge::Runtime
