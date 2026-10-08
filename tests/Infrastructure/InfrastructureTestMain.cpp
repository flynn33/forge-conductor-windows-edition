#include "TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioChatControl.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"

#include <exception>
#include <iostream>
namespace ForgeConductor::Tests {

void registerFoundationWindowsTests(TestRegistry& tests);
void registerStorageWindowsTests(TestRegistry& tests);
void registerDiagnosticWindowsTests(TestRegistry& tests);
void registerLMStudioDeploymentServiceTests(TestRegistry& tests);
void registerLMStudioConfigurationCodecTests(TestRegistry& tests);
void registerLMStudioServeVerifierTests(TestRegistry& tests);
void registerUnicodeCanonicalizerWindowsTests(TestRegistry& tests);
void registerWindowsLMStudioEnvironmentTests(TestRegistry& tests);
void registerWindowsLMStudioConversationReaderTests(TestRegistry& tests);
void registerWindowsLMStudioChatControlTests(TestRegistry& tests);
void registerWindowsLMStudioHostActivatorTests(TestRegistry& tests);

} // namespace ForgeConductor::Tests

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view{argv[1]} == "--activate-lmstudio-chat") {
        namespace Domain = ForgeConductor::Domain;
        namespace Infrastructure = ForgeConductor::Infrastructure::Windows;
        Infrastructure::SystemClock clock;
        Infrastructure::WindowsUuidGenerator uuid;
        const auto id = uuid.next();
        const auto executable = Domain::PathText::create(argv[2]);
        const auto correlation = Domain::CorrelationId::parse("visible-chat-existing-host-activation");
        if (!id || !executable || !correlation) { return 2; }
        const Domain::OperationContext operation{Domain::OperationId{id.value()},
            clock.monotonicNow() + std::chrono::seconds{30}, {}, correlation.value()};
        auto result = Infrastructure::WindowsLMStudioChatControl::activate(executable.value(), operation);
        if (!result) { std::cerr << result.error().code << ": " << result.error().message << '\n'; return 1; }
        std::cout << "LM Studio existing host activated through product controller; no New or Send requested.\n";
        return 0;
    }
    if (argc == 4 && std::string_view{argv[1]} == "--pause-lmstudio-chat") {
        namespace Domain = ForgeConductor::Domain;
        namespace Infrastructure = ForgeConductor::Infrastructure::Windows;
        Infrastructure::SystemClock clock;
        Infrastructure::WindowsUuidGenerator uuid;
        const auto id = uuid.next();
        const auto executable = Domain::PathText::create(argv[2]);
        const auto correlation = Domain::CorrelationId::parse("visible-chat-completed-tool-pause");
        if (!id || !executable || !correlation) { return 2; }
        const Domain::OperationContext operation{Domain::OperationId{id.value()},
            clock.monotonicNow() + std::chrono::seconds{20}, {}, correlation.value()};
        auto result = Infrastructure::WindowsLMStudioChatControl::pauseAtToolBoundary(executable.value(), argv[3], operation);
        if (!result) { std::cerr << result.error().code << ": " << result.error().message << '\n'; return 1; }
        std::cout << (result.value() ? "LM Studio native tool pause confirmed.\n" : "LM Studio pause deferred: no completed native tool boundary or inactive selected chat was observed.\n");
        return result.value() ? 0 : 3;
    }
    if(argc == 4 && (std::string_view{argv[1]} == "--start-lmstudio-chat" || std::string_view{argv[1]} == "--send-lmstudio-chat" || std::string_view{argv[1]} == "--close-lmstudio-window")) {
        namespace Domain = ForgeConductor::Domain;
        namespace Infrastructure = ForgeConductor::Infrastructure::Windows;
        Infrastructure::SystemClock clock;
        Infrastructure::WindowsUuidGenerator uuid;
        const auto id=uuid.next();
        const auto executable=Domain::PathText::create(argv[2]);
        const auto correlation=Domain::CorrelationId::parse("visible-chat-integration-predecessor");
        if(!id || !executable || !correlation) return 2;
        const Domain::OperationContext operation{Domain::OperationId{id.value()},
            clock.monotonicNow()+std::chrono::seconds{45},{},correlation.value()};
        const bool closing=std::string_view{argv[1]}=="--close-lmstudio-window";
        auto result=closing
            ? Infrastructure::WindowsLMStudioChatControl::closeWindow(executable.value(),operation,argv[3])
            : Infrastructure::WindowsLMStudioChatControl::send(executable.value(),argv[3],std::string_view{argv[1]}=="--start-lmstudio-chat",operation);
        if(!result) {std::cerr<<result.error().code<<": "<<result.error().message<<'\n';return 1;}
        std::cout<<(closing?"LM Studio verification window closed normally.\n":"Predecessor LM Studio chat request sent through product controller.\n");return 0;
    }
    ForgeConductor::Tests::TestRegistry tests;
    ForgeConductor::Tests::registerFoundationWindowsTests(tests);
    ForgeConductor::Tests::registerStorageWindowsTests(tests);
    ForgeConductor::Tests::registerDiagnosticWindowsTests(tests);
    ForgeConductor::Tests::registerLMStudioDeploymentServiceTests(tests);
    ForgeConductor::Tests::registerLMStudioConfigurationCodecTests(tests);
    ForgeConductor::Tests::registerLMStudioServeVerifierTests(tests);
    ForgeConductor::Tests::registerUnicodeCanonicalizerWindowsTests(tests);
    ForgeConductor::Tests::registerWindowsLMStudioEnvironmentTests(tests);
    ForgeConductor::Tests::registerWindowsLMStudioConversationReaderTests(tests);
    ForgeConductor::Tests::registerWindowsLMStudioChatControlTests(tests);
    ForgeConductor::Tests::registerWindowsLMStudioHostActivatorTests(tests);

    std::size_t passed = 0U;
    for (const auto& [name, run] : tests) {
        try {
            std::cout << "[RUN] " << name << '\n' << std::flush;
            run();
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
            return 1;
        } catch (...) {
            std::cerr << "[FAIL] " << name << ": unknown exception\n";
            return 1;
        }
    }
    std::cout << passed << '/' << tests.size() << " Windows infrastructure unit tests passed.\n";
    return 0;
}
