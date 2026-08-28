#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Process-owned immutable encodings used when a live connection cannot enter
// normal application flow. All three complete HTTP responses are composed and
// validated once before listener admission begins; per-connection delivery
// only shares their immutable byte storage.
class DashboardConnectionResponseCatalog final {
public:
    using ImmutableBytes =
        std::shared_ptr<const std::vector<std::byte>>;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardConnectionResponseCatalog>>
    create() noexcept;

    ~DashboardConnectionResponseCatalog() noexcept = default;

    DashboardConnectionResponseCatalog(
        const DashboardConnectionResponseCatalog&) = delete;
    DashboardConnectionResponseCatalog& operator=(
        const DashboardConnectionResponseCatalog&) = delete;
    DashboardConnectionResponseCatalog(
        DashboardConnectionResponseCatalog&&) = delete;
    DashboardConnectionResponseCatalog& operator=(
        DashboardConnectionResponseCatalog&&) = delete;

    [[nodiscard]] const ImmutableBytes& genericServiceUnavailable()
        const noexcept;

    [[nodiscard]] const ImmutableBytes& streamUnavailable() const noexcept;

    [[nodiscard]] const ImmutableBytes& internalFailure() const noexcept;

private:
    DashboardConnectionResponseCatalog(
        ImmutableBytes genericServiceUnavailable,
        ImmutableBytes streamUnavailable,
        ImmutableBytes internalFailure) noexcept;

    const ImmutableBytes genericServiceUnavailable_;
    const ImmutableBytes streamUnavailable_;
    const ImmutableBytes internalFailure_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
