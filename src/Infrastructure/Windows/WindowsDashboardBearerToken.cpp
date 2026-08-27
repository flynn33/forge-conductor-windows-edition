#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardBearerToken.h"

#include "Detail/OperationContextGuard.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <atomic>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::string_view LoadAction = "load the dashboard bearer";
constexpr std::string_view CreateAction = "create the dashboard bearer";

class SensitiveBytes final {
public:
    explicit SensitiveBytes(std::vector<std::byte> bytes) noexcept
        : bytes_{std::move(bytes)}
    {
    }

    ~SensitiveBytes() noexcept
    {
        if (!bytes_.empty()) {
            ::SecureZeroMemory(bytes_.data(), bytes_.size());
        }
    }

    SensitiveBytes(const SensitiveBytes&) = delete;
    SensitiveBytes& operator=(const SensitiveBytes&) = delete;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

// The read result may contain raw secret material even when cancellation,
// expiry, or shutdown wins immediately after the dependency returns. This
// guard erases any bytes that were not moved into SensitiveBytes.
class SecureStorageReadWiper final {
public:
    explicit SecureStorageReadWiper(
        Domain::Result<std::optional<std::vector<std::byte>>>& result) noexcept
        : result_{result}
    {
    }

    ~SecureStorageReadWiper() noexcept
    {
        if (result_ && result_.value().has_value()) {
            auto& bytes = result_.value().value();
            if (!bytes.empty()) {
                ::SecureZeroMemory(bytes.data(), bytes.size());
            }
        }
    }

    SecureStorageReadWiper(const SecureStorageReadWiper&) = delete;
    SecureStorageReadWiper& operator=(const SecureStorageReadWiper&) = delete;

private:
    Domain::Result<std::optional<std::vector<std::byte>>>& result_;
};

// std::atomic_flag is guaranteed lock-free. The lease rejects instead of
// waiting, and shutdown never observes or waits on the admission flag.
class SingleAdmissionLease final {
public:
    explicit SingleAdmissionLease(std::atomic_flag& admitted) noexcept
        : admitted_{admitted}
    {
        owns_ = !admitted_.test_and_set(std::memory_order_acquire);
    }

    ~SingleAdmissionLease() noexcept
    {
        if (owns_) {
            admitted_.clear(std::memory_order_release);
        }
    }

    SingleAdmissionLease(const SingleAdmissionLease&) = delete;
    SingleAdmissionLease& operator=(const SingleAdmissionLease&) = delete;

    [[nodiscard]] bool owns() const noexcept { return owns_; }

private:
    std::atomic_flag& admitted_;
    bool owns_{};
};

[[nodiscard]] Domain::Error closedError()
{
    return Domain::makeError(
        Domain::ErrorCodes::TransportClosed,
        "Dashboard bearer storage is closed.");
}

[[nodiscard]] Domain::Error admissionError()
{
    return Domain::makeError(
        Domain::ErrorCodes::LimitExceeded,
        "Dashboard bearer storage already has its single permitted operation in progress.",
        true);
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    return Detail::validateOperationContext(
        context, std::chrono::steady_clock::now(), action);
}

template <typename T>
[[nodiscard]] Domain::Result<T> failure(Domain::Error error)
{
    return Domain::Result<T>::failure(std::move(error));
}

} // namespace

Domain::Result<Contracts::DashboardBearerSecret>
WindowsDashboardBearerTokenGenerator::next() noexcept
{
    try {
        Contracts::DashboardBearerSecret::Bytes raw{};
        const NTSTATUS status = ::BCryptGenRandom(
            nullptr,
            reinterpret_cast<PUCHAR>(raw.data()),
            static_cast<ULONG>(raw.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status)) {
            ::SecureZeroMemory(raw.data(), raw.size());
            return Domain::Result<Contracts::DashboardBearerSecret>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Dashboard bearer generation failed."));
        }

        Contracts::DashboardBearerSecret secret{raw};
        ::SecureZeroMemory(raw.data(), raw.size());
        return Domain::Result<Contracts::DashboardBearerSecret>::success(
            std::move(secret));
    } catch (...) {
        return Domain::Result<Contracts::DashboardBearerSecret>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard bearer generation failed."));
    }
}

WindowsDashboardBearerTokenStore::WindowsDashboardBearerTokenStore(
    Contracts::ISecureStorage& secureStorage,
    Contracts::IDashboardBearerTokenGenerator& generator) noexcept
    : secureStorage_{secureStorage}, generator_{generator}
{
}

WindowsDashboardBearerTokenStore::~WindowsDashboardBearerTokenStore()
{
    shutdown();
}

Domain::Result<std::optional<Domain::Sha256Digest>>
WindowsDashboardBearerTokenStore::loadUnlocked(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto contextResult = validateContext(context, LoadAction);
        if (!contextResult) {
            return failure<std::optional<Domain::Sha256Digest>>(
                std::move(contextResult).error());
        }

        auto stored = secureStorage_.get(
            StorageKey, Contracts::DashboardBearerSecret::SizeBytes, context);
        const SecureStorageReadWiper storedWiper{stored};
        contextResult = validateContext(context, LoadAction);
        if (!contextResult) {
            return failure<std::optional<Domain::Sha256Digest>>(
                std::move(contextResult).error());
        }
        if (shutdownRequested_.test(std::memory_order_acquire)) {
            return failure<std::optional<Domain::Sha256Digest>>(closedError());
        }
        if (!stored) {
            return failure<std::optional<Domain::Sha256Digest>>(
                std::move(stored).error());
        }
        if (!stored.value().has_value()) {
            return Domain::Result<std::optional<Domain::Sha256Digest>>::success(
                std::nullopt);
        }

        SensitiveBytes raw{std::move(stored).value().value()};
        if (raw.bytes().size() != Contracts::DashboardBearerSecret::SizeBytes) {
            return failure<std::optional<Domain::Sha256Digest>>(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Stored dashboard bearer material has an invalid length."));
        }

        Contracts::DashboardBearerSecret::Bytes bytes{};
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            bytes[index] = raw.bytes()[index];
        }
        Contracts::DashboardBearerSecret secret{bytes};
        ::SecureZeroMemory(bytes.data(), bytes.size());
        auto encoded = Contracts::encodeDashboardBearerToken(secret);
        if (!encoded) {
            return failure<std::optional<Domain::Sha256Digest>>(
                std::move(encoded).error());
        }
        return Domain::Result<std::optional<Domain::Sha256Digest>>::success(
            std::optional<Domain::Sha256Digest>{std::move(encoded).value()});
    } catch (...) {
        return Domain::Result<std::optional<Domain::Sha256Digest>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard bearer loading failed."));
    }
}

Domain::Result<std::optional<Domain::Sha256Digest>>
WindowsDashboardBearerTokenStore::load(
    const Domain::OperationContext& context) noexcept
{
    try {
        if (shutdownRequested_.test(std::memory_order_acquire)) {
            return failure<std::optional<Domain::Sha256Digest>>(closedError());
        }
        auto contextResult = validateContext(context, LoadAction);
        if (!contextResult) {
            return failure<std::optional<Domain::Sha256Digest>>(
                std::move(contextResult).error());
        }

        const SingleAdmissionLease admission{operationAdmitted_};
        if (!admission.owns()) {
            return failure<std::optional<Domain::Sha256Digest>>(admissionError());
        }
        if (shutdownRequested_.test(std::memory_order_acquire)) {
            return failure<std::optional<Domain::Sha256Digest>>(closedError());
        }
        return loadUnlocked(context);
    } catch (...) {
        return Domain::Result<std::optional<Domain::Sha256Digest>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard bearer loading failed."));
    }
}

Domain::Result<Domain::Sha256Digest>
WindowsDashboardBearerTokenStore::loadOrCreate(
    const Domain::OperationContext& context) noexcept
{
    try {
        if (shutdownRequested_.test(std::memory_order_acquire)) {
            return failure<Domain::Sha256Digest>(closedError());
        }
        auto contextResult = validateContext(context, CreateAction);
        if (!contextResult) {
            return failure<Domain::Sha256Digest>(std::move(contextResult).error());
        }

        const SingleAdmissionLease admission{operationAdmitted_};
        if (!admission.owns()) {
            return failure<Domain::Sha256Digest>(admissionError());
        }
        if (shutdownRequested_.test(std::memory_order_acquire)) {
            return failure<Domain::Sha256Digest>(closedError());
        }

        auto loaded = loadUnlocked(context);
        if (!loaded) {
            return failure<Domain::Sha256Digest>(std::move(loaded).error());
        }
        if (loaded.value().has_value()) {
            return Domain::Result<Domain::Sha256Digest>::success(
                std::move(loaded).value().value());
        }

        contextResult = validateContext(context, CreateAction);
        if (!contextResult) {
            return failure<Domain::Sha256Digest>(std::move(contextResult).error());
        }
        auto generated = generator_.next();
        if (!generated) {
            return failure<Domain::Sha256Digest>(std::move(generated).error());
        }

        auto token = Contracts::encodeDashboardBearerToken(generated.value());
        if (!token) {
            return token;
        }
        contextResult = validateContext(context, CreateAction);
        if (!contextResult) {
            return failure<Domain::Sha256Digest>(std::move(contextResult).error());
        }

        auto persisted = secureStorage_.put(
            StorageKey, generated.value().bytes(), context);
        if (!persisted) {
            return failure<Domain::Sha256Digest>(std::move(persisted).error());
        }
        return token;
    } catch (...) {
        return Domain::Result<Domain::Sha256Digest>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard bearer creation failed."));
    }
}

void WindowsDashboardBearerTokenStore::shutdown() noexcept
{
    static_cast<void>(shutdownRequested_.test_and_set(std::memory_order_release));
}

} // namespace ForgeConductor::Infrastructure::Windows
