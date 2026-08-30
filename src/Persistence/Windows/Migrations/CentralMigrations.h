#pragma once

#include "MigrationManifest.h"

namespace ForgeConductor::Persistence::Windows::Migrations {

inline constexpr int CentralSourceVersion = 5;
inline constexpr int CentralPhysicalVersion = 7;

[[nodiscard]] std::span<const MigrationStep> centralMigrationSteps() noexcept;
[[nodiscard]] std::span<const SchemaObject> centralRequiredSchema() noexcept;

} // namespace ForgeConductor::Persistence::Windows::Migrations

