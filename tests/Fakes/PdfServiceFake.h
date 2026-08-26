#pragma once

#include "ForgeConductor/Contracts/INativeToolServices.h"
#include "BoundaryFakeSupport.h"
#include "DeterministicResult.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Tests::Fakes {

enum class PdfServiceCall {
    Write,
    FromTextFile
};

struct PdfServiceCapture final {
    PdfServiceCall call;
    Contracts::AuthorizedPath primary;
    std::optional<Contracts::AuthorizedPath> secondary;
    std::string title;
    std::string body;
    std::size_t requestedTextBytes{};
};

class RecordingPdfServiceFake final
    : public Contracts::IPdfService {
public:
    explicit RecordingPdfServiceFake(
        const std::size_t captureTextBytesMaximum =
            DefaultBoundaryCaptureTextBytesMaximum) noexcept
        : captureTextBytesMaximum_{captureTextBytesMaximum}
    {
    }

    DeterministicResult<Domain::PdfWriteReceipt> writeResult;
    DeterministicResult<Domain::PdfWriteReceipt> fromTextFileResult;

    [[nodiscard]] Domain::Result<Domain::PdfWriteReceipt> write(
        const std::string_view title,
        const std::string_view body,
        const Contracts::AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::PdfWriteReceipt>::failure(
                    std::move(gate).error());
            }
            const auto requestedBytes = title.size() + body.size();
            const auto capturedTitleBytes =
                (std::min)(title.size(), captureTextBytesMaximum_);
            const auto remaining =
                captureTextBytesMaximum_ - capturedTitleBytes;
            const auto capturedBodyBytes =
                (std::min)(body.size(), remaining);
            lastCapture_.emplace(PdfServiceCapture{
                PdfServiceCall::Write,
                destination,
                std::nullopt,
                std::string{title.substr(0, capturedTitleBytes)},
                std::string{body.substr(0, capturedBodyBytes)},
                requestedBytes});
            if (requestedBytes > captureTextBytesMaximum_) {
                return Domain::Result<Domain::PdfWriteReceipt>::failure(
                    boundaryPayloadTooLarge(
                        "The PDF text exceeds its capture bound."));
            }
            return writeResult.get();
        } catch (...) {
            return failure(
                "The deterministic PDF write could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::PdfWriteReceipt> fromTextFile(
        const std::string_view title,
        const Contracts::AuthorizedPath& source,
        const Contracts::AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::PdfWriteReceipt>::failure(
                    std::move(gate).error());
            }
            lastCapture_.emplace(PdfServiceCapture{
                PdfServiceCall::FromTextFile,
                source,
                std::optional<Contracts::AuthorizedPath>{destination},
                std::string{title},
                {},
                0});
            return fromTextFileResult.get();
        } catch (...) {
            return failure(
                "The deterministic PDF conversion could not be captured.");
        }
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<PdfServiceCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

private:
    [[nodiscard]] static Domain::Result<Domain::PdfWriteReceipt> failure(
        const char* message)
    {
        return Domain::Result<Domain::PdfWriteReceipt>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, message));
    }

    DeterministicBoundaryState state_;
    std::size_t captureTextBytesMaximum_;
    std::optional<PdfServiceCapture> lastCapture_;
};

} // namespace ForgeConductor::Tests::Fakes
