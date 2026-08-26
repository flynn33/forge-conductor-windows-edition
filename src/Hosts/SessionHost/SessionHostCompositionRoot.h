#pragma once

#include <memory>
#include <span>
#include <string_view>

namespace ForgeConductor::Hosts::SessionHost {

class SessionHostCompositionRoot final {
public:
    SessionHostCompositionRoot();
    ~SessionHostCompositionRoot() noexcept;

    SessionHostCompositionRoot(const SessionHostCompositionRoot&) = delete;
    SessionHostCompositionRoot& operator=(const SessionHostCompositionRoot&) = delete;
    SessionHostCompositionRoot(SessionHostCompositionRoot&&) = delete;
    SessionHostCompositionRoot& operator=(SessionHostCompositionRoot&&) = delete;

    [[nodiscard]] int run(
        std::span<const std::wstring_view> arguments) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Hosts::SessionHost
