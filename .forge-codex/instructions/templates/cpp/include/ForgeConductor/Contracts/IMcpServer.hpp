#pragma once

#include "Result.hpp"

#include <chrono>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace forge
{
    struct McpToolDescriptor final
    {
        std::string name;
        std::string description;
        std::string inputSchemaJson;
    };

    class IMcpServer
    {
    public:
        virtual ~IMcpServer() = default;
        virtual Result<void*> run(
            std::istream& input,
            std::ostream& output,
            std::string const& role,
            std::chrono::steady_clock::time_point deadline) = 0;
        virtual std::vector<McpToolDescriptor> tools() const = 0;
        virtual void shutdown() noexcept = 0;
    };
}
