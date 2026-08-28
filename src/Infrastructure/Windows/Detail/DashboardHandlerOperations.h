#pragma once

#include "ForgeConductor/Dashboard/IDashboardConnectionApplication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// One-shot handler-pool operation for either an accepted canonical request or
// a canonical parser rejection. The operational-state value is captured when
// the request leaves the IOCP boundary and cannot change while it executes.
class DashboardPrepareHandlerOperation final
    : public IDashboardHandlerOperation {
public:
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardPrepareHandlerOperation>>
    createRequest(
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application,
        Dashboard::DashboardHttpRequest request,
        bool operationalServiceActive) noexcept;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardPrepareHandlerOperation>>
    createRejection(Dashboard::DashboardHttpRejection rejection) noexcept;

    ~DashboardPrepareHandlerOperation() noexcept override = default;

    DashboardPrepareHandlerOperation(
        const DashboardPrepareHandlerOperation&) = delete;
    DashboardPrepareHandlerOperation& operator=(
        const DashboardPrepareHandlerOperation&) = delete;
    DashboardPrepareHandlerOperation(
        DashboardPrepareHandlerOperation&&) = delete;
    DashboardPrepareHandlerOperation& operator=(
        DashboardPrepareHandlerOperation&&) = delete;

    [[nodiscard]] DashboardHandlerCompletionKind completionKind()
        const noexcept override;

    [[nodiscard]] DashboardHandlerCompletion execute(
        const Domain::OperationContext& context) override;

private:
    DashboardPrepareHandlerOperation(
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application,
        Dashboard::DashboardHttpRequest request,
        bool operationalServiceActive);

    explicit DashboardPrepareHandlerOperation(
        Dashboard::DashboardHttpRejection rejection);

    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application_;
    std::unique_ptr<Dashboard::DashboardHttpRequest> request_;
    std::unique_ptr<Dashboard::DashboardHttpRejection> rejection_;
    bool operationalServiceActive_{};
    bool executed_{};
};

// One-shot handler-pool operation for the sole defined post-delivery action.
// It strongly retains the application until the action finishes and never
// permits None or an unknown action to enter the executor.
class DashboardPostDeliveryHandlerOperation final
    : public IDashboardHandlerOperation {
public:
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardPostDeliveryHandlerOperation>>
    create(
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application,
        Dashboard::DashboardPostDeliveryAction action) noexcept;

    ~DashboardPostDeliveryHandlerOperation() noexcept override = default;

    DashboardPostDeliveryHandlerOperation(
        const DashboardPostDeliveryHandlerOperation&) = delete;
    DashboardPostDeliveryHandlerOperation& operator=(
        const DashboardPostDeliveryHandlerOperation&) = delete;
    DashboardPostDeliveryHandlerOperation(
        DashboardPostDeliveryHandlerOperation&&) = delete;
    DashboardPostDeliveryHandlerOperation& operator=(
        DashboardPostDeliveryHandlerOperation&&) = delete;

    [[nodiscard]] DashboardHandlerCompletionKind completionKind()
        const noexcept override;

    [[nodiscard]] DashboardHandlerCompletion execute(
        const Domain::OperationContext& context) override;

private:
    DashboardPostDeliveryHandlerOperation(
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application,
        Dashboard::DashboardPostDeliveryAction action) noexcept;

    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application_;
    Dashboard::DashboardPostDeliveryAction action_{
        Dashboard::DashboardPostDeliveryAction::None};
    bool executed_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
