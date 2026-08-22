// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Ports.h"
#include "ForgePersistence/SQLiteStore.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Forge::Telemetry {

class SystemCollector final : public Domain::ISystemMetricsCollector {
public:
    SystemCollector();
    ~SystemCollector() override;
    Domain::SystemMetrics collect() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TelemetryService final : public Domain::ITelemetry {
public:
    TelemetryService(
        Persistence::SQLiteStore& store,
        std::function<std::vector<std::string>()> toolNames);
    ~TelemetryService() override;

    Domain::TelemetrySnapshot currentFrame() override;
    void start(double intervalSec) override;
    void stop() override;
    std::uint64_t addListener(std::function<void(const Domain::TelemetrySnapshot&)> listener) override;
    void removeListener(std::uint64_t id) override;

    [[nodiscard]] double measuredHz() const { return measuredHz_; }

private:
    Domain::TelemetrySnapshot compose();

    Persistence::SQLiteStore& store_;
    std::function<std::vector<std::string>()> toolNames_;
    SystemCollector collector_;
    std::mutex mutex_;
    Domain::TelemetrySnapshot latest_{};
    std::unordered_map<std::uint64_t, std::function<void(const Domain::TelemetrySnapshot&)>> listeners_;
    std::uint64_t nextId_{1};
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<double> measuredHz_{0};
};

} // namespace Forge::Telemetry
