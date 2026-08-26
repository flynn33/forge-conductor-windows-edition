#pragma once

#include "ForgeConductor/Domain/Domain.h"

#include <chrono>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Tests::PlatformBoundaryTestSupport {

using namespace std::chrono_literals;

inline void require(
    const bool condition,
    const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view code,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == code, message);
}

template <typename T>
[[nodiscard]] T parsed(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] inline Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] inline Domain::AuthorityId authorityId()
{
    return parsed<Domain::AuthorityId>(
        "10101010-1010-4010-8010-101010101010");
}

[[nodiscard]] inline Domain::ProjectId projectId()
{
    return parsed<Domain::ProjectId>(
        "20202020-2020-4020-8020-202020202020");
}

[[nodiscard]] inline Domain::OperationId operationId()
{
    return parsed<Domain::OperationId>(
        "30303030-3030-4030-8030-303030303030");
}

[[nodiscard]] inline Domain::OperationId otherOperationId()
{
    return parsed<Domain::OperationId>(
        "40404040-4040-4040-8040-404040404040");
}

[[nodiscard]] inline Domain::ClientId clientId()
{
    return parsed<Domain::ClientId>("platform-boundary-test");
}

[[nodiscard]] inline Domain::CorrelationId correlationId()
{
    return parsed<Domain::CorrelationId>("platform-boundary-correlation");
}

[[nodiscard]] inline Domain::OperationContext activeContext(
    const Domain::MonotonicTimePoint now,
    const Domain::OperationId& id = operationId())
{
    return Domain::OperationContext{
        id,
        now + 10s,
        std::stop_token{},
        correlationId()};
}

[[nodiscard]] inline Domain::OperationContext expiredContext(
    const Domain::MonotonicTimePoint now)
{
    return Domain::OperationContext{
        operationId(),
        now,
        std::stop_token{},
        correlationId()};
}

[[nodiscard]] inline Domain::OperationContext cancelledContext(
    const Domain::MonotonicTimePoint now,
    const std::stop_token token)
{
    return Domain::OperationContext{
        operationId(),
        now + 10s,
        token,
        correlationId()};
}

} // namespace ForgeConductor::Tests::PlatformBoundaryTestSupport
