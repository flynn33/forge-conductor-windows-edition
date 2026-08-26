#pragma once

#include "ForgeConductor/Domain/Domain.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ForgeConductor::Contracts {

class IClock {
public:
    virtual ~IClock() = default;

    [[nodiscard]] virtual Domain::UtcTimePoint utcNow() const noexcept = 0;
    [[nodiscard]] virtual Domain::MonotonicTimePoint monotonicNow() const noexcept = 0;
};

class IUuidGenerator {
public:
    virtual ~IUuidGenerator() = default;

    [[nodiscard]] virtual Domain::Result<Domain::Uuid> next() noexcept = 0;
};

class IHasher {
public:
    virtual ~IHasher() = default;

    [[nodiscard]] virtual Domain::Result<Domain::Sha256Digest> sha256(
        std::span<const std::byte> bytes) noexcept = 0;
};

class IRedactor {
public:
    virtual ~IRedactor() = default;

    [[nodiscard]] virtual Domain::Result<std::string> redact(
        std::string_view value) noexcept = 0;
};

class IDeadlineScheduler {
public:
    virtual ~IDeadlineScheduler() = default;

    [[nodiscard]] virtual Domain::Result<void> waitUntil(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
