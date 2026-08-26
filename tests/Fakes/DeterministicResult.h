#pragma once

#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/Result.h"

#include <utility>

namespace ForgeConductor::Tests::Fakes {

template <typename T>
class DeterministicResult final {
public:
    DeterministicResult()
        : result_{Domain::Result<T>::failure(Domain::makeError(
              Domain::ErrorCodes::InternalFailure,
              "No deterministic result was configured."))}
    {
    }

    explicit DeterministicResult(Domain::Result<T> result)
        : result_{std::move(result)}
    {
    }

    void set(Domain::Result<T> result)
    {
        result_ = std::move(result);
    }

    [[nodiscard]] Domain::Result<T> get() const
    {
        return result_;
    }

private:
    Domain::Result<T> result_;
};

} // namespace ForgeConductor::Tests::Fakes
