// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Forge::Runtime {

struct ForgeEvent final {
    std::string name;
    std::string sourceModuleID;
    std::map<std::string, std::string> payload;
};

class IForgeEventBus {
public:
    virtual void publish(const ForgeEvent& event) = 0;
    virtual std::uint64_t subscribe(std::function<void(const ForgeEvent&)> handler) = 0;
    virtual void unsubscribe(std::uint64_t token) = 0;
    virtual ~IForgeEventBus() = default;
};

class InMemoryEventBus final : public IForgeEventBus {
public:
    void publish(const ForgeEvent& event) override;
    std::uint64_t subscribe(std::function<void(const ForgeEvent&)> handler) override;
    void unsubscribe(std::uint64_t token) override;

private:
    mutable std::mutex mutex_;
    std::uint64_t next_{1};
    std::map<std::uint64_t, std::function<void(const ForgeEvent&)>> handlers_;
};

} // namespace Forge::Runtime
