#pragma once

#include "MigrationManifest.h"

#include <string_view>

namespace ForgeConductor::Persistence::Windows::Migrations {

inline constexpr int ProjectSourceVersion = 1;
inline constexpr int ProjectPhysicalVersion = 3;

[[nodiscard]] std::span<const MigrationStep> projectMigrationSteps() noexcept;
[[nodiscard]] std::span<const SchemaObject> projectRequiredSchema() noexcept;
[[nodiscard]] std::string_view projectFtsSql() noexcept;

} // namespace ForgeConductor::Persistence::Windows::Migrations

