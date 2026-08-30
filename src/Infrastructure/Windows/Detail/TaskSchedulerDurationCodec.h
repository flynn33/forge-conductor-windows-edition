#pragma once

#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Manager/ManagerStartupTaskPolicy.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class TaskSchedulerDurationCodec final {
public:
    static constexpr std::size_t MaximumTextUtf16Units =
        Manager::ManagerStartupTaskPolicy::MaximumTextBytes;

    TaskSchedulerDurationCodec() = delete;

    [[nodiscard]] static Domain::Result<Manager::ManagerStartupTaskDuration>
    parse(std::wstring_view value) noexcept;

    [[nodiscard]] static Domain::Result<std::wstring> format(
        const Manager::ManagerStartupTaskDuration& value) noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
