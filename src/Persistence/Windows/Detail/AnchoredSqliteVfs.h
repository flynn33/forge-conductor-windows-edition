#pragma once

#include "DatabaseNamespaceLease.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace ForgeConductor::Persistence::Windows::Detail {

struct AnchoredSqliteVfsState;

class AnchoredSqliteVfs final {
public:
    static constexpr std::size_t MaximumVfsNameBytes = 64U;

    [[nodiscard]] static Domain::Result<std::unique_ptr<AnchoredSqliteVfs>> create(
        std::shared_ptr<DatabaseNamespaceLease> namespaceLease) noexcept;

    [[nodiscard]] static Domain::Result<std::unique_ptr<AnchoredSqliteVfs>>
    createFrozenReadOnlyVerification(
        std::shared_ptr<DatabaseNamespaceLease> namespaceLease,
        const DatabaseLeafLease& retainedMain) noexcept;

    ~AnchoredSqliteVfs() noexcept;

    AnchoredSqliteVfs(const AnchoredSqliteVfs&) = delete;
    AnchoredSqliteVfs& operator=(const AnchoredSqliteVfs&) = delete;
    AnchoredSqliteVfs(AnchoredSqliteVfs&&) = delete;
    AnchoredSqliteVfs& operator=(AnchoredSqliteVfs&&) = delete;

    [[nodiscard]] std::string_view vfsName() const noexcept;
    [[nodiscard]] static bool registrationBelongsTo(
        std::string_view vfsName,
        const DatabaseNamespaceLease& namespaceLease) noexcept;
    [[nodiscard]] std::size_t openFileCount() const noexcept;
    [[nodiscard]] bool isRegistered() const noexcept;

    [[nodiscard]] Domain::Result<void> close() noexcept;

private:
    [[nodiscard]] static Domain::Result<std::unique_ptr<AnchoredSqliteVfs>> createWithMode(
        std::shared_ptr<DatabaseNamespaceLease> namespaceLease,
        bool frozenReadOnlyVerification,
        const DatabaseLeafLease* retainedMain) noexcept;

    explicit AnchoredSqliteVfs(std::shared_ptr<AnchoredSqliteVfsState> state) noexcept;

    std::shared_ptr<AnchoredSqliteVfsState> state_;
};

} // namespace ForgeConductor::Persistence::Windows::Detail
