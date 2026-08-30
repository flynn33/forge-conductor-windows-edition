#pragma once

#include "ForgeConductor/Domain/ProcessModels.h"
#include "ForgeConductor/Domain/Result.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {

struct EnvironmentEntry final {
    std::wstring name;
    std::wstring value;
};

class CommandLineBuilder final {
public:
    [[nodiscard]] static Domain::Result<std::wstring> utf8ToUtf16(
        std::string_view value);

    [[nodiscard]] static Domain::Result<std::wstring> buildArgumentString(
        const std::vector<std::string>& arguments);

    [[nodiscard]] static Domain::Result<std::vector<wchar_t>> buildCommandLine(
        std::wstring_view absoluteApplicationName,
        const std::vector<std::string>& arguments);

    [[nodiscard]] static Domain::Result<std::vector<EnvironmentEntry>>
    readCurrentEnvironment();

    [[nodiscard]] static Domain::Result<std::vector<wchar_t>>
    buildEnvironmentBlock(
        const Domain::ProcessRequest& request,
        std::span<const EnvironmentEntry> inheritedEnvironment);
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
