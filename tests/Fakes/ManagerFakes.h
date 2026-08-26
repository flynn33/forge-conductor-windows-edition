#pragma once

#include "BoundedFakeSupport.h"
#include "DeterministicResult.h"
#include "ForgeConductor/Contracts/IManagerServices.h"

#include <optional>
#include <utility>

namespace ForgeConductor::Tests::Fakes {

class ManagerClientFake final : public Contracts::IManagerClient {
public:
    explicit ManagerClientFake(const Domain::MonotonicTimePoint now = {}) noexcept
        : gate_{now}
    {
    }

    DeterministicResult<Domain::ManagerStatus> statusResult;
    DeterministicResult<Domain::ManagerSettings> settingsResult;
    DeterministicResult<Domain::ManagerStatus> controlResult;
    DeterministicResult<Domain::ManagerSettings> updateSettingsResult;

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& context) noexcept override
    {
        return finish(context, statusResult);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext& context) noexcept override
    {
        return finish(context, settingsResult);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastControlRequest_ = request;
            return finish(context, controlResult);
        } catch (...) {
            return fakeInternalFailure<Domain::ManagerStatus>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        const bool applyImmediately,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastSettingsPatch_ = patch;
            lastApplyImmediately_ = applyImmediately;
            return finish(context, updateSettingsResult);
        } catch (...) {
            return fakeInternalFailure<Domain::ManagerSettings>();
        }
    }

    void shutdown() noexcept override { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] const std::optional<Domain::ManagerControlRequest>&
    lastControlRequest() const noexcept
    {
        return lastControlRequest_;
    }

    [[nodiscard]] const std::optional<Domain::ManagerSettingsPatch>&
    lastSettingsPatch() const noexcept
    {
        return lastSettingsPatch_;
    }

    [[nodiscard]] const std::optional<bool>&
    lastApplyImmediately() const noexcept
    {
        return lastApplyImmediately_;
    }

private:
    template <typename T>
    [[nodiscard]] Domain::Result<T> finish(
        const Domain::OperationContext& context,
        const DeterministicResult<T>& result) noexcept
    {
        auto accepted = gate_.enter(context);
        if (!accepted) {
            return propagateFakeGateFailure<T>(std::move(accepted));
        }
        try {
            return result.get();
        } catch (...) {
            return fakeInternalFailure<T>();
        }
    }

    BoundedFakeOperationGate gate_;
    std::optional<Domain::ManagerControlRequest> lastControlRequest_;
    std::optional<Domain::ManagerSettingsPatch> lastSettingsPatch_;
    std::optional<bool> lastApplyImmediately_;
};

class ManagerServerFake final : public Contracts::IManagerServer {
public:
    explicit ManagerServerFake(const Domain::MonotonicTimePoint now = {}) noexcept
        : gate_{now}
    {
    }

    DeterministicResult<void> runResult;

    [[nodiscard]] Domain::Result<void> run(
        const Domain::OperationContext& context) noexcept override
    {
        auto accepted = gate_.enter(context);
        if (!accepted) {
            return accepted;
        }
        try {
            return runResult.get();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        gate_.cancel(operationId);
    }

    void shutdown() noexcept override { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    cancelledOperation() const noexcept
    {
        return gate_.cancelledOperation();
    }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return gate_.lastContext();
    }

private:
    BoundedFakeOperationGate gate_;
};

} // namespace ForgeConductor::Tests::Fakes
