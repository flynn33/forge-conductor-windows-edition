#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"

#include <Windows.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows::Detail {

namespace InfrastructureDetail =
    ForgeConductor::Infrastructure::Windows::Detail;

struct NativeDirectoryEntry final {
  std::wstring name;
  DWORD attributes{};

  [[nodiscard]] bool isDirectory() const noexcept {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
  }

  [[nodiscard]] bool isReparsePoint() const noexcept {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
  }
};

struct OpenedNativeObject final {
  OpenedNativeObject(
      InfrastructureDetail::UniqueHandle objectHandle,
      std::wstring objectCanonicalPath, DWORD objectAttributes,
      std::optional<InfrastructureDetail::AnchoredAuthorizedPath>
          authorizedOwner = std::nullopt) noexcept
      : authorizedPathOwner{std::move(authorizedOwner)},
        handle{std::move(objectHandle)},
        canonicalPath{std::move(objectCanonicalPath)},
        attributes{objectAttributes} {}

  OpenedNativeObject(const OpenedNativeObject &) = delete;
  OpenedNativeObject &operator=(const OpenedNativeObject &) = delete;
  OpenedNativeObject(OpenedNativeObject &&) noexcept = default;
  OpenedNativeObject &operator=(OpenedNativeObject &&) noexcept = default;

  // Declared before the leaf handle so the leaf closes first and every
  // authority-directory anchor remains pinned through the entire operation.
  std::optional<InfrastructureDetail::AnchoredAuthorizedPath>
      authorizedPathOwner;
  InfrastructureDetail::UniqueHandle handle;
  std::wstring canonicalPath;
  DWORD attributes{};

  [[nodiscard]] bool isDirectory() const noexcept {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
  }
};

using WalkVisitor = std::function<Domain::Result<bool>(
    HANDLE parentDirectory, const NativeDirectoryEntry &entry,
    std::wstring_view canonicalPath, std::wstring_view relativePath,
    std::size_t depth, const Domain::OperationContext &context)>;

struct NativeWalkOptions final {
  std::size_t maximumVisitedEntries{100'000U};
  std::size_t maximumDepth{128U};
  bool excludeGitAndNodeModules{};
};

[[nodiscard]] Domain::Error nativeFileError(std::string_view action,
                                            DWORD nativeCode) noexcept;

[[nodiscard]] bool samePath(std::wstring_view left,
                            std::wstring_view right) noexcept;

[[nodiscard]] bool sameName(std::wstring_view left,
                            std::wstring_view right) noexcept;

[[nodiscard]] Domain::Result<OpenedNativeObject> openAuthorizedObject(
    const Contracts::AuthorizedPath &path, Domain::FileAccess requiredAccess,
    InfrastructureDetail::MissingPathPolicy missingPolicy,
    const Domain::OperationContext &context,
    ACCESS_MASK desiredAccess = FILE_READ_ATTRIBUTES | FILE_READ_DATA |
                                FILE_LIST_DIRECTORY | FILE_TRAVERSE,
    DWORD shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE) noexcept;

[[nodiscard]] Domain::Result<OpenedNativeObject>
openAuthorizedDirectory(const Contracts::AuthorizedPath &path,
                        const Domain::OperationContext &context) noexcept;

[[nodiscard]] Domain::Result<OpenedNativeObject>
openChildObject(HANDLE parentDirectory, std::wstring_view childName,
                std::wstring_view expectedPath, ACCESS_MASK desiredAccess,
                DWORD shareAccess,
                const Domain::OperationContext &context) noexcept;

[[nodiscard]] Domain::Result<OpenedNativeObject>
openCanonicalDirectory(std::wstring_view canonicalPath,
                       ACCESS_MASK desiredAccess, DWORD shareAccess,
                       const Domain::OperationContext &context) noexcept;

[[nodiscard]] Domain::Result<OpenedNativeObject>
openOrCreateChildDirectory(HANDLE parentDirectory, std::wstring_view childName,
                           std::wstring_view expectedPath,
                           const Domain::OperationContext &context) noexcept;

[[nodiscard]] Domain::Result<void> ensureAuthorizedParentDirectories(
    const Contracts::AuthorizedPath &path,
    const Domain::OperationContext &context) noexcept;

[[nodiscard]] Domain::Result<std::vector<NativeDirectoryEntry>>
enumerateDirectory(HANDLE directory, std::size_t maximumEntries,
                   const Domain::OperationContext &context) noexcept;

[[nodiscard]] Domain::Result<void>
walkAuthorizedDirectory(const Contracts::AuthorizedPath &root,
                        const NativeWalkOptions &options,
                        const WalkVisitor &visitor,
                        const Domain::OperationContext &context) noexcept;

[[nodiscard]] Domain::Result<void>
validateUtf8Text(std::string_view text, std::string_view description) noexcept;

[[nodiscard]] Domain::Result<std::vector<std::byte>>
readOpenedFile(HANDLE file, std::size_t maximumBytes,
               const Domain::OperationContext &context) noexcept;

[[nodiscard]] Domain::Result<void>
deleteOpenedObject(HANDLE object,
                   const Domain::OperationContext &context) noexcept;

} // namespace ForgeConductor::NativeTools::Windows::Detail
