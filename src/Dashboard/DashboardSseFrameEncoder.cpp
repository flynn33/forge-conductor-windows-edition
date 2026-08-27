#include "ForgeConductor/Dashboard/DashboardSseFrameEncoder.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Dashboard {
namespace {

[[nodiscard]] std::vector<std::byte> ownedBytes(
    std::string encoded)
{
    std::vector<std::byte> bytes(encoded.size());
    if (!encoded.empty()) {
        std::memcpy(bytes.data(), encoded.data(), encoded.size());
    }
    return bytes;
}

} // namespace

Domain::Result<DashboardSseFramePair::ImmutableFrame>
DashboardSseFrameEncoder::encode(
    const std::uint64_t sourceSequence,
    const Domain::TelemetrySnapshot& snapshot,
    const std::optional<double> measuredSampleHz,
    const std::size_t maximumEncodedBytes) noexcept
{
    try {
        auto compact =
            DashboardTelemetryJsonCodec::encodeCompactServerSentEvent(
                snapshot,
                measuredSampleHz,
                maximumEncodedBytes);
        if (!compact) {
            return Domain::Result<
                DashboardSseFramePair::ImmutableFrame>::failure(
                    std::move(compact).error());
        }

        auto full = DashboardTelemetryJsonCodec::encodeServerSentEvent(
            snapshot,
            measuredSampleHz,
            maximumEncodedBytes);
        if (!full) {
            return Domain::Result<
                DashboardSseFramePair::ImmutableFrame>::failure(
                    std::move(full).error());
        }

        const auto compactBytes = ownedBytes(std::move(compact).value());
        const auto fullBytes = ownedBytes(std::move(full).value());
        return DashboardSseFramePair::create(
            sourceSequence,
            compactBytes,
            fullBytes);
    } catch (...) {
        return Domain::Result<
            DashboardSseFramePair::ImmutableFrame>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dashboard SSE frame pair could not be encoded."));
    }
}

} // namespace ForgeConductor::Dashboard
