#include "DashboardAdmissionController.h"

#include <atomic>
#include <exception>
#include <limits>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr std::uint64_t CountMask =
    (std::numeric_limits<std::uint32_t>::max)();
constexpr unsigned SseCountShift = 32U;

[[nodiscard]] constexpr std::size_t shortCount(
    const std::uint64_t packed) noexcept
{
    return static_cast<std::size_t>(packed & CountMask);
}

[[nodiscard]] constexpr std::size_t sseCount(
    const std::uint64_t packed) noexcept
{
    return static_cast<std::size_t>(packed >> SseCountShift);
}

[[nodiscard]] constexpr std::uint64_t packCounts(
    const std::size_t shortConnections,
    const std::size_t sseConnections) noexcept
{
    return static_cast<std::uint64_t>(shortConnections) |
        (static_cast<std::uint64_t>(sseConnections) << SseCountShift);
}

[[nodiscard]] Domain::Error capacityError(const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::LimitExceeded,
        std::string{message},
        true);
}

[[nodiscard]] Domain::Error invalidLeaseError(const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        std::string{message});
}

[[nodiscard]] Domain::Error internalError(const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        std::string{message});
}

} // namespace

class DashboardAdmissionState final {
public:
    explicit DashboardAdmissionState(
        Dashboard::DashboardTransportLimits limits) noexcept
        : limits_{limits}
    {
    }

    [[nodiscard]] bool tryReserveShort() noexcept
    {
        auto observed = counts_.load(std::memory_order_acquire);
        for (;;) {
            const auto shortConnections = shortCount(observed);
            const auto sseConnections = sseCount(observed);
            if (shortConnections >= limits_.maximumShortConnections ||
                shortConnections + sseConnections >=
                    limits_.maximumTotalConnections) {
                return false;
            }
            const auto desired = packCounts(
                shortConnections + 1U, sseConnections);
            if (counts_.compare_exchange_weak(
                    observed,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    [[nodiscard]] bool tryConvertShortToSse() noexcept
    {
        auto observed = counts_.load(std::memory_order_acquire);
        for (;;) {
            const auto shortConnections = shortCount(observed);
            const auto sseConnections = sseCount(observed);
            if (sseConnections >= limits_.maximumSseConnections) {
                return false;
            }
            if (shortConnections == 0U) {
                std::terminate();
            }
            const auto desired = packCounts(
                shortConnections - 1U, sseConnections + 1U);
            if (counts_.compare_exchange_weak(
                    observed,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    void release(const DashboardAdmissionKind kind) noexcept
    {
        auto observed = counts_.load(std::memory_order_acquire);
        for (;;) {
            const auto shortConnections = shortCount(observed);
            const auto sseConnections = sseCount(observed);
            if ((kind == DashboardAdmissionKind::Short &&
                 shortConnections == 0U) ||
                (kind == DashboardAdmissionKind::ServerSentEvents &&
                 sseConnections == 0U)) {
                std::terminate();
            }
            const auto desired = kind == DashboardAdmissionKind::Short
                ? packCounts(shortConnections - 1U, sseConnections)
                : packCounts(shortConnections, sseConnections - 1U);
            if (counts_.compare_exchange_weak(
                    observed,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return;
            }
        }
    }

    [[nodiscard]] std::pair<std::size_t, std::size_t> counts()
        const noexcept
    {
        const auto packed = counts_.load(std::memory_order_acquire);
        return {shortCount(packed), sseCount(packed)};
    }

    [[nodiscard]] const Dashboard::DashboardTransportLimits& limits()
        const noexcept
    {
        return limits_;
    }

private:
    static_assert(
        std::atomic<std::uint64_t>::is_always_lock_free,
        "Dashboard admission requires lock-free 64-bit accounting.");

    const Dashboard::DashboardTransportLimits limits_;
    std::atomic<std::uint64_t> counts_{};
};

DashboardAdmissionController::Lease::Lease(
    std::shared_ptr<DashboardAdmissionState> state) noexcept
    : state_{std::move(state)}
{
}

DashboardAdmissionController::Lease::~Lease() noexcept
{
    release();
}

DashboardAdmissionController::Lease::Lease(Lease&& other) noexcept
    : state_{std::move(other.state_)}, kind_{other.kind_}
{
    other.kind_ = DashboardAdmissionKind::Short;
}

DashboardAdmissionController::Lease&
DashboardAdmissionController::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        kind_ = other.kind_;
        other.kind_ = DashboardAdmissionKind::Short;
    }
    return *this;
}

Domain::Result<void>
DashboardAdmissionController::Lease::convertToSse() noexcept
{
    try {
        if (!state_) {
            return Domain::Result<void>::failure(invalidLeaseError(
                "A released dashboard admission cannot become an SSE stream."));
        }
        if (kind_ != DashboardAdmissionKind::Short) {
            return Domain::Result<void>::failure(invalidLeaseError(
                "The dashboard admission is already an SSE stream."));
        }
        if (!state_->tryConvertShortToSse()) {
            return Domain::Result<void>::failure(capacityError(
                "Dashboard SSE stream capacity is exhausted."));
        }
        kind_ = DashboardAdmissionKind::ServerSentEvents;
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalError(
            "Dashboard admission conversion failed safely."));
    }
}

void DashboardAdmissionController::Lease::release() noexcept
{
    if (!state_) {
        return;
    }
    state_->release(kind_);
    state_.reset();
    kind_ = DashboardAdmissionKind::Short;
}

DashboardAdmissionController::DashboardAdmissionController(
    std::shared_ptr<DashboardAdmissionState> state) noexcept
    : state_{std::move(state)}
{
}

Domain::Result<std::unique_ptr<DashboardAdmissionController>>
DashboardAdmissionController::create(
    Dashboard::DashboardTransportLimits limits) noexcept
{
    try {
        auto valid = limits.validate();
        if (!valid) {
            return Domain::Result<
                std::unique_ptr<DashboardAdmissionController>>::failure(
                std::move(valid).error());
        }
        auto state = std::make_shared<DashboardAdmissionState>(limits);
        return Domain::Result<
            std::unique_ptr<DashboardAdmissionController>>::success(
            std::unique_ptr<DashboardAdmissionController>{
                new DashboardAdmissionController{std::move(state)}});
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<DashboardAdmissionController>>::failure(
            internalError(
                "Dashboard admission state could not be allocated."));
    }
}

Domain::Result<DashboardAdmissionController::Lease>
DashboardAdmissionController::tryAccept() const noexcept
{
    try {
        if (!state_->tryReserveShort()) {
            return Domain::Result<Lease>::failure(capacityError(
                "Dashboard short-request capacity is exhausted."));
        }
        return Domain::Result<Lease>::success(Lease{state_});
    } catch (...) {
        return Domain::Result<Lease>::failure(internalError(
            "Dashboard connection admission failed safely."));
    }
}

Domain::Result<DashboardAdmissionSnapshot>
DashboardAdmissionController::snapshot() const noexcept
{
    try {
        const auto [shortConnections, sseConnections] = state_->counts();
        return Domain::Result<DashboardAdmissionSnapshot>::success(
            DashboardAdmissionSnapshot{
                shortConnections,
                sseConnections,
                state_->limits()});
    } catch (...) {
        return Domain::Result<DashboardAdmissionSnapshot>::failure(
            internalError(
                "Dashboard admission diagnostics could not be captured."));
    }
}

const Dashboard::DashboardTransportLimits&
DashboardAdmissionController::limits() const noexcept
{
    return state_->limits();
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
