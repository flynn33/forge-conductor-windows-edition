#include "CliCompositionRoot.h"

#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv)
{
    try {
        std::vector<std::string_view> arguments;
        arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }

        ForgeConductor::Hosts::Cli::CliCompositionRoot compositionRoot;
        return compositionRoot.run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "Forge Conductor composition failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Forge Conductor composition failed with an unknown error.\n";
        return 1;
    }
}
