#include "DashboardIocpCompletionRouter.h"

#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error routerError(
    const std::string_view code,
    const std::string_view message,
    const bool retryable = false)
{
    try {
        return Domain::makeError(
            std::string{code}, std::string{message}, retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard IOCP routing failed safely.");
    }
}

void incrementSaturating(std::uint64_t& value) noexcept
{
    if (value != (std::numeric_limits<std::uint64_t>::max)()) {
        ++value;
    }
}

} // namespace

DashboardIocpCompletionRouterSnapshot::
DashboardIocpCompletionRouterSnapshot(
    const std::size_t fixedTargetCount,
    const std::size_t maximumFixedTargetCount,
    const std::uint64_t fixedDispatchCount,
    const std::uint64_t fallbackDispatchCount,
    const std::uint64_t fatalNotificationCount,
    const bool registrationOpen,
    const bool shutdownRequested,
    const std::optional<DWORD> fatalNativeError) noexcept
    : fixedTargetCount_{fixedTargetCount},
      maximumFixedTargetCount_{maximumFixedTargetCount},
      fixedDispatchCount_{fixedDispatchCount},
      fallbackDispatchCount_{fallbackDispatchCount},
      fatalNotificationCount_{fatalNotificationCount},
      registrationOpen_{registrationOpen},
      shutdownRequested_{shutdownRequested},
      fatalNativeError_{fatalNativeError}
{
}

DashboardIocpCompletionRouter::DashboardIocpCompletionRouter(
    const DashboardIoCompletionKey fallbackOwnedCompletionKey,
    std::shared_ptr<IDashboardIocpCompletionSink> fallback) noexcept
    : fallbackOwnedCompletionKey_{fallbackOwnedCompletionKey},
      fallback_{std::move(fallback)}
{
}

Domain::Result<std::shared_ptr<DashboardIocpCompletionRouter>>
DashboardIocpCompletionRouter::create(
    const DashboardIoCompletionKey fallbackOwnedCompletionKey,
    std::shared_ptr<IDashboardIocpCompletionSink> fallback) noexcept
{
    using CreateResult =
        Domain::Result<std::shared_ptr<DashboardIocpCompletionRouter>>;
    if (fallbackOwnedCompletionKey.value() == 0U ||
        fallbackOwnedCompletionKey.value() ==
            DashboardIocpWorkerKernel::ShutdownKeyValue ||
        fallback == nullptr) {
        return CreateResult::failure(routerError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard IOCP router requires a fallback registry and its nonreserved immutable completion key."));
    }
    try {
        return CreateResult::success(
            std::shared_ptr<DashboardIocpCompletionRouter>{
                new DashboardIocpCompletionRouter{
                    fallbackOwnedCompletionKey,
                    std::move(fallback)}});
    } catch (...) {
        return CreateResult::failure(routerError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard IOCP router could not be allocated."));
    }
}

DashboardIocpCompletionRouter::Entry*
DashboardIocpCompletionRouter::findKeyLocked(
    const DashboardIoCompletionKey key) noexcept
{
    for (auto& entry : entries_) {
        if (entry.target != nullptr && entry.key == key) {
            return std::addressof(entry);
        }
    }
    return nullptr;
}

DashboardIocpCompletionRouter::Entry*
DashboardIocpCompletionRouter::findVacantLocked() noexcept
{
    for (auto& entry : entries_) {
        if (entry.target == nullptr) {
            return std::addressof(entry);
        }
    }
    return nullptr;
}

Domain::Result<void>
DashboardIocpCompletionRouter::registerFixedTarget(
    std::shared_ptr<IDashboardFixedIocpCompletionTarget> target) noexcept
{
    try {
        if (target == nullptr || target->completionKey().value() == 0U ||
            target->completionKey().value() ==
                DashboardIocpWorkerKernel::ShutdownKeyValue) {
            return Domain::Result<void>::failure(routerError(
                Domain::ErrorCodes::InvalidRequest,
                "A fixed dashboard IOCP target requires a nonreserved immutable completion key."));
        }

        const auto key = target->completionKey();
        if (key == fallbackOwnedCompletionKey_) {
            return Domain::Result<void>::failure(routerError(
                Domain::ErrorCodes::InvalidRequest,
                "A fixed dashboard IOCP target cannot claim the fallback registry's immutable completion key."));
        }
        const std::scoped_lock lock{mutex_};
        if (!registrationOpen_) {
            return Domain::Result<void>::failure(routerError(
                Domain::ErrorCodes::Conflict,
                "Dashboard fixed completion registration is closed."));
        }
        if (findKeyLocked(key) != nullptr) {
            return Domain::Result<void>::failure(routerError(
                Domain::ErrorCodes::Conflict,
                "The dashboard fixed completion key is already registered."));
        }
        auto* const vacant = findVacantLocked();
        if (vacant == nullptr) {
            return Domain::Result<void>::failure(routerError(
                Domain::ErrorCodes::LimitExceeded,
                "The active, retiring, and overload completion owners already occupy the fixed routing table.",
                true));
        }
        vacant->key = key;
        vacant->target = std::move(target);
        ++fixedTargetCount_;
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(routerError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard fixed completion target could not be registered safely."));
    }
}

bool DashboardIocpCompletionRouter::unregisterFixedTarget(
    const std::shared_ptr<IDashboardFixedIocpCompletionTarget>& target)
    noexcept
{
    if (target == nullptr) {
        return false;
    }
    std::shared_ptr<IDashboardFixedIocpCompletionTarget> released;
    {
        const std::scoped_lock lock{mutex_};
        auto* const entry = findKeyLocked(target->completionKey());
        if (entry == nullptr || entry->target.get() != target.get()) {
            return false;
        }
        released = std::move(entry->target);
        entry->key = DashboardIoCompletionKey{0U};
        --fixedTargetCount_;
    }
    return released != nullptr;
}

void DashboardIocpCompletionRouter::consume(
    const DashboardIoCompletionPacket packet,
    const DWORD nativeError) noexcept
{
    std::shared_ptr<IDashboardFixedIocpCompletionTarget> target;
    {
        const std::scoped_lock lock{mutex_};
        const auto* const entry = findKeyLocked(packet.completionKey);
        if (entry != nullptr) {
            target = entry->target;
            incrementSaturating(fixedDispatchCount_);
        } else {
            incrementSaturating(fallbackDispatchCount_);
        }
    }
    if (target != nullptr) {
        target->consume(packet, nativeError);
        return;
    }
    fallback_->consume(packet, nativeError);
}

void DashboardIocpCompletionRouter::fatal(
    const DWORD nativeError) noexcept
{
    std::array<
        std::shared_ptr<IDashboardFixedIocpCompletionTarget>,
        MaximumFixedTargetCount> targets;
    bool notify{};
    {
        const std::scoped_lock lock{mutex_};
        incrementSaturating(fatalNotificationCount_);
        if (!fatalNativeError_.has_value()) {
            fatalNativeError_.emplace(nativeError);
            registrationOpen_ = false;
            shutdownRequested_ = true;
            for (std::size_t index{}; index < entries_.size(); ++index) {
                targets[index] = entries_[index].target;
            }
            notify = true;
        }
    }
    if (!notify) {
        return;
    }
    for (const auto& target : targets) {
        if (target != nullptr) {
            target->fatal(nativeError);
        }
    }
    fallback_->fatal(nativeError);
}

void DashboardIocpCompletionRouter::beginShutdown() noexcept
{
    std::array<
        std::shared_ptr<IDashboardFixedIocpCompletionTarget>,
        MaximumFixedTargetCount> targets;
    {
        const std::scoped_lock lock{mutex_};
        if (shutdownRequested_) {
            return;
        }
        registrationOpen_ = false;
        shutdownRequested_ = true;
        for (std::size_t index{}; index < entries_.size(); ++index) {
            targets[index] = entries_[index].target;
        }
    }
    for (const auto& target : targets) {
        if (target != nullptr) {
            target->beginShutdown();
        }
    }
}

DashboardIocpCompletionRouterSnapshot
DashboardIocpCompletionRouter::snapshot() const noexcept
{
    const std::scoped_lock lock{mutex_};
    return DashboardIocpCompletionRouterSnapshot{
        fixedTargetCount_,
        MaximumFixedTargetCount,
        fixedDispatchCount_,
        fallbackDispatchCount_,
        fatalNotificationCount_,
        registrationOpen_,
        shutdownRequested_,
        fatalNativeError_};
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
