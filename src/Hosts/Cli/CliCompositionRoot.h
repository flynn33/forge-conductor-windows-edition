#pragma once

#include <memory>
#include <span>
#include <string_view>

namespace ForgeConductor::Hosts::Cli {

class CliApplication;

class CliCompositionRoot final {
public:
    CliCompositionRoot();
    ~CliCompositionRoot();

    CliCompositionRoot(const CliCompositionRoot&) = delete;
    CliCompositionRoot& operator=(const CliCompositionRoot&) = delete;
    CliCompositionRoot(CliCompositionRoot&&) = delete;
    CliCompositionRoot& operator=(CliCompositionRoot&&) = delete;

    [[nodiscard]] int run(std::span<const std::string_view> arguments) const;

private:
    std::unique_ptr<CliApplication> application_;
};

} // namespace ForgeConductor::Hosts::Cli
