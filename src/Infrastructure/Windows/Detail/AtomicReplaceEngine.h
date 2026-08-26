#pragma once

#include "BoundedSerialExecutor.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail
{

struct AtomicNativeCallResult final
{
    bool succeeded{};
    std::uint32_t errorCode{};
};

struct AtomicFeatureCallResult final
{
    bool succeeded{};
    bool supported{};
    std::uint32_t errorCode{};
};

class IAtomicReplaceNativeOperations
{
  public:
    virtual ~IAtomicReplaceNativeOperations() = default;

    [[nodiscard]] virtual Domain::Result<std::array<std::byte, 16U>> randomBytes() noexcept = 0;

    virtual void beforeReadOpen(std::wstring_view leafName) noexcept = 0;

    virtual void beforeReplacementVerification() noexcept = 0;

    virtual void beforePublish(std::wstring_view destinationName) noexcept = 0;

    [[nodiscard]] virtual AtomicFeatureCallResult queryPosixRenameSupport(
        HANDLE directory) noexcept = 0;

    [[nodiscard]] virtual AtomicNativeCallResult renameFile(HANDLE source,
                                                            HANDLE destinationDirectory,
                                                            std::wstring_view destinationName,
                                                            bool replaceExisting) noexcept = 0;
};

class AtomicReplaceEngine final
{
  public:
    static constexpr std::size_t MaximumContentBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t MaximumTemporaryNameAttempts = 32U;
    static constexpr std::size_t MaximumRetainedTemporaryFiles = 32U;
    static constexpr std::size_t MaximumStaleTemporaryDeletesPerOperation = 32U;

    [[nodiscard]] static Domain::Result<void> validateExtendedAttributeSize(
        std::uint32_t size) noexcept;

    [[nodiscard]] static Domain::Result<void> validateSecurityIdentityEquivalence(
        bool ownerMatches, bool groupMatches, bool mandatoryLabelMatches) noexcept;

    AtomicReplaceEngine();
    explicit AtomicReplaceEngine(IAtomicReplaceNativeOperations &nativeOperations) noexcept;
    ~AtomicReplaceEngine();

    AtomicReplaceEngine(const AtomicReplaceEngine &) = delete;
    AtomicReplaceEngine &operator=(const AtomicReplaceEngine &) = delete;

    [[nodiscard]] Domain::Result<std::vector<std::byte>> read(
        const Contracts::AuthorizedPath &path, std::size_t maximumBytes,
        const Domain::OperationContext &context) noexcept;

    [[nodiscard]] Domain::Result<void> replace(const Contracts::AuthorizedPath &path,
                                               std::span<const std::byte> content,
                                               bool retainBackup,
                                               const Domain::OperationContext &context) noexcept;

  private:
    std::unique_ptr<IAtomicReplaceNativeOperations> ownedNativeOperations_;
    IAtomicReplaceNativeOperations *nativeOperations_{};
    BoundedSerialExecutor executor_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
