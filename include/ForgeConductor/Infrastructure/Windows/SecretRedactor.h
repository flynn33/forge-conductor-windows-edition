#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <cstddef>

namespace ForgeConductor::Infrastructure::Windows {

class SecretRedactor final : public Contracts::IRedactor {
public:
    static constexpr std::size_t MaximumInputBytes = 256U * 1024U;

    [[nodiscard]] Domain::Result<std::string> redact(
        std::string_view value) noexcept override;
};

} // namespace ForgeConductor::Infrastructure::Windows
