#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"

namespace ForgeConductor::Infrastructure::Windows {

class WindowsUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override;
};

} // namespace ForgeConductor::Infrastructure::Windows
