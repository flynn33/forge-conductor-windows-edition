#include "SessionHostCompositionRoot.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

int wmain(const int argc, wchar_t** const argv)
{
    try {
        std::vector<std::wstring_view> arguments;
        arguments.reserve(
            argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }

        ForgeConductor::Hosts::SessionHost::SessionHostCompositionRoot
            compositionRoot;
        return compositionRoot.run(
            std::span<const std::wstring_view>{arguments});
    } catch (const std::exception& error) {
        constexpr std::size_t MaximumStartupErrorBytes = 4096U;
        const std::string_view message{error.what()};
        std::cerr << "Forge Conductor session-host startup failed: "
                  << (message.size() <= MaximumStartupErrorBytes
                          ? message
                          : std::string_view{
                                "The detailed startup error exceeded the reporting bound."})
                  << '\n';
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr
            << "Forge Conductor session-host startup failed safely.\n";
        return EXIT_FAILURE;
    }
}
