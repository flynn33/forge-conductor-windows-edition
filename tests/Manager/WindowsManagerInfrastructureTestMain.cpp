#include "Infrastructure/TestSupport.h"

#include <exception>
#include <iostream>

namespace ForgeConductor::Tests {

void registerWindowsManagerAuthenticationTests(TestRegistry& tests);
void registerWindowsManagerOwnershipTests(TestRegistry& tests);
void registerManagerPipeInfrastructureTests(TestRegistry& tests);

} // namespace ForgeConductor::Tests

int main()
{
    ForgeConductor::Tests::TestRegistry tests;
    ForgeConductor::Tests::registerWindowsManagerAuthenticationTests(tests);
    ForgeConductor::Tests::registerWindowsManagerOwnershipTests(tests);
    ForgeConductor::Tests::registerManagerPipeInfrastructureTests(tests);

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

    std::cout << passed << '/' << tests.size()
              << " manager Windows infrastructure tests passed.\n";
    return 0;
}
