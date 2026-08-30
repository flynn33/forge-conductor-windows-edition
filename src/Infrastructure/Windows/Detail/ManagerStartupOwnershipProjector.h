#pragma once

#include "ForgeConductor/Manager/ManagerStartupTaskPolicy.h"

#include <string>

namespace ForgeConductor::Infrastructure::Windows::Detail {

struct ManagerStartupOwnershipProjection final {
    Manager::ManagerStartupTaskObservation observation;
    bool foreign{};
};

class ManagerStartupOwnershipProjector final {
public:
    ManagerStartupOwnershipProjector() = delete;

    [[nodiscard]] static ManagerStartupOwnershipProjection project(
        std::string registrationIdentity,
        const Manager::ManagerStartupTaskOwnership& expected,
        Manager::ManagerStartupTaskOwnership observed) noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
