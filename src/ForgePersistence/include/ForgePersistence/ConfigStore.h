// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Ports.h"
#include "ForgePersistence/AppPaths.h"

#include <mutex>

namespace Forge::Persistence {

class ConfigStore final : public Domain::IConfiguration {
public:
    explicit ConfigStore(AppPaths paths);

    std::string stringAt(const std::string& key, const std::string& fallback) const override;
    int intAt(const std::string& key, int fallback) const override;
    bool boolAt(const std::string& key, bool fallback) const override;
    Domain::AppConfig model() const override;
    void reload() override;
    void save(const Domain::AppConfig& config);

private:
    AppPaths paths_;
    mutable std::mutex mutex_;
    Domain::AppConfig config_{};
};

} // namespace Forge::Persistence
