#pragma once

#include "ForgeConductor/Contracts/ISecureStorage.h"
#include "BoundaryFakeSupport.h"
#include "DeterministicResult.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

enum class SecureStorageCall {
    Put,
    Get,
    Remove
};

struct SecureStorageCapture final {
    SecureStorageCall call;
    std::string key;
    std::size_t requestedKeyBytes{};
    std::size_t requestedSecretBytes{};
    std::size_t requestedMaximumBytes{};
    std::vector<std::byte> capturedSecret;
};

class RecordingSecureStorageFake final
    : public Contracts::ISecureStorage {
public:
    explicit RecordingSecureStorageFake(
        const std::size_t captureBytesMaximum =
            DefaultBoundaryCaptureBytesMaximum,
        const std::size_t captureTextBytesMaximum =
            DefaultBoundaryCaptureTextBytesMaximum) noexcept
        : captureBytesMaximum_{captureBytesMaximum},
          captureTextBytesMaximum_{captureTextBytesMaximum}
    {
    }

    DeterministicResult<void> putResult;
    DeterministicResult<std::optional<std::vector<std::byte>>> getResult;
    DeterministicResult<void> removeResult;

    [[nodiscard]] Domain::Result<void> put(
        const std::string_view key,
        const std::span<const std::byte> secret,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return gate;
            }
            const auto capturedKeyBytes =
                (std::min)(key.size(), captureTextBytesMaximum_);
            const auto capturedSecretBytes =
                (std::min)(secret.size(), captureBytesMaximum_);
            lastCapture_.emplace(SecureStorageCapture{
                SecureStorageCall::Put,
                std::string{key.substr(0, capturedKeyBytes)},
                key.size(),
                secret.size(),
                0,
                std::vector<std::byte>{
                    secret.begin(),
                    secret.begin() + capturedSecretBytes}});
            if (key.size() > captureTextBytesMaximum_ ||
                secret.size() > captureBytesMaximum_) {
                return Domain::Result<void>::failure(boundaryPayloadTooLarge(
                    "The secure-storage put exceeds its capture bound."));
            }
            return putResult.get();
        } catch (...) {
            return voidFailure(
                "The deterministic secure-storage put could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<std::optional<std::vector<std::byte>>> get(
        const std::string_view key,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<
                    std::optional<std::vector<std::byte>>>::failure(
                        std::move(gate).error());
            }
            captureKey(SecureStorageCall::Get, key, maximumBytes);
            if (key.size() > captureTextBytesMaximum_) {
                return optionalBytesFailure(
                    "The secure-storage key exceeds its capture bound.");
            }
            auto result = getResult.get();
            if (result && result.value() &&
                (result.value()->size() > maximumBytes ||
                 result.value()->size() > captureBytesMaximum_)) {
                return optionalBytesFailure(
                    "The scripted secret exceeds its output bound.");
            }
            return result;
        } catch (...) {
            return Domain::Result<
                std::optional<std::vector<std::byte>>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InternalFailure,
                        "The deterministic secure-storage get could not be captured."));
        }
    }

    [[nodiscard]] Domain::Result<void> remove(
        const std::string_view key,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return gate;
            }
            captureKey(SecureStorageCall::Remove, key, 0);
            if (key.size() > captureTextBytesMaximum_) {
                return Domain::Result<void>::failure(boundaryPayloadTooLarge(
                    "The secure-storage key exceeds its capture bound."));
            }
            return removeResult.get();
        } catch (...) {
            return voidFailure(
                "The deterministic secure-storage remove could not be captured.");
        }
    }

    void shutdown() noexcept override { state_.shutdown(); }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] bool isShutdown() const noexcept { return state_.isShutdown(); }
    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<SecureStorageCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

private:
    void captureKey(
        const SecureStorageCall call,
        const std::string_view key,
        const std::size_t maximumBytes)
    {
        const auto capturedKeyBytes =
            (std::min)(key.size(), captureTextBytesMaximum_);
        lastCapture_.emplace(SecureStorageCapture{
            call,
            std::string{key.substr(0, capturedKeyBytes)},
            key.size(),
            0,
            maximumBytes,
            {}});
    }

    [[nodiscard]] static Domain::Result<void> voidFailure(
        const char* message)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, message));
    }

    [[nodiscard]] static
    Domain::Result<std::optional<std::vector<std::byte>>> optionalBytesFailure(
        const char* message)
    {
        return Domain::Result<
            std::optional<std::vector<std::byte>>>::failure(
                boundaryPayloadTooLarge(message));
    }

    DeterministicBoundaryState state_;
    std::size_t captureBytesMaximum_;
    std::size_t captureTextBytesMaximum_;
    std::optional<SecureStorageCapture> lastCapture_;
};

} // namespace ForgeConductor::Tests::Fakes
