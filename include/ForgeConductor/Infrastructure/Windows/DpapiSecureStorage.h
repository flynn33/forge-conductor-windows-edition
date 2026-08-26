#pragma once

#include "ForgeConductor/Contracts/ISecureStorage.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {

class DpapiSecureStorage final : public Contracts::ISecureStorage {
public:
    static constexpr std::size_t MaximumKeyBytes = 128U;
    static constexpr std::size_t MaximumSecretBytes = 64U * 1024U;
    static constexpr std::size_t MaximumEntryCount = 128U;
    static constexpr std::wstring_view DefaultRegistrySubkey =
        L"Software\\Forge Conductor\\SecureStorage";

    explicit DpapiSecureStorage(std::wstring registrySubkey);
    ~DpapiSecureStorage() override;

    DpapiSecureStorage(const DpapiSecureStorage&) = delete;
    DpapiSecureStorage& operator=(const DpapiSecureStorage&) = delete;

    [[nodiscard]] Domain::Result<void> put(
        std::string_view key,
        std::span<const std::byte> secret,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::optional<std::vector<std::byte>>> get(
        std::string_view key,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> remove(
        std::string_view key,
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
