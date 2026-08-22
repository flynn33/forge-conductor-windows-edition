// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <string>

namespace Forge::Domain {

class IClock {
public:
    virtual std::chrono::system_clock::time_point now() const = 0;
    virtual ~IClock() = default;
};

class SystemClock final : public IClock {
public:
    std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::now();
    }
};

[[nodiscard]] std::string iso8601(std::chrono::system_clock::time_point tp);
[[nodiscard]] std::string makeUuid();

} // namespace Forge::Domain
