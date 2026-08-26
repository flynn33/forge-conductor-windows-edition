#pragma once

#include "ForgeConductor/Contracts/INativeToolServices.h"

#include <cstddef>

namespace ForgeConductor::NativeTools::Windows {

class WindowsTextSearchService final : public Contracts::ITextSearchService {
public:
  static constexpr std::size_t MaximumMatches = 200U;
  static constexpr std::size_t MaximumResponseBytes = 2U * 1024U * 1024U;
  static constexpr std::size_t MaximumFileBytes = 2U * 1024U * 1024U;

  [[nodiscard]] Domain::Result<std::vector<std::string>>
  search(const Contracts::AuthorizedPath &root, std::string_view query,
         std::size_t maximumMatches, std::size_t maximumResponseBytes,
         const Domain::OperationContext &context) noexcept override;
};

} // namespace ForgeConductor::NativeTools::Windows
