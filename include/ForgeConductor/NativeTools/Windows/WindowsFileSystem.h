#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IPathGlobService.h"

#include <cstddef>
#include <memory>

namespace ForgeConductor::NativeTools::Windows {

class WindowsFileSystem final : public Contracts::IFileSystem,
                                public Contracts::ITextFileEditService {
public:
  static constexpr std::size_t MaximumTextFileBytes = 2U * 1024U * 1024U;
  static constexpr std::size_t MaximumListEntries = 1'000U;

  explicit WindowsFileSystem(
      std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore);
  ~WindowsFileSystem() override;

  WindowsFileSystem(const WindowsFileSystem &) = delete;
  WindowsFileSystem &operator=(const WindowsFileSystem &) = delete;

  [[nodiscard]] Domain::Result<std::vector<std::byte>>
  readFile(const Contracts::AuthorizedPath &path, std::size_t maximumBytes,
           const Domain::OperationContext &context) noexcept override;

  [[nodiscard]] Domain::Result<void>
  writeFile(const Contracts::AuthorizedPath &path,
            std::span<const std::byte> content,
            const Domain::OperationContext &context) noexcept override;

  [[nodiscard]] Domain::Result<Domain::DirectoryListing>
  list(const Contracts::AuthorizedPath &directory, std::size_t maximumEntries,
       const Domain::OperationContext &context) noexcept override;

  [[nodiscard]] Domain::Result<void>
  createDirectory(const Contracts::AuthorizedPath &directory,
                  const Domain::OperationContext &context) noexcept override;

  [[nodiscard]] Domain::Result<void>
  remove(const Contracts::AuthorizedPath &path, bool recursive,
         const Domain::OperationContext &context) noexcept override;

  [[nodiscard]] Domain::Result<void>
  move(const Contracts::AuthorizedPath &source,
       const Contracts::AuthorizedPath &destination,
       const Domain::OperationContext &context) noexcept override;

  [[nodiscard]] Domain::Result<Contracts::TextFileEditReport>
  replaceAll(const Contracts::AuthorizedPath &readablePath,
             const Contracts::AuthorizedPath &writablePath,
             std::string_view oldText, std::string_view replacementText,
             const Domain::OperationContext &context) noexcept override;

private:
  class Impl;
  std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::NativeTools::Windows
