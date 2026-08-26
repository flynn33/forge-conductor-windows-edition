#include "TestSupport.h"

#include <exception>
#include <iostream>

namespace ForgeConductor::Tests {

void registerFoundationWindowsTests(TestRegistry& tests);
void registerStorageWindowsTests(TestRegistry& tests);
void registerDiagnosticWindowsTests(TestRegistry& tests);
void registerUnicodeCanonicalizerWindowsTests(TestRegistry& tests);

} // namespace ForgeConductor::Tests

int main()
{
    ForgeConductor::Tests::TestRegistry tests;
    ForgeConductor::Tests::registerFoundationWindowsTests(tests);
    ForgeConductor::Tests::registerStorageWindowsTests(tests);
    ForgeConductor::Tests::registerDiagnosticWindowsTests(tests);
    ForgeConductor::Tests::registerUnicodeCanonicalizerWindowsTests(tests);

    std::size_t passed = 0U;
    for (const auto& [name, run] : tests) {
        try {
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
