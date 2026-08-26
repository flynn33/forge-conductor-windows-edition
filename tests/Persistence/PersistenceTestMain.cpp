#include "PersistenceTestSupport.h"

#include <exception>
#include <filesystem>
#include <iostream>

namespace ForgeConductor::Tests {

void registerWinsqliteKernelTests(TestRegistry& tests);
void registerCentralMigrationTests(TestRegistry& tests, const std::filesystem::path& fixtures);
void registerProjectMigrationTests(TestRegistry& tests, const std::filesystem::path& fixtures);
void registerBackupIntegrityTests(TestRegistry& tests);
void registerDatabaseConcurrencyTests(
    TestRegistry& tests,
    const std::filesystem::path& processFixture);
void registerDatabaseAuthorityTests(TestRegistry& tests);
void registerDatabaseStoreSerializationTests(TestRegistry& tests);
void registerIntegrityRecoveryTests(TestRegistry& tests);

} // namespace ForgeConductor::Tests

int wmain(const int argumentCount, wchar_t** arguments)
{
    if (argumentCount != 3 || arguments == nullptr || arguments[1] == nullptr ||
        arguments[2] == nullptr) {
        std::cerr << "Usage: ForgeConductor.Persistence.UnitTests "
                     "<fixture-directory> <process-fixture-executable>\n";
        return 2;
    }

    const std::filesystem::path fixtures{arguments[1]};
    const std::filesystem::path processFixture{arguments[2]};
    std::error_code pathError;
    if (!std::filesystem::is_directory(fixtures, pathError) || pathError) {
        std::cerr << "Persistence fixture directory is unavailable.\n";
        return 2;
    }
    pathError.clear();
    if (!std::filesystem::is_regular_file(processFixture, pathError) || pathError) {
        std::cerr << "Persistence process fixture is unavailable.\n";
        return 2;
    }

    ForgeConductor::Tests::TestRegistry tests;
    ForgeConductor::Tests::registerWinsqliteKernelTests(tests);
    ForgeConductor::Tests::registerCentralMigrationTests(tests, fixtures);
    ForgeConductor::Tests::registerProjectMigrationTests(tests, fixtures);
    ForgeConductor::Tests::registerBackupIntegrityTests(tests);
    ForgeConductor::Tests::registerDatabaseConcurrencyTests(tests, processFixture);
    ForgeConductor::Tests::registerDatabaseAuthorityTests(tests);
    ForgeConductor::Tests::registerDatabaseStoreSerializationTests(tests);
    ForgeConductor::Tests::registerIntegrityRecoveryTests(tests);

    std::size_t passed{};
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

    std::cout << passed << '/' << tests.size() << " persistence tests passed.\n";
    return 0;
}
