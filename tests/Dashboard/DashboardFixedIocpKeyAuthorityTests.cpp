#include "Infrastructure/Windows/Detail/DashboardFixedIocpKeyAuthority.h"

#include "Infrastructure/Windows/Detail/DashboardIocpWorkerKernel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;

using Authority = Detail::DashboardFixedIocpKeyAuthority;
using Key = Detail::DashboardIoCompletionKey;

std::atomic_size_t assertionCount{};

void require(const bool condition, const std::string_view message)
{
    assertionCount.fetch_add(1U, std::memory_order_relaxed);
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

static_assert(std::is_final_v<Authority>);
static_assert(!std::is_default_constructible_v<Authority>);
static_assert(!std::is_copy_assignable_v<Authority>);
static_assert(Authority::FixedKeyCount == 4U);
static_assert(noexcept(Authority::create()));
static_assert(noexcept(Authority::create(Key{1U}, Key{2U}, Key{3U}, Key{4U})));
static_assert(noexcept(std::declval<const Authority&>().deadline()));
static_assert(noexcept(std::declval<const Authority&>().overload()));
static_assert(noexcept(std::declval<const Authority&>().listenerSlotA()));
static_assert(noexcept(std::declval<const Authority&>().listenerSlotB()));
static_assert(noexcept(std::declval<const Authority&>().completionKeys()));
static_assert(noexcept(std::declval<const Authority&>().contains(Key{1U})));

void productionDefaultsAssignExactlyFourRoleKeys()
{
    const auto authority = take(Authority::create());

    require(authority.deadline() == Key{1U},
            "the production deadline key changed");
    require(authority.overload() == Key{2U},
            "the production overload key changed");
    require(authority.listenerSlotA() == Key{3U},
            "the production listener-slot-A key changed");
    require(authority.listenerSlotB() == Key{4U},
            "the production listener-slot-B key changed");

    const auto keys = authority.completionKeys();
    require(keys.size() == Authority::FixedKeyCount,
            "the authority did not expose exactly four keys");
    for (std::size_t index{}; index < keys.size(); ++index) {
        require(keys[index] == Key{index + 1U},
                "the production fixed-key order changed");
        require(authority.contains(keys[index]),
                "the authority did not recognize one of its role keys");
    }
    require(!authority.contains(Key{5U}),
            "the authority claimed a dynamic key");
}

void injectedAuthorityPreservesRoleOrder()
{
    const auto authority = take(Authority::create(
        Key{41U}, Key{11U}, Key{29U}, Key{7U}));
    const std::array expected{Key{41U}, Key{11U}, Key{29U}, Key{7U}};
    const auto actual = authority.completionKeys();

    require(std::equal(expected.begin(), expected.end(), actual.begin()),
            "the injected authority reordered role keys");
    require(authority.deadline() == expected[0U] &&
                authority.overload() == expected[1U] &&
                authority.listenerSlotA() == expected[2U] &&
                authority.listenerSlotB() == expected[3U],
            "an injected role accessor returned the wrong key");
}

void requireInvalid(
    const Key deadline,
    const Key overload,
    const Key listenerSlotA,
    const Key listenerSlotB,
    const std::string_view message)
{
    const auto result = Authority::create(
        deadline, overload, listenerSlotA, listenerSlotB);
    require(!result, message);
    require(result.error().code == Domain::ErrorCodes::InvalidRequest,
            "invalid fixed keys used the wrong error code");
}

void zeroShutdownAndDuplicateKeysAreRejected()
{
    constexpr auto Shutdown = Detail::DashboardIocpWorkerKernel::ShutdownKeyValue;

    requireInvalid(Key{0U}, Key{2U}, Key{3U}, Key{4U},
                   "a zero deadline key was accepted");
    requireInvalid(Key{1U}, Key{0U}, Key{3U}, Key{4U},
                   "a zero overload key was accepted");
    requireInvalid(Key{1U}, Key{2U}, Key{0U}, Key{4U},
                   "a zero listener-slot-A key was accepted");
    requireInvalid(Key{1U}, Key{2U}, Key{3U}, Key{0U},
                   "a zero listener-slot-B key was accepted");

    requireInvalid(Key{Shutdown}, Key{2U}, Key{3U}, Key{4U},
                   "a shutdown deadline key was accepted");
    requireInvalid(Key{1U}, Key{Shutdown}, Key{3U}, Key{4U},
                   "a shutdown overload key was accepted");
    requireInvalid(Key{1U}, Key{2U}, Key{Shutdown}, Key{4U},
                   "a shutdown listener-slot-A key was accepted");
    requireInvalid(Key{1U}, Key{2U}, Key{3U}, Key{Shutdown},
                   "a shutdown listener-slot-B key was accepted");

    requireInvalid(Key{1U}, Key{1U}, Key{3U}, Key{4U},
                   "duplicate deadline and overload keys were accepted");
    requireInvalid(Key{1U}, Key{2U}, Key{1U}, Key{4U},
                   "duplicate deadline and listener-slot-A keys were accepted");
    requireInvalid(Key{1U}, Key{2U}, Key{3U}, Key{1U},
                   "duplicate deadline and listener-slot-B keys were accepted");
    requireInvalid(Key{1U}, Key{2U}, Key{2U}, Key{4U},
                   "duplicate overload and listener-slot-A keys were accepted");
    requireInvalid(Key{1U}, Key{2U}, Key{3U}, Key{2U},
                   "duplicate overload and listener-slot-B keys were accepted");
    requireInvalid(Key{1U}, Key{2U}, Key{3U}, Key{3U},
                   "duplicate listener-slot keys were accepted");
}

} // namespace

int main()
{
    try {
        productionDefaultsAssignExactlyFourRoleKeys();
        injectedAuthorityPreservesRoleOrder();
        zeroShutdownAndDuplicateKeysAreRejected();
        std::cout << "Dashboard fixed IOCP key authority tests passed ("
                  << assertionCount.load(std::memory_order_relaxed)
                  << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard fixed IOCP key authority tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard fixed IOCP key authority tests failed with "
                     "an unknown error.\n";
        return 1;
    }
}
