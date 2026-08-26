#pragma once

#include "ForgeConductor/Domain/Identifiers.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Domain {

class PathText final {
public:
    static constexpr std::size_t MaximumBytes = 32 * 1024;

    [[nodiscard]] static Result<PathText> create(std::string_view value)
    {
        if (value.empty() || value.size() > MaximumBytes ||
            value.find('\0') != std::string_view::npos) {
            return Result<PathText>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Path text is empty, contains NUL, or exceeds 32768 UTF-8 bytes."));
        }
        return Result<PathText>::success(PathText{std::string{value}});
    }

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    auto operator<=>(const PathText&) const = default;

private:
    explicit PathText(std::string value) : value_{std::move(value)} {}

    std::string value_;
};

struct DirectoryListing final {
    std::vector<PathText> entries;
    bool truncated{};
};

enum class FileAccess {
    Read,
    Write,
    Create,
    Delete,
    Execute
};

struct PathAuthorizationRequest final {
    PathText requestedPath;
    std::optional<PathText> basePath;
    FileAccess access{FileAccess::Read};
    bool protectAuthorityRoot{};
};

} // namespace ForgeConductor::Domain
