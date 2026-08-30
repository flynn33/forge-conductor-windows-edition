#pragma once

#include "ForgeConductor/Domain/ManagerStartupModels.h"
#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Manager/ManagerStartupTaskPolicy.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

struct ManagerStartupResolvedRegistration final {
    std::wstring taskPath;
    Manager::ManagerStartupTaskDefinition definition;

    bool operator==(const ManagerStartupResolvedRegistration&) const = default;
};

// Purely resolves the canonical per-user Task Scheduler projection. Native
// registration and filesystem access remain the responsibility of the caller.
class ManagerStartupDefinitionBuilder final {
public:
    static constexpr std::size_t MaximumPurposeSuffixCharacters = 48U;

    ManagerStartupDefinitionBuilder() = delete;

    [[nodiscard]] static Domain::Result<ManagerStartupResolvedRegistration>
    build(
        const Domain::ManagerStartupDefinition& startup,
        const WindowsCurrentUserIdentity& identity,
        std::string_view purposeSuffix = {}) noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
