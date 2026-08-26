#pragma once

#include "ForgeConductor/Contracts/INativeSessionHostServices.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace ForgeConductor::Tests::Fakes {

// A bounded revision-CAS ledger used only by native session-host tests. The
// checksum is an explicit valid placeholder because production checksum bytes
// are owned by the persistence implementation, not this contract fake.
class InMemoryNativeSessionLedger final
    : public Contracts::INativeSessionLedger {
public:
    InMemoryNativeSessionLedger() = default;

    explicit InMemoryNativeSessionLedger(Domain::NativeSessionLedger ledger)
        : ledger_{std::move(ledger)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::NativeSessionLedger> load(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto accepted = accept(context);
            if (!accepted) {
                return Domain::Result<Domain::NativeSessionLedger>::failure(
                    std::move(accepted).error());
            }
            ++loadCalls_;
            auto valid = Domain::validateNativeSessionLedger(ledger_);
            if (!valid) {
                return Domain::Result<Domain::NativeSessionLedger>::failure(
                    std::move(valid).error());
            }
            return Domain::Result<Domain::NativeSessionLedger>::success(ledger_);
        } catch (...) {
            return internal<Domain::NativeSessionLedger>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::NativeSessionLedger> commit(
        const Domain::NativeSessionLedger& ledger,
        const std::uint64_t expectedRevision,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto accepted = accept(context);
            if (!accepted) {
                return Domain::Result<Domain::NativeSessionLedger>::failure(
                    std::move(accepted).error());
            }
            if (expectedRevision != ledger_.revision ||
                ledger.revision != expectedRevision) {
                return Domain::Result<Domain::NativeSessionLedger>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "The in-memory native ledger revision changed.",
                        true));
            }
            auto candidate = ledger;
            ++candidate.revision;
            auto digest = Domain::Sha256Digest::parse(
                "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
            if (!digest) {
                return Domain::Result<Domain::NativeSessionLedger>::failure(
                    std::move(digest).error());
            }
            candidate.contentSha256 = std::move(digest).value();
            auto valid = Domain::validateNativeSessionLedger(candidate);
            if (!valid) {
                return Domain::Result<Domain::NativeSessionLedger>::failure(
                    std::move(valid).error());
            }
            ledger_ = candidate;
            ++commitCalls_;
            return Domain::Result<Domain::NativeSessionLedger>::success(
                std::move(candidate));
        } catch (...) {
            return internal<Domain::NativeSessionLedger>();
        }
    }

    void shutdown() noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            shutdown_ = true;
        } catch (...) {
        }
    }

    [[nodiscard]] Domain::NativeSessionLedger snapshot() const
    {
        std::lock_guard lock{mutex_};
        return ledger_;
    }

    [[nodiscard]] std::size_t loadCalls() const noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return loadCalls_;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] std::size_t commitCalls() const noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return commitCalls_;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] bool isShutdown() const noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return shutdown_;
        } catch (...) {
            return true;
        }
    }

private:
    [[nodiscard]] Domain::Result<void> accept(
        const Domain::OperationContext& context) const
    {
        if (shutdown_ || context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The in-memory native ledger is closed or cancelled."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The in-memory native ledger deadline expired."));
        }
        return Domain::Result<void>::success();
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> internal() noexcept
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The in-memory native ledger failed safely."));
    }

    mutable std::mutex mutex_;
    Domain::NativeSessionLedger ledger_;
    std::size_t loadCalls_{};
    std::size_t commitCalls_{};
    bool shutdown_{};
};

} // namespace ForgeConductor::Tests::Fakes
