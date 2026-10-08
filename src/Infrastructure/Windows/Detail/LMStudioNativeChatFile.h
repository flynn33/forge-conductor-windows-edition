#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <Windows.h>
#include <string>

namespace ForgeConductor::Infrastructure::Windows::Detail {

struct NativeChatFile final {
    std::string bytes;
    BY_HANDLE_FILE_INFORMATION revision{};
};

[[nodiscard]] bool sameRevision(
    const BY_HANDLE_FILE_INFORMATION& left, const BY_HANDLE_FILE_INFORMATION& right);

[[nodiscard]] Domain::Result<NativeChatFile> readNativeChatFile(
    const std::wstring& path, const Domain::OperationContext& context);

} // namespace ForgeConductor::Infrastructure::Windows::Detail
