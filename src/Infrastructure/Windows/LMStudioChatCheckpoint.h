#pragma once

#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "Detail/UniqueHandle.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <functional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// The scope is supplied by the current caller; a document never issues authority.
class LMStudioChatCheckpoint final {
public:
    struct RouteRecoverySnapshot final {
        nlohmann::json document;
        std::vector<std::byte> stored;
        std::string sha256;
    };
    static constexpr std::size_t MaximumPlainBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t MaximumStoredBytes = 10U * 1024U * 1024U;
    static constexpr std::string_view SourceContract = "selected-native-chat-effects-v1";

    LMStudioChatCheckpoint(const Domain::PathText& home, const Domain::ProjectId& project,
        nlohmann::json scope, const Domain::OperationContext& operation);
    [[nodiscard]] Domain::Result<std::optional<nlohmann::json>> load(
        const Domain::OperationContext& operation) noexcept;
    [[nodiscard]] Domain::Result<void> save(const nlohmann::json& state,
        const Domain::OperationContext& operation) noexcept;
    [[nodiscard]] Domain::Result<RouteRecoverySnapshot> inspectRouteRecovery(
        const Domain::OperationContext& operation) noexcept;
    [[nodiscard]] Domain::Result<Domain::PathText> recoverRoute(
        const RouteRecoverySnapshot& snapshot, const nlohmann::json& state,
        const std::function<Domain::Result<void>()>& freshAuthority,
        const Domain::OperationContext& operation) noexcept;
    [[nodiscard]] const nlohmann::json& scope() const noexcept { return scope_; }
    [[nodiscard]] const Domain::PathText& path() const noexcept { return path_; }
    [[nodiscard]] static Domain::Result<std::vector<std::byte>> seal(
        const nlohmann::json& document) noexcept;
    [[nodiscard]] static Domain::Result<nlohmann::json> open(
        std::span<const std::byte> bytes) noexcept;
private:
    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorized(
        Domain::FileAccess access, const Domain::OperationContext& operation) noexcept;
    Domain::ProjectId project_;
    Domain::PathText path_;
    nlohmann::json scope_;
    WindowsWorkspaceAuthority authority_;
    WindowsAtomicFileStore files_;
    UniqueHandle directory_, writer_;
    std::uint64_t revision_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
