#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Dashboard {

// Immutable, platform-neutral path produced only after the route catalog has
// classified an exact raw /static/ target. The restricted canonical spelling
// prevents Windows case, separator, device-name, and Unicode normalization
// aliases before a later infrastructure adapter resolves the path.
class DashboardStaticResourcePath final {
public:
    static constexpr std::size_t MaximumRawTargetBytes = 8U * 1024U;
    static constexpr std::size_t MaximumDecodedBytes = 1024U;
    static constexpr std::size_t MaximumSegmentBytes = 255U;

    [[nodiscard]] static Domain::Result<DashboardStaticResourcePath> decode(
        std::string_view classifiedRawTarget) noexcept;

    [[nodiscard]] const std::string& relativePath() const noexcept
    {
        return relativePath_;
    }

    [[nodiscard]] std::string_view mimeType() const noexcept;

    bool operator==(const DashboardStaticResourcePath&) const = default;

private:
    enum class MimeKind : std::uint8_t {
        Html,
        Css,
        JavaScript,
        Json,
    };

    DashboardStaticResourcePath(std::string relativePath, const MimeKind mimeKind)
        : relativePath_{std::move(relativePath)}, mimeKind_{mimeKind}
    {
    }

    std::string relativePath_;
    MimeKind mimeKind_;
};

} // namespace ForgeConductor::Dashboard
