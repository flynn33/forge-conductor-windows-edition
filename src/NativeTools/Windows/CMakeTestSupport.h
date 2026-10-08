#pragma once

#include "ForgeConductor/Domain/ShellJobModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Contracts/AuthorityCapabilities.h"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace ForgeConductor::NativeTools::Windows::Detail {

inline constexpr std::size_t MaximumCTestReportBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t MaximumCTestFailuresPerPage = 32U;
struct CTestReportPage final {
    Domain::CMakeTestCounts counts;
    std::vector<Domain::CMakeTestFailure> failures;
};

[[nodiscard]] Domain::Result<void> validateCMakeTestRequest(
    const Domain::CMakeTestRequest&, const Contracts::WorkspaceAuthority&,
    const Domain::OperationContext&) noexcept;
[[nodiscard]] Domain::Result<void> validateFreshCTestReport(const Domain::CMakeTestMetadata&) noexcept;
[[nodiscard]] Domain::Result<CTestReportPage> parseCTestJUnit(
    std::string_view, std::uint64_t offset, std::size_t limit,
    const Domain::OperationContext* context = nullptr) noexcept;
void captureCTestReport(Domain::CMakeTestMetadata&,
    const Domain::OperationContext* context = nullptr);
[[nodiscard]] Domain::Result<CTestReportPage> readCTestReport(
    const Domain::CMakeTestMetadata&, std::uint64_t offset, std::size_t limit,
    const Domain::OperationContext* context = nullptr) noexcept;
[[nodiscard]] nlohmann::json encodeCMakeTestMetadata(const Domain::CMakeTestMetadata&);
[[nodiscard]] Domain::CMakeTestMetadata decodeCMakeTestMetadata(const nlohmann::json&);
void validateCTestReceipt(const Domain::ShellJobSnapshot&, const std::filesystem::path& directory);

} // namespace ForgeConductor::NativeTools::Windows::Detail
