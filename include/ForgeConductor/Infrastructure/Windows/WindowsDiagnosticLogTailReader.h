#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {

// Bounded, handle-anchored reader for the application-owned diagnostic JSONL
// master file. Calls are independent and safe to issue concurrently; the
// shared diagnostic lock serializes each snapshot with sink append/rotation.
class WindowsDiagnosticLogTailReader final {
public:
    static constexpr std::size_t MaximumRequestedLines = 100U;
    static constexpr std::size_t MaximumRequestedLineBytes = 16U * 1024U;
    static constexpr std::size_t MaximumRequestedAggregateBytes = 512U * 1024U;

    explicit WindowsDiagnosticLogTailReader(Domain::PathText diagnosticsRoot);

    WindowsDiagnosticLogTailReader(const WindowsDiagnosticLogTailReader&) = delete;
    WindowsDiagnosticLogTailReader& operator=(const WindowsDiagnosticLogTailReader&) = delete;
    WindowsDiagnosticLogTailReader(WindowsDiagnosticLogTailReader&&) = delete;
    WindowsDiagnosticLogTailReader& operator=(WindowsDiagnosticLogTailReader&&) = delete;

    // Returns the newest selected lines in their original file order, without
    // LF or CRLF delimiters. A selected line is rejected rather than split or
    // truncated when it exceeds either caller-provided byte budget.
    [[nodiscard]] Domain::Result<std::vector<std::string>> newestLines(
        std::size_t maximumLines,
        std::size_t maximumLineBytes,
        std::size_t maximumAggregateBytes,
        const Domain::OperationContext& context) const noexcept;

private:
    Domain::PathText diagnosticsRoot_;
};

} // namespace ForgeConductor::Infrastructure::Windows
