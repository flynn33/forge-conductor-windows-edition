#include "Infrastructure/TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerBootstrapCommand.h"
#include "Infrastructure/Windows/Detail/IWindowsManagerBootstrapPlatform.h"

#include <nlohmann/json.hpp>
#include <memory>
#include <filesystem>
#include <sstream>
#include <string>

namespace ForgeConductor::Infrastructure::Windows::Detail {
struct WindowsManagerBootstrapCommandTestAccess final {
    [[nodiscard]] static std::unique_ptr<WindowsManagerBootstrapCommand> create(
        std::unique_ptr<IWindowsManagerBootstrapPlatform> platform)
    {
        return std::unique_ptr<WindowsManagerBootstrapCommand>{
            new WindowsManagerBootstrapCommand{std::move(platform)}};
    }
};
}

namespace ForgeConductor::Tests {
namespace {
namespace W = Infrastructure::Windows;
namespace Detail = W::Detail;
using Json = nlohmann::json;
class RecordingBootstrap final : public Detail::IWindowsManagerBootstrapPlatform {
public:
    std::size_t calls{};
    std::string home;
    bool isolated{};
    bool fail{};
    [[nodiscard]] Domain::Result<void> start(const Domain::PathText& value, const bool mode) noexcept override
    {
        ++calls; home = value.value(); isolated = mode;
        if (fail) return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::ProcessLaunchFailed, "Unbounded platform text must not escape: " + home));
        return Domain::Result<void>::success();
    }
};

void fixedManagerProfileIsTheOnlyBootstrapInput()
{
    auto platform = std::make_unique<RecordingBootstrap>();
    auto* recorded = platform.get();
    auto command = Detail::WindowsManagerBootstrapCommandTestAccess::create(std::move(platform));
    const std::string home = "C:\\Forge Home\\isolated-測試";
    for (const bool isolated : {false, true}) {
        std::istringstream input{Json{{"home", home}, {"isolated_profile", isolated}}.dump()};
        take(command->run(input));
        require(recorded->home == home && recorded->isolated == isolated,
            "The exact selected home/profile mode changed at the desktop launch boundary.");
    }
    require(recorded->calls == 2U, "The bootstrap did not delegate exactly once per valid request.");
    recorded->fail = true;
    std::istringstream input{Json{{"home", home}, {"isolated_profile", true}}.dump()};
    const auto result = command->run(input);
    requireError(result, Domain::ErrorCodes::HostCapabilityUnavailable, "The broker failure was not retained.");
    require(result.error().message.find(home) == std::string::npos,
        "The helper reflected platform output or selected path into its public failure.");
    require(recorded->calls == 3U, "The helper retried a failed desktop bootstrap.");
}

void malformedBootstrapCannotReachDesktopLaunch()
{
    auto platform = std::make_unique<RecordingBootstrap>();
    auto* recorded = platform.get();
    auto command = Detail::WindowsManagerBootstrapCommandTestAccess::create(std::move(platform));
    const Json good{{"home", "C:\\Forge Home"}, {"isolated_profile", true}};
    std::vector<std::string> rejected{"", "{", "[]", "null", "{}", "\"C:\\\\Forge Home\""};
    for (const auto* extra : {"executable", "arguments", "verb", "environment", "parent_pid"}) {
        auto request = good; request[extra] = "arbitrary"; rejected.push_back(request.dump());
    }
    for (const auto& value : {Json(), Json(2), Json(false), Json::array({"path"})}) {
        auto request = good; request["home"] = value; rejected.push_back(request.dump());
    }
    for (const auto& value : {Json(), Json(2), Json("true"), Json::array({"path"})}) {
        auto request = good; request["isolated_profile"] = value; rejected.push_back(request.dump());
    }
    auto oversizedHome = good; oversizedHome["home"] = "C:\\" + std::string(Domain::PathText::MaximumBytes, 'p');
    rejected.push_back(oversizedHome.dump());
    auto nul = good; nul["home"] = std::string{"C:\\home\0--open", 14U}; rejected.push_back(nul.dump());
    for (const auto& bytes : rejected) {
        std::istringstream input{bytes};
        requireError(command->run(input), Domain::ErrorCodes::InvalidRequest,
            "Malformed bootstrap input reached the desktop Shell.");
    }
    std::istringstream oversized{std::string(W::WindowsManagerBootstrapCommand::MaximumRequestBytes + 1U, 'x')};
    requireError(command->run(oversized), Domain::ErrorCodes::PayloadTooLarge,
        "The bootstrap request byte cap did not hold.");
    require(recorded->calls == 0U, "Invalid bootstrap input invoked the launch platform.");
}

void bootstrapCannotRunWithoutPlatformOrReadTransport()
{
    auto command = Detail::WindowsManagerBootstrapCommandTestAccess::create({});
    const auto bytes = Json{{"home", "C:\\Forge Home"}, {"isolated_profile", true}}.dump();
    std::istringstream input{bytes};
    requireError(command->run(input), Domain::ErrorCodes::IntegrityFailure,
        "Missing platform was not a typed failure.");
    std::istringstream failed{bytes}; failed.setstate(std::ios::badbit);
    requireError(command->run(failed), Domain::ErrorCodes::TransportClosed,
        "A failed bootstrap input stream was admitted.");
    auto native = Detail::createWindowsManagerBootstrapPlatform();
    for (const auto* path : {"relative", "C:relative", "\\\\server\\share\\home", "C:\\", "C:/Forge Home",
        "C:\\..\\home", "C:\\.\\home", "C:\\home\\", "C:\\home.", "C:\\home ", "C:\\home\n--open"})
        require(!native->start(take(Domain::PathText::create(path)), true),
            "Native bootstrap admitted a home outside the existing Manager profile contract.");
    require(!std::filesystem::exists("C:\\ForgeConductor-nonexistent-bootstrap-home"),
        "The native nonexistent-home fixture unexpectedly exists.");
    requireError(native->start(take(Domain::PathText::create("C:\\ForgeConductor-nonexistent-bootstrap-home")), true),
        Domain::ErrorCodes::InvalidRequest, "Native bootstrap admitted a nonexistent home.");
}
}

void registerWindowsManagerBootstrapCommandTests(TestRegistry& tests)
{
    tests.emplace_back("manager bootstrap fixed profile input", fixedManagerProfileIsTheOnlyBootstrapInput);
    tests.emplace_back("manager bootstrap rejects malformed and arbitrary launch input", malformedBootstrapCannotReachDesktopLaunch);
    tests.emplace_back("manager bootstrap platform and input failures", bootstrapCannotRunWithoutPlatformOrReadTransport);
}
}
