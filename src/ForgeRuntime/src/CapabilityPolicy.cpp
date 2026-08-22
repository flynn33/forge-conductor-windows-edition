// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/CapabilityPolicy.h"

#include <algorithm>

namespace Forge::Runtime {

bool DefaultCapabilityPolicy::allows(const ModuleManifest& manifest, Capability capability) const {
    return std::find(
               manifest.capabilitiesRequested.begin(),
               manifest.capabilitiesRequested.end(),
               capability) != manifest.capabilitiesRequested.end();
}

bool DefaultCommunicationGuard::allow(
    const std::string& sourceID,
    const std::string& destinationID) const {
    if (sourceID.empty() || destinationID.empty()) {
        return false;
    }
    if (sourceID == destinationID) {
        return false;
    }
    if (destinationID.rfind("forsetti.internal.", 0) == 0 ||
        destinationID.rfind("forge.internal.", 0) == 0) {
        return false;
    }
    return true;
}

} // namespace Forge::Runtime
