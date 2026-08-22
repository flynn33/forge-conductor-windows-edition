// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/EventBus.h"

namespace Forge::Runtime {

void InMemoryEventBus::publish(const ForgeEvent& event) {
    std::vector<std::function<void(const ForgeEvent&)>> snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot.reserve(handlers_.size());
        for (const auto& [_, handler] : handlers_) {
            snapshot.push_back(handler);
        }
    }
    for (const auto& handler : snapshot) {
        handler(event);
    }
}

std::uint64_t InMemoryEventBus::subscribe(std::function<void(const ForgeEvent&)> handler) {
    std::lock_guard lock(mutex_);
    const auto token = next_++;
    handlers_.emplace(token, std::move(handler));
    return token;
}

void InMemoryEventBus::unsubscribe(std::uint64_t token) {
    std::lock_guard lock(mutex_);
    handlers_.erase(token);
}

} // namespace Forge::Runtime
