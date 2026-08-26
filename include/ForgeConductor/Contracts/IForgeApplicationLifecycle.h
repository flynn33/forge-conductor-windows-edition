#pragma once

#include "ForgeConductor/Domain/Result.h"

namespace ForgeConductor::Contracts {

class IForgeApplicationLifecycle {
public:
    virtual ~IForgeApplicationLifecycle() = default;

    [[nodiscard]] virtual Domain::Result<void> start() noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> stop() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
