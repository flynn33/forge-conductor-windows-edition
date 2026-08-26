#pragma once

#include "ForgeConductor/Domain/Domain.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ForgeConductor::Persistence::Windows {

enum class DatabaseStoreKind {
    Central,
    Project
};

struct DatabaseSchemaSnapshot final {
    DatabaseStoreKind kind{DatabaseStoreKind::Central};
    int physicalVersion{};
    int sourceCompatibilityVersion{};
    bool fts5Enabled{};
    std::vector<std::string> tables;
    std::vector<std::string> indexes;
    std::vector<std::string> triggers;
};

struct DatabaseBackupReport final {
    Domain::PathText backupPath;
    std::int64_t pageCount{};
    bool quickCheckPassed{};
};

} // namespace ForgeConductor::Persistence::Windows

