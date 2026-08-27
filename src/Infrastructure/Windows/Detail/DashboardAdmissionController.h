#pragma once

#include "ForgeConductor/Dashboard/DashboardTransportLimits.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class DashboardAdmissionState;

enum class DashboardAdmissionKind : std::uint8_t {
    Short,
    ServerSentEvents,
};

// Immutable diagnostic observation of the single shared admission owner.
class DashboardAdmissionSnapshot final {
public:
    DashboardAdmissionSnapshot(const DashboardAdmissionSnapshot&) = default;
    DashboardAdmissionSnapshot(DashboardAdmissionSnapshot&&) noexcept = default;
    DashboardAdmissionSnapshot& operator=(
        const DashboardAdmissionSnapshot&) = delete;
    DashboardAdmissionSnapshot& operator=(
        DashboardAdmissionSnapshot&&) = delete;

    [[nodiscard]] std::size_t shortConnectionCount() const noexcept
    {
        return shortConnectionCount_;
    }

    [[nodiscard]] std::size_t sseConnectionCount() const noexcept
    {
        return sseConnectionCount_;
    }

    [[nodiscard]] std::size_t totalConnectionCount() const noexcept
    {
        return shortConnectionCount_ + sseConnectionCount_;
    }

    [[nodiscard]] const Dashboard::DashboardTransportLimits& limits()
        const noexcept
    {
        return limits_;
    }

    [[nodiscard]] bool withinLimits() const noexcept
    {
        return shortConnectionCount_ <= limits_.maximumShortConnections &&
            sseConnectionCount_ <= limits_.maximumSseConnections &&
            totalConnectionCount() <= limits_.maximumTotalConnections;
    }

private:
    friend class DashboardAdmissionController;

    DashboardAdmissionSnapshot(
        const std::size_t shortConnectionCount,
        const std::size_t sseConnectionCount,
        Dashboard::DashboardTransportLimits limits) noexcept
        : shortConnectionCount_{shortConnectionCount},
          sseConnectionCount_{sseConnectionCount},
          limits_{limits}
    {
    }

    std::size_t shortConnectionCount_{};
    std::size_t sseConnectionCount_{};
    Dashboard::DashboardTransportLimits limits_;
};

// The composition root owns exactly one controller per dashboard runtime and
// injects it into active and retiring listener generations. This is an
// ownership/injection responsibility, not a process-global singleton. Leases
// retain only its bounded accounting state, so generation retirement cannot
// invalidate a live connection's final release.
class DashboardAdmissionController final {
public:
    // A lease belongs to one serialized connection state machine. Moving,
    // converting, releasing, destroying, or inspecting the same Lease may not
    // race. Controller operations performed through distinct leases are
    // thread-safe.
    class Lease final {
    public:
        ~Lease() noexcept;

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        [[nodiscard]] Domain::Result<void> convertToSse() noexcept;
        void release() noexcept;

        [[nodiscard]] bool ownsAdmission() const noexcept
        {
            return state_ != nullptr;
        }

        [[nodiscard]] DashboardAdmissionKind kind() const noexcept
        {
            return kind_;
        }

    private:
        friend class DashboardAdmissionController;

        explicit Lease(
            std::shared_ptr<DashboardAdmissionState> state) noexcept;

        std::shared_ptr<DashboardAdmissionState> state_;
        DashboardAdmissionKind kind_{DashboardAdmissionKind::Short};
    };

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardAdmissionController>>
    create(
        Dashboard::DashboardTransportLimits limits = {}) noexcept;

    ~DashboardAdmissionController() noexcept = default;

    DashboardAdmissionController(const DashboardAdmissionController&) = delete;
    DashboardAdmissionController& operator=(
        const DashboardAdmissionController&) = delete;
    DashboardAdmissionController(DashboardAdmissionController&&) = delete;
    DashboardAdmissionController& operator=(
        DashboardAdmissionController&&) = delete;

    // Reserves one unclassified/short slot immediately. Capacity failure is
    // retryable and never creates a waiter or queued admission request.
    [[nodiscard]] Domain::Result<Lease> tryAccept() const noexcept;

    [[nodiscard]] Domain::Result<DashboardAdmissionSnapshot> snapshot()
        const noexcept;

    [[nodiscard]] const Dashboard::DashboardTransportLimits& limits()
        const noexcept;

private:
    explicit DashboardAdmissionController(
        std::shared_ptr<DashboardAdmissionState> state) noexcept;

    std::shared_ptr<DashboardAdmissionState> state_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
