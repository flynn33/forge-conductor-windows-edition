#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"

namespace ForgeConductor::Infrastructure::Windows {

class BCryptSha256Hasher final : public Contracts::IHasher {
public:
    [[nodiscard]] Domain::Result<Domain::Sha256Digest> sha256(
        std::span<const std::byte> bytes) noexcept override;
};

} // namespace ForgeConductor::Infrastructure::Windows
