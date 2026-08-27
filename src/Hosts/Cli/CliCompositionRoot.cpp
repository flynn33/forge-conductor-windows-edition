#include "CliCompositionRoot.h"
#include "McpServeCompositionRoot.h"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace ForgeConductor::Hosts::Cli {
namespace {

constexpr std::string_view ProductName{"Forge Conductor"};
constexpr std::string_view ProductVersion{"0.9.0"};

} // namespace

class CliApplication final {
public:
    [[nodiscard]] int run(const std::span<const std::string_view> arguments) const
    {
        if (arguments.empty()) {
            printHelp();
            return 0;
        }

        const auto command = arguments.front();
        if (command == "--version" || command == "version") {
            std::cout << ProductName << ' ' << ProductVersion << " (Windows native)\n";
            return 0;
        }
        if (command == "--self-test") {
            static_assert(!std::is_copy_constructible_v<CliCompositionRoot>);
            static_assert(!std::is_move_constructible_v<CliCompositionRoot>);
            if constexpr (sizeof(void*) != 8) {
                std::cerr << "Forge Conductor scaffold self-test requires a 64-bit build.\n";
                return 1;
            }
            std::cout << "Forge Conductor native scaffold self-test passed.\n";
            return 0;
        }
        if (command == "--help" || command == "-h" || command == "help") {
            printHelp();
            return 0;
        }
        if (command == "serve" || command == "mcp-serve" || command == "mcp") {
            return serve(arguments.subspan(1U));
        }

        std::cerr << "Unknown command: " << command << '\n';
        return 2;
    }

private:
    [[nodiscard]] static int serve(
        const std::span<const std::string_view> arguments)
    {
        McpServeOptions options;
        if (!arguments.empty()) {
            if (arguments.size() != 2U || arguments.front() != "--home") {
                std::cerr
                    << "Usage: forge-conductor serve [--home PATH]\n";
                return 2;
            }
            auto home = Domain::PathText::create(arguments[1U]);
            if (!home) {
                throw std::runtime_error{
                    home.error().code + ": " + home.error().message};
            }
            options.explicitHome = std::move(home).value();
        }

        McpServeCompositionRoot serveRoot{std::move(options)};
        return serveRoot.run();
    }

    static void printHelp()
    {
        std::cout
            << "Forge Conductor for Windows\n"
            << "Usage: forge-conductor COMMAND\n"
            << "Commands:\n"
            << "  serve [--home PATH]  Run one stdio MCP connection.\n"
            << "  mcp-serve             Alias for serve.\n"
            << "  mcp                   Alias for serve.\n"
            << "  version               Print the product version.\n"
            << "  --self-test           Validate the native CLI scaffold.\n";
    }
};

CliCompositionRoot::CliCompositionRoot()
    : application_{std::make_unique<CliApplication>()}
{
}

CliCompositionRoot::~CliCompositionRoot() = default;

int CliCompositionRoot::run(const std::span<const std::string_view> arguments) const
{
    return application_->run(arguments);
}

} // namespace ForgeConductor::Hosts::Cli
