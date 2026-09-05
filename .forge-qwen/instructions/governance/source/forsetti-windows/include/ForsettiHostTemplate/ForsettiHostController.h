// Forsetti Framework for Windows
// Copyright (c) 2026 James Daley. All Rights Reserved.
// Proprietary and Confidential. Patent Pending.

#pragma once

#include "ForsettiCore/ForsettiRuntime.h"
#include "ForsettiHostTemplate/ForsettiHostState.h"

#include <memory>
#include <mutex>
#include <string>

namespace Forsetti {

class IForsettiHostController {
public:
    virtual void boot(ForsettiHostLaunchStrategy launchStrategy = ForsettiHostLaunchStrategy::restoreOnly()) = 0;
    virtual void shutdown() = 0;
    virtual void activateModule(const std::string& moduleID) = 0;
    virtual void deactivateModule(const std::string& moduleID) = 0;
    virtual ForsettiHostStateSnapshot snapshot() const = 0;

    virtual ~IForsettiHostController() = default;
};

class ForsettiHostController final : public IForsettiHostController {
public:
    explicit ForsettiHostController(std::shared_ptr<ForsettiRuntime> runtime);

    void boot(ForsettiHostLaunchStrategy launchStrategy = ForsettiHostLaunchStrategy::restoreOnly()) override;
    void shutdown() override;
    void activateModule(const std::string& moduleID) override;
    void deactivateModule(const std::string& moduleID) override;
    ForsettiHostStateSnapshot snapshot() const override;

private:
    void applyLaunchStrategy(const ForsettiHostLaunchStrategy& launchStrategy);
    void refreshSnapshotLocked(ForsettiHostStateKind state);
    void recordFailureLocked(ForsettiHostStateKind state, const std::string& message);

    std::shared_ptr<ForsettiRuntime> runtime_;
    mutable std::mutex mutex_;
    ForsettiHostStateSnapshot snapshot_;
};

} // namespace Forsetti
