#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/ConfigurationModels.h"

#include <cstddef>

namespace ForgeConductor::Contracts {

class IConfigurationStore {
public:
    static constexpr std::size_t SchemaVersion = 1U;
    static constexpr std::size_t MaximumDocumentBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumDocumentDepth = 32U;

    virtual ~IConfigurationStore() = default;

    [[nodiscard]] virtual Domain::Result<Domain::AppConfig> load(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AppConfig> update(
        const Domain::AppConfigPatch& patch,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AppConfig> reload(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
