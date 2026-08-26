#pragma once

#include "UniqueHandle.h"

#include <Windows.h>

#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class RelativeOpenDisposition : unsigned char {
    OpenExisting,
    CreateNew,
    OpenOrCreate,
};

enum class RelativeObjectType : unsigned char {
    File,
    Directory,
};

struct RelativeOpenOptions final {
    ACCESS_MASK desiredAccess{};
    ULONG shareAccess{};
    RelativeOpenDisposition disposition{RelativeOpenDisposition::OpenExisting};
    ULONG fileAttributes{FILE_ATTRIBUTE_NORMAL};
    RelativeObjectType objectType{RelativeObjectType::File};
    bool writeThrough{};
    bool sequentialAccess{};
    bool deleteOnClose{};
};

struct RelativeOpenResult final {
    UniqueHandle handle;
    LONG nativeStatus{};
    DWORD win32Error{};
    ULONG_PTR createInformation{};

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle); }
    [[nodiscard]] bool wasOpened() const noexcept { return createInformation == 1U; }
    [[nodiscard]] bool wasCreated() const noexcept { return createInformation == 2U; }
};

[[nodiscard]] RelativeOpenResult openRelative(HANDLE rootDirectory,
                                              std::wstring_view oneComponentName,
                                              const RelativeOpenOptions& options) noexcept;

} // namespace ForgeConductor::Infrastructure::Windows::Detail
