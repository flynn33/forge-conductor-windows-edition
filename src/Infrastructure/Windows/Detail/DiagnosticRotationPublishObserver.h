#pragma once

#include "ForgeConductor/Infrastructure/Windows/WindowsDiagnosticSink.h"

#include <memory>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail
{

class IDiagnosticRotationPublishObserver
{
  public:
    virtual ~IDiagnosticRotationPublishObserver() = default;

    virtual void afterStagedFileCreation(std::wstring_view stagedPath) noexcept = 0;
    virtual void beforeStagedFileValidation(std::wstring_view stagedPath) noexcept = 0;
};

struct WindowsDiagnosticSinkTestAccess final
{
    [[nodiscard]] static std::unique_ptr<WindowsDiagnosticSink> create(
        WindowsDiagnosticSinkOptions options, std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IRedactor> redactor, std::shared_ptr<Contracts::IHasher> hasher,
        std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority,
        std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore,
        std::shared_ptr<IDiagnosticRotationPublishObserver> rotationPublishObserver);
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
