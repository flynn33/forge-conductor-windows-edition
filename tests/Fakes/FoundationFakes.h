#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IUnicodeCanonicalizer.h"

#include <optional>
#include <cstddef>
#include <mutex>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

class UnicodeCanonicalizerFake final
    : public Contracts::IUnicodeCanonicalizer {
public:
    using Mapping = std::pair<std::string, std::string>;

    explicit UnicodeCanonicalizerFake(
        std::vector<Mapping> mappings = {})
        : mappings_{std::move(mappings)}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::NfcUtf8Key> nfcKey(
        const std::string_view value) const noexcept override
    {
        try {
            if (value.size() > MaximumInputBytes) {
                return Domain::Result<Contracts::NfcUtf8Key>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "The deterministic Unicode input exceeded its bound."));
            }
            for (const auto& [input, key] : mappings_) {
                if (input == value) {
                    return Contracts::NfcUtf8Key::create(key);
                }
            }
            return Contracts::NfcUtf8Key::create(std::string{value});
        } catch (...) {
            return Domain::Result<Contracts::NfcUtf8Key>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The deterministic Unicode canonicalizer failed safely."));
        }
    }

private:
    std::vector<Mapping> mappings_;
};

class FakeClock final : public Contracts::IClock {
public:
    FakeClock(Domain::UtcTimePoint utc, Domain::MonotonicTimePoint monotonic)
        : utc_{utc}, monotonic_{monotonic}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return utc_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return monotonic_;
    }

    template <typename Rep, typename Period>
    void advance(const std::chrono::duration<Rep, Period> by) noexcept
    {
        std::lock_guard lock{mutex_};
        utc_ += by;
        monotonic_ += by;
    }

private:
    mutable std::mutex mutex_;
    Domain::UtcTimePoint utc_;
    Domain::MonotonicTimePoint monotonic_;
};

class SequenceUuidGenerator final : public Contracts::IUuidGenerator {
public:
    explicit SequenceUuidGenerator(std::vector<Domain::Uuid> values)
        : values_{std::move(values)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        std::lock_guard lock{mutex_};
        if (next_ >= values_.size()) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The deterministic UUID sequence is exhausted."));
        }
        return Domain::Result<Domain::Uuid>::success(values_[next_++]);
    }

    [[nodiscard]] std::size_t consumed() const noexcept
    {
        std::lock_guard lock{mutex_};
        return next_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Domain::Uuid> values_;
    std::size_t next_{};
};

class ScriptedHasher final : public Contracts::IHasher {
public:
    explicit ScriptedHasher(Domain::Sha256Digest digest)
        : digest_{std::move(digest)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::Sha256Digest> sha256(
        const std::span<const std::byte> bytes) noexcept override
    {
        lastByteCount_ = bytes.size();
        ++calls_;
        return Domain::Result<Domain::Sha256Digest>::success(digest_);
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
    [[nodiscard]] std::size_t lastByteCount() const noexcept { return lastByteCount_; }

private:
    Domain::Sha256Digest digest_;
    std::size_t calls_{};
    std::size_t lastByteCount_{};
};

class ScriptedRedactor final : public Contracts::IRedactor {
public:
    explicit ScriptedRedactor(const bool rejectPrivateKeys = true)
        : rejectPrivateKeys_{rejectPrivateKeys}
    {
    }

    [[nodiscard]] Domain::Result<std::string> redact(
        const std::string_view value) noexcept override
    {
        try {
            ++calls_;
            lastInputBytes_ = value.size();
            if (rejectPrivateKeys_ && value.find("PRIVATE KEY") != std::string_view::npos) {
                return Domain::Result<std::string>::failure(Domain::makeError(
                    Domain::ErrorCodes::RedactionRejected,
                    "Private-key material was rejected."));
            }
            if (value.find("secret") != std::string_view::npos) {
                return Domain::Result<std::string>::success("<redacted>");
            }
            return Domain::Result<std::string>::success(std::string{value});
        } catch (...) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic redactor could not allocate output."));
        }
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
    [[nodiscard]] std::size_t lastInputBytes() const noexcept { return lastInputBytes_; }

private:
    bool rejectPrivateKeys_{};
    std::size_t calls_{};
    std::size_t lastInputBytes_{};
};

class FakeDeadlineScheduler final : public Contracts::IDeadlineScheduler {
public:
    explicit FakeDeadlineScheduler(
        const Domain::MonotonicTimePoint now = {}) noexcept
        : now_{now}
    {
    }

    [[nodiscard]] Domain::Result<void> waitUntil(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++calls_;
            lastContext_ = context;
            lastDeadline_ = context.deadline;
            lastOperationId_ = context.operationId;
            lastCorrelationId_ = context.correlationId;
            if (shutdown_ || context.isCancellationRequested()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic wait was cancelled."));
            }
            if (context.isExpired(now_)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic wait deadline expired."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic wait could not be recorded."));
        }
    }

    void shutdown() noexcept override { shutdown_ = true; }

    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }

    [[nodiscard]] Domain::MonotonicTimePoint now() const noexcept { return now_; }
    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return lastContext_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint lastDeadline() const noexcept
    {
        return lastDeadline_;
    }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    lastOperationId() const noexcept
    {
        return lastOperationId_;
    }

    [[nodiscard]] const std::optional<Domain::CorrelationId>&
    lastCorrelationId() const noexcept
    {
        return lastCorrelationId_;
    }

private:
    Domain::MonotonicTimePoint now_{};
    Domain::MonotonicTimePoint lastDeadline_{};
    std::size_t calls_{};
    std::optional<Domain::OperationContext> lastContext_;
    std::optional<Domain::OperationId> lastOperationId_;
    std::optional<Domain::CorrelationId> lastCorrelationId_;
    bool shutdown_{};
};

} // namespace ForgeConductor::Tests::Fakes
