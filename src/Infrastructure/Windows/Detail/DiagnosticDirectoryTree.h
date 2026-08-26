#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "UniqueHandle.h"

#include <Windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class IDiagnosticDirectoryAnchorObserver {
public:
    virtual ~IDiagnosticDirectoryAnchorObserver() = default;

    virtual void onDirectoryAnchored(std::wstring_view path) noexcept = 0;
};

struct AnchoredDiagnosticDirectoryTree final {
    std::wstring root;
    std::vector<UniqueHandle> handles;
};

[[nodiscard]] Domain::Result<AnchoredDiagnosticDirectoryTree>
prepareAnchoredDiagnosticDirectory(const Domain::PathText& rootText,
                                   const Domain::OperationContext& context, HANDLE shutdownEvent,
                                   IDiagnosticDirectoryAnchorObserver* observer = nullptr) noexcept;

} // namespace ForgeConductor::Infrastructure::Windows::Detail
