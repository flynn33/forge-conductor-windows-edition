#pragma once

#include <span>
#include <string_view>

namespace ForgeConductor::Persistence::Windows::Migrations {

struct MigrationStep final {
    int version{};
    std::string_view identifier;
    std::string_view sql;
    std::string_view contentSha256;
};

struct SchemaObject final {
    std::string_view type;
    std::string_view name;
};

} // namespace ForgeConductor::Persistence::Windows::Migrations

