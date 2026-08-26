#include "CliCompositionRoot.h"

#include <iostream>
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

        std::cerr << "Unknown command: " << command << '\n';
        return 2;
    }

private:
    static void printHelp()
    {
        std::cout
            << "Forge Conductor for Windows\n"
            << "Usage: forge-conductor [--version|version|--self-test|--help]\n";
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
