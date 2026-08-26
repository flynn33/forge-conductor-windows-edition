#pragma once

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <cstddef>
#include <memory>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows
{

namespace Detail
{
class IDiagnosticRotationPublishObserver;
struct WindowsDiagnosticSinkTestAccess;
} // namespace Detail

struct WindowsDiagnosticSinkOptions final
{
    Domain::PathText diagnosticsRoot;
    Domain::PathText exportRoot;
    Domain::ResourceBudgets budgets;
    bool enableEtw{true};
};

class WindowsDiagnosticSink final : public Contracts::IDiagnosticSink
{
  public:
    static constexpr std::string_view MasterLogName{"forge-diagnostics.jsonl"};
    static constexpr std::size_t MaximumExportBasenameBytes = 128U;
    static constexpr std::size_t MaximumRetainedLogFiles = 10U;
    static constexpr std::size_t MaximumRetainedRecords = Domain::MaximumDiagnosticRingRecords;

    WindowsDiagnosticSink(WindowsDiagnosticSinkOptions options, std::shared_ptr<Contracts::IClock> clock,
                          std::shared_ptr<Contracts::IRedactor> redactor, std::shared_ptr<Contracts::IHasher> hasher,
                          std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority,
                          std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore);
    ~WindowsDiagnosticSink() noexcept override;

    WindowsDiagnosticSink(const WindowsDiagnosticSink &) = delete;
    WindowsDiagnosticSink &operator=(const WindowsDiagnosticSink &) = delete;
    WindowsDiagnosticSink(WindowsDiagnosticSink &&) = delete;
    WindowsDiagnosticSink &operator=(WindowsDiagnosticSink &&) = delete;

    [[nodiscard]] Domain::Result<void> record(const Domain::DiagnosticEnvelope &event,
                                              const Domain::OperationContext &context) noexcept override;

    [[nodiscard]] Domain::Result<std::vector<Domain::DiagnosticEnvelope>> recent(
        std::size_t maximumCount, const Domain::OperationContext &context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::DiagnosticExportResult> exportData(
        const Domain::DiagnosticExportRequest &request, const Contracts::WorkspaceAuthority &authority,
        const Domain::OperationContext &context) noexcept override;

    void shutdown() noexcept override;

  private:
    friend struct Detail::WindowsDiagnosticSinkTestAccess;

    WindowsDiagnosticSink(WindowsDiagnosticSinkOptions options, std::shared_ptr<Contracts::IClock> clock,
                          std::shared_ptr<Contracts::IRedactor> redactor, std::shared_ptr<Contracts::IHasher> hasher,
                          std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority,
                          std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore,
                          std::shared_ptr<Detail::IDiagnosticRotationPublishObserver> rotationPublishObserver);

    class Impl;
    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
