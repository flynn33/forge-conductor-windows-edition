#pragma once

#include "ForgeConductor/Contracts/IPathGlobService.h"

#include <cstddef>

namespace ForgeConductor::NativeTools::Windows {

class WindowsPathGlobService final : public Contracts::IPathGlobService {
public:
  static constexpr std::size_t MaximumMatches = 500U;
  static constexpr std::size_t MaximumResponseBytes = 2U * 1024U * 1024U;

  [[nodiscard]] Domain::Result<std::vector<Domain::PathText>>
  glob(const Contracts::AuthorizedPath &root, std::string_view pattern,
       std::size_t maximumMatches, std::size_t maximumResponseBytes,
       const Domain::OperationContext &context) noexcept override;
};

} // namespace ForgeConductor::NativeTools::Windows
