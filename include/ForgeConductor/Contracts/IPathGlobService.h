#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace ForgeConductor::Contracts {

struct TextFileEditReport final {
  std::size_t replacements{};
  std::size_t bytesWritten{};

  bool operator==(const TextFileEditReport &) const = default;
};

class IPathGlobService {
public:
  virtual ~IPathGlobService() = default;

  [[nodiscard]] virtual Domain::Result<std::vector<Domain::PathText>>
  glob(const AuthorizedPath &root, std::string_view pattern,
       std::size_t maximumMatches, std::size_t maximumResponseBytes,
       const Domain::OperationContext &context) noexcept = 0;
};

class ITextFileEditService {
public:
  virtual ~ITextFileEditService() = default;

  [[nodiscard]] virtual Domain::Result<TextFileEditReport>
  replaceAll(const AuthorizedPath &readablePath,
             const AuthorizedPath &writablePath, std::string_view oldText,
             std::string_view replacementText,
             const Domain::OperationContext &context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
