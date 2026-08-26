#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "BoundaryFakeSupport.h"
#include "DeterministicResult.h"

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

struct AtomicReadCapture final {
    Contracts::AuthorizedPath path;
    std::size_t maximumBytes{};
};

struct AtomicReplaceCapture final {
    Contracts::AuthorizedPath path;
    std::size_t requestedBytes{};
    std::vector<std::byte> capturedContent;
    bool retainBackup{};
};

class RecordingAtomicFileStoreFake final
    : public Contracts::IAtomicFileStore {
public:
    explicit RecordingAtomicFileStoreFake(
        const std::size_t captureBytesMaximum =
            DefaultBoundaryCaptureBytesMaximum) noexcept
        : captureBytesMaximum_{captureBytesMaximum}
    {
    }

    DeterministicResult<std::vector<std::byte>> readResult;
    DeterministicResult<void> replaceResult;

    [[nodiscard]] Domain::Result<std::vector<std::byte>> read(
        const Contracts::AuthorizedPath& path,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    std::move(gate).error());
            }
            lastRead_.emplace(AtomicReadCapture{path, maximumBytes});
            auto result = readResult.get();
            if (result &&
                (result.value().size() > maximumBytes ||
                 result.value().size() > captureBytesMaximum_)) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    boundaryPayloadTooLarge(
                        "The scripted atomic read exceeds its output bound."));
            }
            return result;
        } catch (...) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The deterministic atomic read could not be captured."));
        }
    }

    [[nodiscard]] Domain::Result<void> replace(
        const Contracts::AuthorizedPath& path,
        const std::span<const std::byte> content,
        const bool retainBackup,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return gate;
            }
            const auto capturedCount =
                (std::min)(content.size(), captureBytesMaximum_);
            lastReplace_.emplace(AtomicReplaceCapture{
                path,
                content.size(),
                std::vector<std::byte>{
                    content.begin(),
                    content.begin() + capturedCount},
                retainBackup});
            if (content.size() > captureBytesMaximum_) {
                return Domain::Result<void>::failure(boundaryPayloadTooLarge(
                    "The atomic replacement exceeds the capture bound."));
            }
            return replaceResult.get();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic atomic replacement could not be captured."));
        }
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }
    [[nodiscard]] std::size_t captureBytesMaximum() const noexcept
    {
        return captureBytesMaximum_;
    }

    [[nodiscard]] const std::optional<AtomicReadCapture>&
    lastRead() const noexcept
    {
        return lastRead_;
    }

    [[nodiscard]] const std::optional<AtomicReplaceCapture>&
    lastReplace() const noexcept
    {
        return lastReplace_;
    }

private:
    DeterministicBoundaryState state_;
    std::size_t captureBytesMaximum_;
    std::optional<AtomicReadCapture> lastRead_;
    std::optional<AtomicReplaceCapture> lastReplace_;
};

} // namespace ForgeConductor::Tests::Fakes
