#include "CliCompositionRoot.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::string strictWideToUtf8(const std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error{"A CLI argument is not valid UTF-16."};
    }
    std::string converted(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), converted.data(), required,
            nullptr, nullptr) != required) {
        throw std::runtime_error{"A CLI argument could not be converted to UTF-8."};
    }
    return converted;
}

} // namespace

int wmain(const int argc, wchar_t** const argv)
{
    try {
        std::vector<std::string> argumentStorage;
        argumentStorage.reserve(
            argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
        for (int index = 1; index < argc; ++index) {
            argumentStorage.push_back(strictWideToUtf8(argv[index]));
        }

        std::vector<std::string_view> arguments;
        arguments.reserve(argumentStorage.size());
        for (const auto& argument : argumentStorage) {
            arguments.emplace_back(argument);
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
