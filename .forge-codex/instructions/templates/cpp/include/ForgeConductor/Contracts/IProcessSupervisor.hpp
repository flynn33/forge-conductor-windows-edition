#pragma once

#include "Result.hpp"

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace forge
{
    struct ProcessRequest final
    {
        std::wstring executable;
        std::vector<std::wstring> arguments;
        std::optional<std::wstring> workingDirectory;
        std::map<std::wstring, std::wstring> environment;
        std::chrono::milliseconds timeout;
        std::size_t maximumStdoutBytes{};
        std::size_t maximumStderrBytes{};
    };

    struct ProcessResult final
    {
        unsigned long exitCode{};
        std::string stdoutUtf8;
        std::string stderrUtf8;
        bool timedOut{};
        bool cancelled{};
        bool stdoutTruncated{};
        bool stderrTruncated{};
    };

    class IProcessSupervisor
    {
    public:
        virtual ~IProcessSupervisor() = default;
        virtual Result<ProcessResult> run(ProcessRequest const& request) = 0;
        virtual void cancelAll() noexcept = 0;
        virtual void shutdown() noexcept = 0;
    };
}
