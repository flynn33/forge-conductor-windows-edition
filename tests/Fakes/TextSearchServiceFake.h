#pragma once

#include "ForgeConductor/Contracts/INativeToolServices.h"
#include "BoundaryFakeSupport.h"
#include "DeterministicResult.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

struct TextSearchCapture final {
    Contracts::AuthorizedPath root;
    std::string query;
    std::size_t requestedQueryBytes{};
    std::size_t maximumMatches{};
    std::size_t maximumResponseBytes{};
};

class RecordingTextSearchServiceFake final
    : public Contracts::ITextSearchService {
public:
    explicit RecordingTextSearchServiceFake(
        const std::size_t captureItemsMaximum =
            DefaultBoundaryCaptureItemsMaximum,
        const std::size_t captureTextBytesMaximum =
            DefaultBoundaryCaptureTextBytesMaximum,
        const std::size_t outputBytesMaximum =
            DefaultBoundaryCaptureBytesMaximum) noexcept
        : captureItemsMaximum_{captureItemsMaximum},
          captureTextBytesMaximum_{captureTextBytesMaximum},
          outputBytesMaximum_{outputBytesMaximum}
    {
    }

    DeterministicResult<std::vector<std::string>> searchResult;

    [[nodiscard]] Domain::Result<std::vector<std::string>> search(
        const Contracts::AuthorizedPath& root,
        const std::string_view query,
        const std::size_t maximumMatches,
        const std::size_t maximumResponseBytes,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<std::vector<std::string>>::failure(
                    std::move(gate).error());
            }
            const auto capturedQuery = query.substr(
                0,
                (std::min)(query.size(), captureTextBytesMaximum_));
            lastCapture_.emplace(TextSearchCapture{
                root,
                std::string{capturedQuery},
                query.size(),
                maximumMatches,
                maximumResponseBytes});
            if (query.size() > captureTextBytesMaximum_) {
                return stringVectorFailure(
                    "The text-search query exceeds its capture bound.");
            }

            auto result = searchResult.get();
            if (result &&
                (result.value().size() > maximumMatches ||
                 result.value().size() > captureItemsMaximum_ ||
                 textBytes(result.value()) > maximumResponseBytes ||
                 textBytes(result.value()) > outputBytesMaximum_)) {
                return Domain::Result<std::vector<std::string>>::failure(
                    boundaryLimitExceeded(
                        "The scripted text-search response exceeds its bound."));
            }
            return result;
        } catch (...) {
            return Domain::Result<std::vector<std::string>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The deterministic text search could not be captured."));
        }
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<TextSearchCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

private:
    [[nodiscard]] static std::size_t textBytes(
        const std::vector<std::string>& values) noexcept
    {
        std::size_t bytes{};
        for (const auto& value : values) {
            bytes += value.size();
        }
        return bytes;
    }

    [[nodiscard]] static Domain::Result<std::vector<std::string>>
    stringVectorFailure(const char* message)
    {
        return Domain::Result<std::vector<std::string>>::failure(
            boundaryPayloadTooLarge(message));
    }

    DeterministicBoundaryState state_;
    std::size_t captureItemsMaximum_;
    std::size_t captureTextBytesMaximum_;
    std::size_t outputBytesMaximum_;
    std::optional<TextSearchCapture> lastCapture_;
};

} // namespace ForgeConductor::Tests::Fakes
