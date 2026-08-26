#include "TestSupport.h"

#include <exception>
#include <iostream>
#include <string>

namespace ForgeConductor::Tests {

void registerProcessWindowsTests(TestRegistry& tests, const std::wstring& fixturePath);
void registerOverlappedPipeReaderTests(TestRegistry& tests);

} // namespace ForgeConductor::Tests

int wmain(const int argumentCount, wchar_t** const arguments)
{
    if (argumentCount != 2 || arguments == nullptr || arguments[1] == nullptr) {
        std::wcerr << L"Usage: process-tests <absolute-fixture-path>\n";
        return 2;
    }

    ForgeConductor::Tests::TestRegistry tests;
    ForgeConductor::Tests::registerProcessWindowsTests(tests, std::wstring{arguments[1]});
    ForgeConductor::Tests::registerOverlappedPipeReaderTests(tests);

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
    std::cout << passed << '/' << tests.size() << " Windows process integration tests passed.\n";
    return 0;
}
