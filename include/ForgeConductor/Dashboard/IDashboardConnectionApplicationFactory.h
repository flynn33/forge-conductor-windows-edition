#pragma once

#include "ForgeConductor/Dashboard/IDashboardConnectionApplication.h"
#include "ForgeConductor/Domain/ConfigurationModels.h"
#include "ForgeConductor/Domain/Result.h"

#include <memory>

namespace ForgeConductor::Dashboard {

// Transport-neutral construction boundary for immutable dashboard listener
// generations. The caller supplies an already validated configuration; each
// successful call returns a distinct application object that retains the
// exact endpoint policy for that generation.
class IDashboardConnectionApplicationFactory {
public:
    virtual ~IDashboardConnectionApplicationFactory() noexcept = default;

    [[nodiscard]] virtual Domain::Result<
        std::shared_ptr<IDashboardConnectionApplication>>
    create(const Domain::DashboardConfig& configuration) noexcept = 0;
};

} // namespace ForgeConductor::Dashboard
