#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ForgeConductor::Contracts {

class ISecureStorage {
public:
    static constexpr std::size_t MaximumKeyBytes = 128U;
    static constexpr std::size_t MaximumSecretBytes = 64U * 1024U;
    static constexpr std::size_t MaximumEntryCount = 128U;

    virtual ~ISecureStorage() = default;

    [[nodiscard]] virtual Domain::Result<void> put(
        std::string_view key,
        std::span<const std::byte> secret,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<std::vector<std::byte>>> get(
        std::string_view key,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> remove(
        std::string_view key,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
