#include "DashboardFixedIocpKeyAuthority.h"

#include "DashboardIocpWorkerKernel.h"

#include "ForgeConductor/Domain/Error.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error fixedKeyAuthorityError(
    const std::string_view code,
    std::string message)
{
    return Domain::makeError(code, std::move(message));
}

[[nodiscard]] Domain::Error invalidFixedKeysError()
{
    return fixedKeyAuthorityError(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard fixed IOCP keys must contain exactly four nonzero, "
        "distinct role keys and must exclude the worker shutdown key.");
}

} // namespace

Domain::Result<DashboardFixedIocpKeyAuthority>
DashboardFixedIocpKeyAuthority::create() noexcept
{
    return create(
        DashboardIoCompletionKey{1U},
        DashboardIoCompletionKey{2U},
        DashboardIoCompletionKey{3U},
        DashboardIoCompletionKey{4U});
}

Domain::Result<DashboardFixedIocpKeyAuthority>
DashboardFixedIocpKeyAuthority::create(
    const DashboardIoCompletionKey deadline,
    const DashboardIoCompletionKey overload,
    const DashboardIoCompletionKey listenerSlotA,
    const DashboardIoCompletionKey listenerSlotB) noexcept
{
    using CreationResult = Domain::Result<DashboardFixedIocpKeyAuthority>;
    try {
        const std::array keys{
            deadline,
            overload,
            listenerSlotA,
            listenerSlotB};
        std::array<std::uintptr_t, FixedKeyCount> values{};
        std::transform(
            keys.begin(),
            keys.end(),
            values.begin(),
            [](const DashboardIoCompletionKey key) noexcept {
                return key.value();
            });

        if (std::any_of(
                values.begin(),
                values.end(),
                [](const std::uintptr_t value) noexcept {
                    return value == 0U ||
                        value == DashboardIocpWorkerKernel::ShutdownKeyValue;
                })) {
            return CreationResult::failure(invalidFixedKeysError());
        }

        auto sortedValues = values;
        std::sort(sortedValues.begin(), sortedValues.end());
        if (std::adjacent_find(
                sortedValues.begin(), sortedValues.end()) !=
            sortedValues.end()) {
            return CreationResult::failure(invalidFixedKeysError());
        }

        return CreationResult::success(
            DashboardFixedIocpKeyAuthority{keys});
    } catch (...) {
        return CreationResult::failure(fixedKeyAuthorityError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard fixed IOCP key authority could not be created."));
    }
}

DashboardFixedIocpKeyAuthority::DashboardFixedIocpKeyAuthority(
    std::array<DashboardIoCompletionKey, FixedKeyCount> keys) noexcept
    : keys_{std::move(keys)}
{
}

DashboardIoCompletionKey
DashboardFixedIocpKeyAuthority::deadline() const noexcept
{
    return keys_[0U];
}

DashboardIoCompletionKey
DashboardFixedIocpKeyAuthority::overload() const noexcept
{
    return keys_[1U];
}

DashboardIoCompletionKey
DashboardFixedIocpKeyAuthority::listenerSlotA() const noexcept
{
    return keys_[2U];
}

DashboardIoCompletionKey
DashboardFixedIocpKeyAuthority::listenerSlotB() const noexcept
{
    return keys_[3U];
}

std::span<
    const DashboardIoCompletionKey,
    DashboardFixedIocpKeyAuthority::FixedKeyCount>
DashboardFixedIocpKeyAuthority::completionKeys() const noexcept
{
    return keys_;
}

bool DashboardFixedIocpKeyAuthority::contains(
    const DashboardIoCompletionKey key) const noexcept
{
    return std::find(keys_.begin(), keys_.end(), key) != keys_.end();
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
