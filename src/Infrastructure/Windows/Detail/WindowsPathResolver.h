#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/Result.h"
#include "UniqueHandle.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail
{

enum class MissingPathPolicy
{
    Reject,
    AllowLeaf,
    AllowDescendants
};

enum class AnchorSharePolicy
{
    DenyConcurrentWrite,
    AllowConcurrentWrite
};

class AnchoredAuthorizedPath final
{
  public:
    AnchoredAuthorizedPath(std::wstring canonicalPath,
                           std::vector<UniqueHandle> directoryAnchors) noexcept;

    AnchoredAuthorizedPath(const AnchoredAuthorizedPath &) = delete;
    AnchoredAuthorizedPath &operator=(const AnchoredAuthorizedPath &) = delete;
    AnchoredAuthorizedPath(AnchoredAuthorizedPath &&) noexcept = default;
    AnchoredAuthorizedPath &operator=(AnchoredAuthorizedPath &&) noexcept = default;

    [[nodiscard]] const std::wstring &canonicalPath() const noexcept
    {
        return canonicalPath_;
    }
    [[nodiscard]] HANDLE parentDirectoryHandle() const noexcept;
    [[nodiscard]] Domain::Result<void> revalidateDirectoryAnchors() const noexcept;

  private:
    std::wstring canonicalPath_;
    // Every existing directory from the volume root through the target parent
    // remains open without delete sharing through the handle-relative
    // filesystem commit boundary.
    std::vector<UniqueHandle> directoryAnchors_;
};

class WindowsPathResolver final
{
  public:
    [[nodiscard]] static Domain::Result<void> validateDirectoryCaseSensitivityFlags(
        std::uint32_t flags) noexcept;

    [[nodiscard]] static Domain::Result<std::wstring> resolveAppOwnedRoot(
        std::string_view utf8Path) noexcept;

    [[nodiscard]] static Domain::Result<std::wstring> resolveAppOwnedChild(
        std::wstring_view canonicalRoot, std::wstring_view relativePath,
        MissingPathPolicy policy) noexcept;

    [[nodiscard]] static Domain::Result<std::wstring> resolveAuthorizedPath(
        const Contracts::AuthorizedPath &path, Domain::FileAccess requiredAccess,
        MissingPathPolicy policy) noexcept;

    [[nodiscard]] static Domain::Result<AnchoredAuthorizedPath> resolveAnchoredAuthorizedPath(
        const Contracts::AuthorizedPath &path, Domain::FileAccess requiredAccess,
        MissingPathPolicy policy, AnchorSharePolicy sharePolicy) noexcept;

    [[nodiscard]] static Domain::Result<Domain::PathText> toPathText(
        std::wstring_view canonicalPath) noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
