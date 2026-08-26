#pragma once

#include "ForgeConductor/Domain/Domain.h"
#include <chrono>

#include <functional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void require(
    const bool condition,
    const std::string_view message)
{
    if (!condition) {
        throw TestFailure{std::string{message}};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw TestFailure{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
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

struct TestContext final {
    Domain::MonotonicTimePoint now{std::chrono::steady_clock::now()};
    Domain::OperationId operationId{parse<Domain::OperationId>(
        "11111111-1111-4111-8111-111111111111")};
    Domain::CorrelationId correlationId{
        parse<Domain::CorrelationId>("p06-test-correlation")};
    std::stop_source cancellation;

    [[nodiscard]] Domain::OperationContext active() const
    {
        return Domain::OperationContext{
            operationId,
            now + std::chrono::minutes{5},
            cancellation.get_token(),
            correlationId};
    }

    [[nodiscard]] Domain::OperationContext expired() const
    {
        return Domain::OperationContext{
            operationId,
            now,
            cancellation.get_token(),
            correlationId};
    }
};

using TestCase = std::pair<std::string, std::function<void()>>;
using TestRegistry = std::vector<TestCase>;

inline void addTest(
    TestRegistry& tests,
    std::string name,
    std::function<void()> run)
{
    tests.emplace_back(std::move(name), std::move(run));
}

} // namespace ForgeConductor::Tests
