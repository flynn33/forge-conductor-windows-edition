#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"

#include "Detail/OperationContextGuard.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::string_view LoadAction = "load the manager authentication token";
constexpr std::string_view CreateAction = "create the manager authentication token";

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

[[nodiscard]] Domain::Error closedError()
{
    return Domain::makeError(Domain::ErrorCodes::TransportClosed,
                             "Manager authentication is closed.");
}

[[nodiscard]] Domain::Error admissionError()
{
    return Domain::makeError(
        Domain::ErrorCodes::LimitExceeded,
        "Manager authentication already has its single permitted operation in progress.",
        true);
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    return Detail::validateOperationContext(context, std::chrono::steady_clock::now(), action);
}

template <typename T>
[[nodiscard]] Domain::Result<T> failure(Domain::Error error)
{
    return Domain::Result<T>::failure(std::move(error));
}

} // namespace

Domain::Result<Contracts::ManagerAuthenticationSecret>
WindowsManagerAuthenticationTokenGenerator::next() noexcept
{
    try {
        Contracts::ManagerAuthenticationSecret::Bytes raw{};
        const NTSTATUS status = ::BCryptGenRandom(
            nullptr,
            reinterpret_cast<PUCHAR>(raw.data()),
            static_cast<ULONG>(raw.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status)) {
            ::SecureZeroMemory(raw.data(), raw.size());
            return Domain::Result<Contracts::ManagerAuthenticationSecret>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                  "Manager authentication token generation failed."));
        }

        Contracts::ManagerAuthenticationSecret secret{raw};
        ::SecureZeroMemory(raw.data(), raw.size());
        return Domain::Result<Contracts::ManagerAuthenticationSecret>::success(
            std::move(secret));
    } catch (...) {
        return Domain::Result<Contracts::ManagerAuthenticationSecret>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Manager authentication token generation failed."));
    }
}

WindowsManagerAuthenticationTokenStore::WindowsManagerAuthenticationTokenStore(
    Contracts::ISecureStorage& secureStorage,
    Contracts::IManagerAuthenticationTokenGenerator& generator) noexcept
    : secureStorage_{secureStorage}, generator_{generator}
{
}

WindowsManagerAuthenticationTokenStore::~WindowsManagerAuthenticationTokenStore()
{
    shutdown();
}

Domain::Result<std::optional<Domain::Sha256Digest>>
WindowsManagerAuthenticationTokenStore::loadUnlocked(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto contextResult = validateContext(context, LoadAction);
        if (!contextResult) {
            return failure<std::optional<Domain::Sha256Digest>>(
                std::move(contextResult).error());
        }

        auto stored = secureStorage_.get(
            StorageKey, Contracts::ManagerAuthenticationSecret::SizeBytes, context);
        if (!stored) {
            return failure<std::optional<Domain::Sha256Digest>>(std::move(stored).error());
        }
        if (!stored.value().has_value()) {
            return Domain::Result<std::optional<Domain::Sha256Digest>>::success(std::nullopt);
        }

        SensitiveBytes raw{std::move(stored).value().value()};
        if (raw.bytes().size() != Contracts::ManagerAuthenticationSecret::SizeBytes) {
            return failure<std::optional<Domain::Sha256Digest>>(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Stored manager authentication material has an invalid length."));
        }

        Contracts::ManagerAuthenticationSecret::Bytes bytes{};
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            bytes[index] = raw.bytes()[index];
        }
        Contracts::ManagerAuthenticationSecret secret{bytes};
        ::SecureZeroMemory(bytes.data(), bytes.size());
        auto encoded = Contracts::encodeManagerAuthenticationToken(secret);
        if (!encoded) {
            return failure<std::optional<Domain::Sha256Digest>>(std::move(encoded).error());
        }
        return Domain::Result<std::optional<Domain::Sha256Digest>>::success(
            std::optional<Domain::Sha256Digest>{std::move(encoded).value()});
    } catch (...) {
        return Domain::Result<std::optional<Domain::Sha256Digest>>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Manager authentication token loading failed."));
    }
}

Domain::Result<std::optional<Domain::Sha256Digest>>
WindowsManagerAuthenticationTokenStore::load(
    const Domain::OperationContext& context) noexcept
{
    try {
        if (shutdownRequested_.load(std::memory_order_acquire)) {
            return failure<std::optional<Domain::Sha256Digest>>(closedError());
        }
        auto contextResult = validateContext(context, LoadAction);
        if (!contextResult) {
            return failure<std::optional<Domain::Sha256Digest>>(
                std::move(contextResult).error());
        }

        std::unique_lock admission{admissionMutex_, std::try_to_lock};
        if (!admission.owns_lock()) {
            return failure<std::optional<Domain::Sha256Digest>>(admissionError());
        }
        if (shutdownRequested_.load(std::memory_order_acquire)) {
            return failure<std::optional<Domain::Sha256Digest>>(closedError());
        }
        return loadUnlocked(context);
    } catch (...) {
        return Domain::Result<std::optional<Domain::Sha256Digest>>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Manager authentication token loading failed."));
    }
}

Domain::Result<Domain::Sha256Digest>
WindowsManagerAuthenticationTokenStore::loadOrCreate(
    const Domain::OperationContext& context) noexcept
{
    try {
        if (shutdownRequested_.load(std::memory_order_acquire)) {
            return failure<Domain::Sha256Digest>(closedError());
        }
        auto contextResult = validateContext(context, CreateAction);
        if (!contextResult) {
            return failure<Domain::Sha256Digest>(std::move(contextResult).error());
        }

        std::unique_lock admission{admissionMutex_, std::try_to_lock};
        if (!admission.owns_lock()) {
            return failure<Domain::Sha256Digest>(admissionError());
        }
        if (shutdownRequested_.load(std::memory_order_acquire)) {
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

        auto token = Contracts::encodeManagerAuthenticationToken(generated.value());
        if (!token) {
            return token;
        }
        contextResult = validateContext(context, CreateAction);
        if (!contextResult) {
            return failure<Domain::Sha256Digest>(std::move(contextResult).error());
        }

        auto persisted = secureStorage_.put(StorageKey, generated.value().bytes(), context);
        if (!persisted) {
            return failure<Domain::Sha256Digest>(std::move(persisted).error());
        }
        return token;
    } catch (...) {
        return Domain::Result<Domain::Sha256Digest>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Manager authentication token creation failed."));
    }
}

void WindowsManagerAuthenticationTokenStore::shutdown() noexcept
{
    shutdownRequested_.store(true, std::memory_order_release);
    try {
        const std::lock_guard admission{admissionMutex_};
    } catch (...) {
        // Shutdown cannot surface exceptions across the process boundary.
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
