#include "Infrastructure/TestSupport.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "ForgeConductor/NativeTools/Windows/WindowsShellService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "NativeTools/Windows/CMakeTestSupport.h"
#include "NativeTools/Windows/ShellJobStorage.h"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>

namespace {
using namespace ForgeConductor;
using namespace ForgeConductor::Tests;
using namespace std::chrono_literals;
namespace Native = NativeTools::Windows;
namespace Detail = NativeTools::Windows::Detail;
namespace InfrastructureDetail = Infrastructure::Windows::Detail;
using Json = nlohmann::json;

Domain::PathText path(const std::filesystem::path& value) {
    return take(InfrastructureDetail::WindowsPathResolver::toPathText(std::filesystem::absolute(value).wstring()));
}
Domain::OperationContext context(unsigned index = 1U) {
    char id[37]{};
    require(std::snprintf(id, sizeof(id), "%08x-0000-4000-8000-%012x", index, index) == 36, "operation formatting failed");
    return {parse<Domain::OperationId>(id), std::chrono::steady_clock::now() + 30s, {},
        parse<Domain::CorrelationId>("native-cmake-test-tests")};
}
Contracts::WorkspaceAuthority authority(const Domain::PathText& root, bool write = true, bool shell = true,
    std::string_view project = "20000000-0000-4000-8000-000000000001") {
    std::vector<Domain::FileAccess> grants{Domain::FileAccess::Read, Domain::FileAccess::Execute};
    if (write) grants.push_back(Domain::FileAccess::Write);
    Fakes::DeterministicWorkspaceAuthority issuer{parse<Domain::AuthorityId>("30000000-0000-4000-8000-000000000001"),
        parse<Domain::ClientId>("40000000-0000-4000-8000-000000000001"), {root}, Domain::FileAccess::Execute,
        std::move(grants), {}, shell, 1U};
    issuer.setNow(std::chrono::steady_clock::now());
    return take(issuer.authorityFor(parse<Domain::ProjectId>(project), context()));
}
void write(const std::filesystem::path& destination, std::string_view content) {
    std::ofstream file{destination, std::ios::binary | std::ios::trunc};
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(static_cast<bool>(file), "private fixture write failed");
}
std::string read(const std::filesystem::path& file) {
    std::ifstream input{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}
std::string digest(std::string_view value) {
    Infrastructure::Windows::BCryptSha256Hasher hasher;
    return take(hasher.sha256(std::as_bytes(std::span{value.data(), value.size()}))).value();
}
void writeReceipt(const std::filesystem::path& destination, Json value) {
    value["sha256"] = digest(value.at("payload").dump());
    write(destination, value.dump());
}
std::filesystem::path native(std::string_view text) {
    return {std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}
struct Workspace {
    std::filesystem::path root;
    Workspace() {
        LARGE_INTEGER counter{}; require(::QueryPerformanceCounter(&counter) != FALSE, "private path nonce failed");
        root = std::filesystem::temp_directory_path() / (L"Forge CTest Ω " + std::to_wstring(::GetCurrentProcessId()) + L" " + std::to_wstring(counter.QuadPart));
        require(std::filesystem::create_directory(root), "private workspace creation failed");
    }
    ~Workspace() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
class LaunchFailureSupervisor final : public Contracts::IProcessSupervisor {
public:
    std::atomic<unsigned> calls{};
    Domain::Result<Domain::ProcessResult> run(const Domain::ProcessRequest&, const Contracts::WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept override {
        if (calls.fetch_add(1U) == 0U) return Domain::Result<Domain::ProcessResult>::success(Domain::ProcessResult{});
        return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(Domain::ErrorCodes::ProcessLaunchFailed, "Scripted second-phase launch failure."));
    }
    void cancel(const Domain::OperationId&) noexcept override {}
    void cancelAll() noexcept override {}
    void shutdown() noexcept override {}
};
Domain::ShellJobSnapshot terminal(Native::WindowsShellService& service, std::string_view id, const Contracts::WorkspaceAuthority& scope) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    for (;;) {
        auto job = take(service.getJob(id, scope, context(5U)));
        if (job.state != Domain::ShellJobState::Running) return job;
        require(std::chrono::steady_clock::now() < deadline, "private job failed to finish");
        std::this_thread::sleep_for(20ms);
    }
}
Domain::CMakeTestRunStatus run(Native::WindowsShellService& service, Domain::CMakeTestRequest request, const Contracts::WorkspaceAuthority& scope) {
    const auto started = take(service.startCMakeTestRun(request, scope, context(2U)));
    static_cast<void>(terminal(service, started.jobId, scope));
    return take(service.getCMakeTestRun(started.jobId, 0U, 8U, scope, context(3U)));
}
void parserTests() {
    const std::string report = R"(<?xml version="1.0" encoding="UTF-8"?><testsuite tests="4" failures="1" skipped="1" disabled="1"><testcase name="pass" status="run"><properties/><system-out>passed</system-out></testcase><testcase name="fail Ω" status="fail"><failure message="Failed"/><properties/><system-out>expected &lt;value&gt;
actual output</system-out></testcase><testcase name="skip" status="notrun"><skipped message="SKIP_RETURN_CODE"/><properties/><system-out/></testcase><testcase name="disabled" status="disabled"><properties/><system-out/></testcase></testsuite>)";
    const auto parsed = take(Detail::parseCTestJUnit(report, 0U, 1U));
    require(parsed.counts == Domain::CMakeTestCounts{4U, 1U, 1U, 1U, 1U} && parsed.failures.size() == 1U &&
        parsed.failures[0].name == "fail Ω" && parsed.failures[0].message == "Failed" &&
        parsed.failures[0].output == "expected <value>\nactual output", "CTest producer-shaped fixture was not parsed exactly");
    require(take(Detail::parseCTestJUnit(report, 1U, 1U)).failures.empty(), "CTest failure EOF repeated a case");
    require(!Detail::parseCTestJUnit(report, 2U, 1U), "CTest accepted failure offset past EOF");
    auto mismatch = report; mismatch.replace(mismatch.find("tests=\"4\""), 9U, "tests=\"5\"");
    require(!Detail::parseCTestJUnit(mismatch, 0U, 1U), "CTest trusted declared counts over actual cases");
    require(!Detail::parseCTestJUnit(report.substr(0U, report.size() - 12U), 0U, 1U), "CTest accepted partial XML");
    require(!Detail::parseCTestJUnit(R"(<!DOCTYPE testsuite [<!ENTITY x SYSTEM "file:///C:/Windows/win.ini">]><testsuite tests="0" failures="0" skipped="0" disabled="0">&x;</testsuite>)", 0U, 1U),
        "CTest accepted a DTD/external entity");
    require(!Detail::parseCTestJUnit(R"(<testsuite xmlns="urn:other" tests="0" failures="0" skipped="0" disabled="0"/>)", 0U, 1U), "CTest accepted another XML vocabulary");
    require(!Detail::parseCTestJUnit(R"(<testsuite xmlns:x="urn:other" x:tests="0" failures="0" skipped="0" disabled="0"/>)", 0U, 1U), "CTest accepted a namespaced count attribute");
    require(!Detail::parseCTestJUnit(R"(<testsuite tests="1" failures="0" skipped="0" disabled="0"><testcase name="pass" status="run"><properties><property name="p"><unexpected/></property></properties></testcase></testsuite>)", 0U, 1U), "CTest accepted unsupported nested result data");
    require(!Detail::parseCTestJUnit(R"(<testsuite tests="0" failures="0" skipped="0" disabled="0">unexpected data</testsuite>)", 0U, 1U), "CTest accepted unsupported suite mixed text");
    require(!Detail::parseCTestJUnit(R"(<testsuite tests="1" failures="0" skipped="0" disabled="0"><testcase name="pass" status="run"><system-out/><system-out/></testcase></testsuite>)", 0U, 1U), "CTest accepted duplicate output elements");
    require(!Detail::parseCTestJUnit(R"(<testsuite tests="1" failures="1" skipped="0" disabled="0"><testcase name="fail" status="fail"><failure/><system-out/>unexpected data</testcase></testsuite>)", 0U, 1U), "CTest accepted mixed text after an empty output element");
    require(!Detail::parseCTestJUnit(std::string(Detail::MaximumCTestReportBytes + 1U, 'x'), 0U, 1U), "CTest report byte bound was not enforced");
    const auto longReport = std::string{"<testsuite tests=\"1\" failures=\"1\" skipped=\"0\" disabled=\"0\"><testcase name=\"fail\" status=\"fail\"><failure message=\"Failed\"/><system-out>"} +
        std::string(5000U, 'x') + "</system-out></testcase></testsuite>";
    const auto bounded = take(Detail::parseCTestJUnit(longReport, 0U, 1U));
    require(bounded.failures[0].output.size() == 2048U && bounded.failures[0].outputTruncated, "CTest failure excerpt lost its bound/truncation flag");
    const std::string emptyReport = "<?xml version=\"1.0\"?><testsuite tests=\"0\" failures=\"0\" skipped=\"0\" disabled=\"0\"/>";
    const auto empty = take(Detail::parseCTestJUnit(emptyReport, 0U, 0U));
    require(empty.counts.tests == 0U, "Actual empty JUnit counts were invented");
    require(take(Detail::parseCTestJUnit(std::string{"\xef\xbb\xbf"} + emptyReport, 0U, 0U)).counts.tests == 0U,
        "CTest rejected an ordinary UTF-8 BOM");
    const auto forcedUtf8 = take(Detail::parseCTestJUnit(
        "<?xml version=\"1.0\" encoding=\"windows-1252\"?><testsuite tests=\"1\" failures=\"1\" skipped=\"0\" disabled=\"0\">"
        "<testcase name=\"fail Ω\" status=\"fail\"><failure message=\"échec\"/><system-out>actual Ω café</system-out></testcase></testsuite>", 0U, 1U));
    require(forcedUtf8.failures[0].name == "fail Ω" && forcedUtf8.failures[0].message == "échec" &&
        forcedUtf8.failures[0].output == "actual Ω café", "CTest decoded valid UTF-8 through a conflicting non-UTF-8 declaration");
    std::string utf16LE, utf16BE;
    for (const auto character : emptyReport) {
        utf16LE.push_back(character); utf16LE.push_back('\0');
        utf16BE.push_back('\0'); utf16BE.push_back(character);
    }
    require(!Detail::parseCTestJUnit(utf16LE, 0U, 0U), "CTest accepted BOMless UTF-16LE as bounded UTF-8");
    require(!Detail::parseCTestJUnit(utf16BE, 0U, 0U), "CTest accepted BOMless UTF-16BE as bounded UTF-8");
    auto nulReport = emptyReport; nulReport.insert(20U, 1U, '\0');
    require(!Detail::parseCTestJUnit(nulReport, 0U, 0U), "CTest accepted an embedded XML NUL");
    require(!Detail::parseCTestJUnit(std::string{"\xc3\x28"} + emptyReport, 0U, 0U), "CTest accepted invalid UTF-8");
    std::string escapedReport{"<testsuite tests=\"32\" failures=\"32\" skipped=\"0\" disabled=\"0\">"};
    for (unsigned index = 0U; index != 32U; ++index) {
        escapedReport += "<testcase name=\"" + std::to_string(index);
        for (unsigned character = 0U; character != 1020U; ++character) escapedReport += "&quot;";
        escapedReport += "\" status=\"fail\"><failure message=\"";
        for (unsigned character = 0U; character != 1024U; ++character) escapedReport += "&quot;";
        escapedReport += "\"/><system-out>" + std::string(2048U, '\\') + "</system-out></testcase>";
    }
    escapedReport += "</testsuite>";
    const auto byteBounded = take(Detail::parseCTestJUnit(escapedReport, 0U, 32U));
    Json page = Json::array();
    for (const auto& item : byteBounded.failures) page.push_back({{"name", item.name}, {"status", item.status},
        {"message", item.message}, {"output", item.output}, {"output_truncated", item.outputTruncated}});
    require(!byteBounded.failures.empty() && byteBounded.failures.size() < 32U && page.dump().size() <= 24U * 1024U,
        "Worst-case JSON escaping exceeded the CTest failure-page byte budget");
    const auto next = take(Detail::parseCTestJUnit(escapedReport, byteBounded.failures.size(), 32U));
    require(next.failures.front().name.starts_with(std::to_string(byteBounded.failures.size())), "Byte-limited CTest paging skipped a failure");
    auto stopped = context(); std::stop_source cancellation; cancellation.request_stop(); stopped.cancellation = cancellation.get_token();
    const auto cancelledParse = Detail::parseCTestJUnit(report, 0U, 1U, &stopped);
    require(!cancelledParse && cancelledParse.error().code == Domain::ErrorCodes::Cancelled, "Small CTest parsing ignored cancellation");
    const auto largeReport = std::string{"<testsuite tests=\"1\" failures=\"1\" skipped=\"0\" disabled=\"0\"><testcase name=\"large\" status=\"fail\"><failure/><system-out>"} +
        std::string(Detail::MaximumCTestReportBytes - 1024U, 'x') + "</system-out></testcase></testsuite>";
    const auto large = take(Detail::parseCTestJUnit(largeReport, 0U, 1U));
    require(large.counts == Domain::CMakeTestCounts{1U,0U,1U,0U,0U} && large.failures[0].outputTruncated,
        "Near-bound valid CTest output was not parsed and bounded");
    auto shortDeadline = context(); shortDeadline.deadline = std::chrono::steady_clock::now() + 1ms;
    const auto deadlineStarted = std::chrono::steady_clock::now();
    const auto deadlineResult = Detail::parseCTestJUnit(largeReport, 0U, 1U, &shortDeadline);
    const auto deadlineElapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - deadlineStarted);
    require(!deadlineResult && deadlineResult.error().code == Domain::ErrorCodes::DeadlineExceeded,
        "Near-bound CTest parse published counts after its deadline; elapsed_us=" + std::to_string(deadlineElapsed.count()));
    unsigned cancellationAttempts{};
    bool cancelledDuringParse{};
    std::chrono::microseconds cancellationElapsed{}, requestStartedElapsed{}, requestCompletedElapsed{};
    for (; cancellationAttempts != 10U && !cancelledDuringParse; ++cancellationAttempts) {
        std::stop_source duringParse; auto cancelDuringParse = context(); cancelDuringParse.cancellation = duringParse.get_token();
        std::latch workerReady{1}, beginCancellation{1};
        std::chrono::steady_clock::time_point requestStarted{}, requestCompleted{};
        bool stopAccepted{};
        std::jthread canceller{[&] {
            workerReady.count_down();
            beginCancellation.wait();
            std::this_thread::sleep_for(1ms);
            requestStarted = std::chrono::steady_clock::now();
            stopAccepted = duringParse.request_stop();
            requestCompleted = std::chrono::steady_clock::now();
        }};
        workerReady.wait();
        beginCancellation.count_down();
        const auto parsingStarted = std::chrono::steady_clock::now();
        const auto cancellationResult = Detail::parseCTestJUnit(largeReport, 0U, 1U, &cancelDuringParse);
        const auto parsingReturned = std::chrono::steady_clock::now();
        canceller.join();
        require(stopAccepted, "CTest cancellation worker did not request cancellation");
        cancellationElapsed = std::chrono::duration_cast<std::chrono::microseconds>(parsingReturned - parsingStarted);
        requestStartedElapsed = std::chrono::duration_cast<std::chrono::microseconds>(requestStarted - parsingStarted);
        requestCompletedElapsed = std::chrono::duration_cast<std::chrono::microseconds>(requestCompleted - parsingStarted);
        if (requestStarted <= parsingStarted || requestCompleted >= parsingReturned) continue;
        require(!cancellationResult && cancellationResult.error().code == Domain::ErrorCodes::Cancelled,
            "Near-bound CTest parse published counts after a during-parse cancellation; elapsed_us=" + std::to_string(cancellationElapsed.count()) +
            " request_started_us=" + std::to_string(requestStartedElapsed.count()) + " request_completed_us=" + std::to_string(requestCompletedElapsed.count()));
        cancelledDuringParse = true;
    }
    require(cancelledDuringParse, "No during-parse CTest cancellation was exercised in 10 attempts; last_elapsed_us=" +
        std::to_string(cancellationElapsed.count()) + " last_request_started_us=" + std::to_string(requestStartedElapsed.count()) +
        " last_request_completed_us=" + std::to_string(requestCompletedElapsed.count()));
    std::cout << "Near-bound CTest context guards: bytes=" << largeReport.size() << " deadline_elapsed_us=" << deadlineElapsed.count()
              << " cancellation_attempts=" << cancellationAttempts << " cancellation_elapsed_us=" << cancellationElapsed.count()
              << " request_started_us=" << requestStartedElapsed.count() << " request_completed_us=" << requestCompletedElapsed.count() << '\n';
}
void reportLifecycleTests() {
    Workspace workspace;
    Domain::CMakeTestMetadata metadata;
    const auto report = workspace.root / L"phase-report.xml";
    metadata.reportPath = path(report).value();
    require(static_cast<bool>(Detail::validateFreshCTestReport(metadata)), "Owned new report path was rejected");
    write(report, "<testsuite tests=\"0\" failures=\"0\" skipped=\"0\" disabled=\"0\"/>");
    require(!Detail::validateFreshCTestReport(metadata), "An existing stale report remained eligible for CTest overwrite");
    // A completed process may be interrupted while parsing its final evidence.
    metadata.testResult = Domain::ProcessResult{};
    auto stopped = context(); std::stop_source cancellation; cancellation.request_stop(); stopped.cancellation = cancellation.get_token();
    Detail::captureCTestReport(metadata, &stopped);
    require(!metadata.counts && !metadata.reportSha256.empty() && metadata.reportError &&
        metadata.reportError->code == Domain::ErrorCodes::Cancelled, "Cancelled report parsing inferred complete counts or lost actual report identity");
    auto expired = context(); expired.deadline = std::chrono::steady_clock::now();
    Detail::captureCTestReport(metadata, &expired);
    require(!metadata.counts && metadata.reportError && metadata.reportError->code == Domain::ErrorCodes::DeadlineExceeded,
        "Expired report parsing inferred complete counts");
    Detail::captureCTestReport(metadata);
    require(metadata.counts && metadata.counts->tests == 0U && !metadata.reportError, "Actual empty report was confused with a missing report");
    write(report, "<testsuite>");
    Detail::captureCTestReport(metadata);
    require(!metadata.counts && metadata.reportError && metadata.reportError->code == Domain::ErrorCodes::IntegrityFailure &&
        !metadata.reportSha256.empty(), "Malformed actual report lost its sealed error disposition");
    write(report, std::string(Detail::MaximumCTestReportBytes + 1U, 'x'));
    Detail::captureCTestReport(metadata);
    require(metadata.reportUnverified && !metadata.counts && metadata.reportSha256.empty() && metadata.testResult && metadata.reportError &&
        metadata.reportError->code == Domain::ErrorCodes::IntegrityFailure, "Oversized report invented a seal or lost the actual process result");
    std::filesystem::remove(report); std::filesystem::create_directory(report);
    Detail::captureCTestReport(metadata);
    require(metadata.reportUnverified && !metadata.counts && metadata.reportSha256.empty() && metadata.testResult,
        "Unreadable report path invented a seal or lost the actual process result");
}

void unsealedReceiptByteTests() {
    Workspace workspace;
    const auto jobs = workspace.root / L"jobs";
    const auto scope = authority(path(workspace.root));
    Json outcomes = Json::array(); bool rejected = true;
    // Synthetic interrupted/admission-failed receipts exercise the real storage codec without launching a process.
    for (const auto state : {Domain::ShellJobState::Running, Domain::ShellJobState::Failed}) {
        Domain::ShellJobSnapshot snapshot;
        snapshot.jobId = state == Domain::ShellJobState::Running ? "61000000-0000-4000-8000-000000000001" : "61000000-0000-4000-8000-000000000002";
        snapshot.state = state; snapshot.command = "ctest"; snapshot.cwd = path(workspace.root).value(); snapshot.timeoutSeconds = 1U;
        snapshot.cmakeTest.emplace(); snapshot.cmakeTest->buildDirectory = snapshot.cwd;
        const auto report = (jobs / scope.projectId().value() / (snapshot.jobId + ".ctest.xml")).generic_u8string();
        snapshot.cmakeTest->reportPath = {reinterpret_cast<const char*>(report.data()), report.size()};
        if (state == Domain::ShellJobState::Failed) {
            snapshot.error = Domain::makeError(Domain::ErrorCodes::ProcessLaunchFailed, "Synthetic admission fixture.");
            snapshot.cmakeTest->reportError = Domain::makeError(Domain::ErrorCodes::RecordNotFound, "No report was created.");
        }
        auto storage = take(Detail::ShellJobStorage::create(path(jobs), scope.projectId(), snapshot, context()));
        storage->persist(snapshot); storage.reset();
        const auto receipt = native(snapshot.receiptPath);
        auto envelope = Json::parse(read(receipt));
        envelope["payload"]["job_host_pid"] = 0U; envelope["payload"]["job_host_creation_time"] = 0U;
        writeReceipt(receipt, envelope);
        const auto original = Detail::ShellJobStorage::load(path(jobs), scope.projectId(), snapshot.jobId);
        require(original.hasValue() && original.value().cmakeTest->reportBytes == 0U && original.value().cmakeTest->reportSha256.empty(),
            "Valid synthetic missing-report receipt did not survive its original codec");
        envelope["payload"]["cmake_test"]["report_bytes"] = 42U;
        writeReceipt(receipt, envelope);
        const auto loaded = Detail::ShellJobStorage::load(path(jobs), scope.projectId(), snapshot.jobId);
        const bool invalid = !loaded && loaded.error().code == Domain::ErrorCodes::IntegrityFailure;
        rejected = rejected && invalid;
        outcomes.push_back({{"state", static_cast<int>(state)}, {"accepted", loaded.hasValue()},
            {"error_code", loaded ? Json(nullptr) : Json(loaded.error().code)}});
    }
    require(rejected, "Unsealed CTest receipts claimed positive report sizes: " + outcomes.dump());
}

void restoredStatusContextTests(const std::filesystem::path& powershell) {
    Workspace workspace;
    const auto jobs = workspace.root / L"jobs";
    const auto scope = authority(path(workspace.root));
    auto supervisor = std::make_shared<LaunchFailureSupervisor>();
    Json outcomes = Json::array();
    bool rejected = true;
    for (const bool malformed : {true, false}) {
        Domain::ShellJobSnapshot snapshot;
        snapshot.jobId = malformed ? "62000000-0000-4000-8000-000000000001" : "62000000-0000-4000-8000-000000000002";
        snapshot.state = Domain::ShellJobState::Running; snapshot.command = "ctest";
        snapshot.cwd = path(workspace.root).value(); snapshot.timeoutSeconds = 30U;
        snapshot.cmakeTest.emplace(); snapshot.cmakeTest->buildDirectory = snapshot.cwd;
        const auto reportPath = jobs / scope.projectId().value() / (snapshot.jobId + ".ctest.xml");
        const auto encoded = reportPath.generic_u8string();
        snapshot.cmakeTest->reportPath = {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
        auto storage = take(Detail::ShellJobStorage::create(path(jobs), scope.projectId(), snapshot, context()));
        storage->persist(snapshot);
        const auto report = std::string{"<testsuite tests=\"1\" failures=\""} + (malformed ? "2" : "1") +
            "\" skipped=\"0\" disabled=\"0\"><testcase name=\"restored\" status=\"fail\"><failure/><system-out>" +
            std::string(Detail::MaximumCTestReportBytes - 1024U, 'x') + "</system-out></testcase></testsuite>";
        write(reportPath, report);
        // Only the phase result is synthetic; the seal, parser and reopened storage path are real.
        snapshot.result = Domain::ProcessResult{}; snapshot.result->exitCode = 1;
        snapshot.cmakeTest->testResult = snapshot.result;
        auto interruptedCapture = context(); interruptedCapture.deadline = std::chrono::steady_clock::now();
        Detail::captureCTestReport(*snapshot.cmakeTest, malformed ? nullptr : &interruptedCapture);
        const auto expectedReportError = malformed ? Domain::ErrorCodes::IntegrityFailure : Domain::ErrorCodes::DeadlineExceeded;
        require(!snapshot.cmakeTest->counts && snapshot.cmakeTest->reportError &&
            snapshot.cmakeTest->reportError->code == expectedReportError &&
            snapshot.cmakeTest->reportBytes == report.size() && snapshot.cmakeTest->reportSha256 == digest(report),
            "Restored status fixture did not record a real sealed counts-null report");
        snapshot.state = malformed ? Domain::ShellJobState::Failed : Domain::ShellJobState::TimedOut;
        snapshot.error = snapshot.cmakeTest->reportError;
        storage->persist(snapshot); storage.reset();

        Native::WindowsShellService reopened{path(powershell), supervisor, path(jobs)};
        const auto baseline = take(reopened.getCMakeTestRun(snapshot.jobId, 0U, 1U, scope, context()));
        require(baseline.job.state == snapshot.state && !baseline.job.cmakeTest->counts &&
            baseline.job.cmakeTest->reportSha256 == snapshot.cmakeTest->reportSha256 &&
            baseline.job.cmakeTest->testResult->exitCode == 1 && baseline.failures.empty(),
            "Reopened counts-null status fixture did not preserve its phase/report disposition");

        auto shortDeadline = context(); shortDeadline.deadline = std::chrono::steady_clock::now() + 1ms;
        const auto deadlineStarted = std::chrono::steady_clock::now();
        require(deadlineStarted < shortDeadline.deadline, "Restored deadline fixture was already expired before the call");
        const auto expiredStatus = reopened.getCMakeTestRun(snapshot.jobId, 0U, 1U, scope, shortDeadline);
        const auto deadlineFinished = std::chrono::steady_clock::now();
        const bool deadlineRejected = !expiredStatus && expiredStatus.error().code == Domain::ErrorCodes::DeadlineExceeded;

        std::stop_source cancellation;
        auto cancelledContext = context(); cancelledContext.cancellation = cancellation.get_token();
        std::atomic<bool> invokeStatus{};
        std::chrono::steady_clock::time_point cancellationRequested;
        std::jthread canceller{[&] {
            while (!invokeStatus.load(std::memory_order_acquire)) std::this_thread::yield();
            std::this_thread::sleep_for(1ms);
            cancellationRequested = std::chrono::steady_clock::now();
            cancellation.request_stop();
        }};
        const auto cancellationStarted = std::chrono::steady_clock::now();
        invokeStatus.store(true, std::memory_order_release);
        const auto cancelledStatus = reopened.getCMakeTestRun(snapshot.jobId, 0U, 1U, scope, cancelledContext);
        const auto cancellationFinished = std::chrono::steady_clock::now();
        canceller.join();
        const bool cancelledRejected = !cancelledStatus && cancelledStatus.error().code == Domain::ErrorCodes::Cancelled;
        const bool deadlineExercised = deadlineFinished >= shortDeadline.deadline;
        const bool cancellationExercised = cancellationRequested > cancellationStarted && cancellationRequested < cancellationFinished;
        rejected = rejected && deadlineRejected && cancelledRejected && deadlineExercised && cancellationExercised;
        outcomes.push_back({{"disposition", malformed ? "malformed" : "parse_interrupted"}, {"report_bytes", report.size()},
            {"deadline_accepted", expiredStatus.hasValue()}, {"deadline_elapsed_us",
                std::chrono::duration_cast<std::chrono::microseconds>(deadlineFinished - deadlineStarted).count()},
            {"deadline_exercised", deadlineExercised}, {"deadline_error", expiredStatus ? Json(nullptr) : Json(expiredStatus.error().code)},
            {"cancellation_accepted", cancelledStatus.hasValue()}, {"cancellation_elapsed_us",
                std::chrono::duration_cast<std::chrono::microseconds>(cancellationFinished - cancellationStarted).count()},
            {"cancellation_exercised", cancellationExercised}, {"cancellation_error", cancelledStatus ? Json(nullptr) : Json(cancelledStatus.error().code)}});
        reopened.shutdown();
    }
    require(supervisor->calls.load() == 0U, "Restored status verification unexpectedly dispatched a process");
    require(rejected, "Reopened counts-null CTest status ignored its active read context: " + outcomes.dump());
    std::cout << "Reopened counts-null CTest status context guards: " << outcomes.dump() << '\n';
}

void integration(const std::filesystem::path& powershell, const std::filesystem::path& cmake,
    std::string_view generator, std::string_view generatorInstance) {
    Workspace workspace;
    const auto source = workspace.root / L"source";
    const auto build = workspace.root / L"build";
    const auto jobs = workspace.root / L"jobs";
    std::filesystem::create_directory(source);
    write(source / L"CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.21)
project(ForgeOwnedCTestFixture NONE)
enable_testing()
add_custom_target(success COMMAND "${CMAKE_COMMAND}" -E true)
add_custom_target(failed_build
    COMMAND "${CMAKE_COMMAND}" -E echo FORGE_CTEST_EXPECTED_FAILED_BUILD_REACHED
    COMMAND "${CMAKE_COMMAND}" -E false)
add_custom_target(slow_build COMMAND "${CMAKE_COMMAND}" -E sleep 10)
add_test(NAME pass COMMAND "${CMAKE_COMMAND}" -E true)
add_test(NAME fail COMMAND "${CMAKE_COMMAND}" -E false)
add_test(NAME skip COMMAND "${CMAKE_COMMAND}" -E false)
set_tests_properties(skip PROPERTIES SKIP_RETURN_CODE 1)
add_test(NAME disabled COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(disabled PROPERTIES DISABLED TRUE)
add_test(NAME sleep COMMAND "${CMAKE_COMMAND}" -E sleep 10)
)");
    const auto scope = authority(path(workspace.root));
    Infrastructure::Windows::SystemClock clock;
    const auto budgets = Domain::budgetsForProfile(Domain::ResourceProfile::Constrained8GiB);
    auto diagnostics = std::make_shared<Infrastructure::Windows::WindowsRuntimeDiagnostics>(clock, budgets);
    auto supervisor = std::make_shared<Infrastructure::Windows::WindowsProcessSupervisor>(budgets, diagnostics);
    Native::WindowsShellService service{path(powershell), supervisor, path(jobs)};
    Domain::ProcessRequest configure{path(cmake)};
    configure.arguments = {"-S", path(source).value(), "-B", path(build).value(), "-G", std::string{generator}};
    if (!generatorInstance.empty()) configure.arguments.push_back("-DCMAKE_GENERATOR_INSTANCE=" + std::string{generatorInstance});
    configure.workingDirectory = path(source); configure.timeout = 30s;
    const auto diagnostic = [](const Domain::ShellJobSnapshot& job) {
        return Json{{"state", static_cast<int>(job.state)}, {"elapsed_ms", job.elapsed.count()},
            {"pid", job.processId}, {"pid_creation_time", job.processCreationTime},
            {"error", job.error ? Json{{"code", job.error->code}, {"message", job.error->message}} : Json(nullptr)},
            {"result", job.result ? Json{{"exit_code", job.result->exitCode}, {"timed_out", job.result->timedOut},
                {"cancelled", job.result->cancelled}, {"termination_confirmed", job.result->terminationConfirmed},
                {"elapsed_ms", job.result->elapsed.count()},
                {"stdout", job.result->stdoutUtf8.substr(0U, 8U * 1024U)}, {"stderr", job.result->stderrUtf8.substr(0U, 4U * 1024U)}} : Json(nullptr)}}.dump();
    };
    wchar_t systemDirectory[MAX_PATH]{};
    const auto systemDirectoryLength = ::GetSystemDirectoryW(systemDirectory, MAX_PATH);
    require(systemDirectoryLength != 0U && systemDirectoryLength < MAX_PATH, "Windows system directory resolution failed");
    Domain::ProcessRequest knownFolder{path(std::filesystem::path{systemDirectory} / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe")};
    knownFolder.arguments = {"-NoLogo", "-NoProfile", "-NonInteractive", "-Command",
        "$ErrorActionPreference = 'Stop'; "
        "if ($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -lt 1) { throw 'Expected Windows PowerShell 5.1'; }; "
        "$common = [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonApplicationData); "
        "if ([string]::IsNullOrWhiteSpace($common) -or -not [IO.Path]::IsPathRooted($common) -or -not [IO.Directory]::Exists($common)) { "
        "throw 'CommonApplicationData is not an existing absolute directory'; }; "
        "[Console]::WriteLine('FORGE_NATIVE_COMMON_APPLICATION_DATA_READY'); exit 0"};
    knownFolder.workingDirectory = path(source); knownFolder.timeout = 15s;
    knownFolder.maximumStdoutBytes = 8U * 1024U; knownFolder.maximumStderrBytes = 4U * 1024U;
    const auto folderChecked = terminal(service, take(service.startProcess(knownFolder, scope, context(6U))).jobId, scope);
    require(folderChecked.state == Domain::ShellJobState::Completed && folderChecked.result && folderChecked.result->exitCode == 0 &&
        folderChecked.result->terminationConfirmed && !folderChecked.result->timedOut && !folderChecked.result->cancelled &&
        (folderChecked.result->stdoutUtf8 == "FORGE_NATIVE_COMMON_APPLICATION_DATA_READY\r\n" ||
            folderChecked.result->stdoutUtf8 == "FORGE_NATIVE_COMMON_APPLICATION_DATA_READY\n"),
        "Native Windows PowerShell known-folder probe did not complete successfully: " + diagnostic(folderChecked));
    std::cout << "Native Windows PowerShell known-folder probe: " << diagnostic(folderChecked) << '\n';
    const auto initialized = terminal(service, take(service.startProcess(configure, scope, context(4U))).jobId, scope);
    const auto configureDiagnostic = diagnostic(initialized);
    require(initialized.state == Domain::ShellJobState::Completed && initialized.result && initialized.result->exitCode == 0,
        "Disposable CMake initialization failed: " + configureDiagnostic);
    Domain::CMakeTestRequest request{path(build)}; request.timeout = 20s; request.configuration = "Debug";
    request.filter = "^pass$";
    const auto passed = run(service, request, scope);
    require(passed.job.state == Domain::ShellJobState::Completed && passed.job.cmakeTest->counts == Domain::CMakeTestCounts{1U,1U,0U,0U,0U} &&
        !passed.job.cmakeTest->buildResult && passed.job.cmakeTest->testResult->exitCode == 0 && !passed.job.cmakeTest->reportSha256.empty(), "Real passing CTest phase was not sealed");
    request.filter = "^(pass|fail|skip|disabled)$";
    const auto mixed = run(service, request, scope);
    require(mixed.job.state == Domain::ShellJobState::Failed && mixed.job.cmakeTest->testResult->exitCode != 0 &&
        mixed.job.cmakeTest->counts == Domain::CMakeTestCounts{4U,1U,1U,1U,1U} && mixed.failures.size() == 1U && mixed.failures[0].name == "fail", "Real failed/skipped/disabled CTest cases differ");
    const auto eof = take(service.getCMakeTestRun(mixed.job.jobId, 1U, 1U, scope, context()));
    require(eof.failures.empty() && !eof.hasMore && eof.totalFailures == 1U, "CTest failure paging EOF differs");
    request.filter = "^nothing-matches$";
    const auto noTests = run(service, request, scope);
    require(noTests.job.state == Domain::ShellJobState::Failed && noTests.job.cmakeTest->testResult &&
        noTests.job.cmakeTest->testResult->exitCode != 0 && (!noTests.job.cmakeTest->counts || noTests.job.cmakeTest->counts->tests == 0U), "Empty CTest selection was reported as success");
    request.filter = "^pass$"; request.build = true; request.target = "success";
    const auto built = run(service, request, scope);
    require(built.job.state == Domain::ShellJobState::Completed && built.job.cmakeTest->buildResult->exitCode == 0 &&
        built.job.cmakeTest->testResult->exitCode == 0, "Two real native phases did not complete in order: " + diagnostic(built.job));
    const auto rebuilt = run(service, request, scope);
    require(rebuilt.job.state == Domain::ShellJobState::Completed && rebuilt.job.cmakeTest->buildResult &&
        rebuilt.job.cmakeTest->testResult && rebuilt.job.cmakeTest->buildResult->exitCode == 0 &&
        rebuilt.job.cmakeTest->buildResult->terminationConfirmed && !rebuilt.job.cmakeTest->buildResult->timedOut &&
        !rebuilt.job.cmakeTest->buildResult->cancelled && rebuilt.job.cmakeTest->testResult->exitCode == 0 &&
        rebuilt.job.cmakeTest->testResult->terminationConfirmed && !rebuilt.job.cmakeTest->testResult->timedOut &&
        !rebuilt.job.cmakeTest->testResult->cancelled &&
        rebuilt.job.cmakeTest->counts == Domain::CMakeTestCounts{1U,1U,0U,0U,0U},
        "Repeated build on the initialized tree did not complete both native phases and actual counts: " + diagnostic(rebuilt.job));
    request.target = "failed_build";
    const auto buildFailed = run(service, request, scope);
    require(buildFailed.job.state == Domain::ShellJobState::Failed && buildFailed.job.cmakeTest->buildResult &&
        buildFailed.job.cmakeTest->buildResult->exitCode != 0 && !buildFailed.job.cmakeTest->testResult && !buildFailed.job.cmakeTest->counts,
        "Failed build invented a CTest phase/result");
    const auto expectedFailureTargetReached = [&] {
        constexpr std::string_view marker{"FORGE_CTEST_EXPECTED_FAILED_BUILD_REACHED"};
        std::istringstream output{buildFailed.job.cmakeTest->buildResult->stdoutUtf8};
        std::string line;
        while (std::getline(output, line)) {
            const auto first = line.find_first_not_of(" \t\r");
            if (first != std::string::npos && std::string_view{line}.substr(first, marker.size()) == marker &&
                    line.find_first_not_of(" \t\r", first + marker.size()) == std::string::npos) return true;
        }
        return false;
    }();
    require(expectedFailureTargetReached,
        "Expected build failure occurred before reaching its target: " + diagnostic(buildFailed.job));
    request.build = false; request.target.reset();
    require(!service.startCMakeTestRun(request, authority(path(workspace.root), false), context()), "CTest bypassed write authority");
    require(!service.startCMakeTestRun(request, authority(path(workspace.root), true, false), context()), "CTest bypassed shell policy");
    auto invalid = request; invalid.target = "success";
    require(!service.startCMakeTestRun(invalid, scope, context()), "Test-only mode accepted a build target");
    invalid = request; invalid.timeout = 3601s;
    require(!service.startCMakeTestRun(invalid, scope, context()), "CTest accepted an excessive lifetime");
    invalid = request; invalid.buildDirectory = path(source);
    require(!service.startCMakeTestRun(invalid, scope, context()), "CTest accepted an uninitialized directory");
    require(!service.getCMakeTestRun(passed.job.jobId, 0U, 0U, scope, context()), "CTest accepted an empty failure page");
    require(!service.getCMakeTestRun(passed.job.jobId, 0U, 1U, authority(path(workspace.root), true, true,
        "20000000-0000-4000-8000-000000000002"), context()), "CTest status crossed project ownership");

    request.filter = "^sleep$"; request.timeout = 20s;
    const auto cancellable = take(service.startCMakeTestRun(request, scope, context()));
    static_cast<void>(take(service.cancelJob(cancellable.jobId, scope, context())));
    const auto cancelled = terminal(service, cancellable.jobId, scope);
    require(cancelled.state == Domain::ShellJobState::Cancelled && !cancelled.cmakeTest->counts, "CTest cancellation claimed a complete report");
    request.timeout = 250ms;
    const auto timed = run(service, request, scope);
    require(timed.job.state == Domain::ShellJobState::TimedOut && !timed.job.cmakeTest->counts, "CTest timeout claimed a complete report");
    request.build = true; request.target = "slow_build"; request.filter = "^pass$";
    const auto timedBuild = run(service, request, scope);
    require(timedBuild.job.state == Domain::ShellJobState::TimedOut && !timedBuild.job.cmakeTest->testResult && !timedBuild.job.cmakeTest->counts,
        "Build timeout launched the test phase or claimed test counts");
    request.timeout = 20s;
    const auto cancellableBuild = take(service.startCMakeTestRun(request, scope, context()));
    static_cast<void>(take(service.cancelJob(cancellableBuild.jobId, scope, context())));
    const auto cancelledBuild = terminal(service, cancellableBuild.jobId, scope);
    require(cancelledBuild.state == Domain::ShellJobState::Cancelled && !cancelledBuild.cmakeTest->testResult && !cancelledBuild.cmakeTest->counts,
        "Build cancellation launched the test phase or claimed test counts");

    const auto log = take(service.readJobLog(built.job.jobId, false, 0U, 0U, scope, context()));
    require(log.totalBytes > 0U && !log.text.empty(), "Sequential phases lost durable output");
    service.shutdown();
    Native::WindowsShellService reopened{path(powershell), supervisor, path(jobs)};
    const auto restored = take(reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()));
    require(restored.job.cmakeTest->counts == mixed.job.cmakeTest->counts && restored.job.cmakeTest->reportSha256 == mixed.job.cmakeTest->reportSha256 &&
        restored.job.cmakeTest->testResult->exitCode == mixed.job.cmakeTest->testResult->exitCode && restored.failures[0].name == "fail", "CTest metadata/report did not survive reconnect");
    const auto reportPath = native(mixed.job.cmakeTest->reportPath); const auto original = read(reportPath);
    write(reportPath, "<testsuite tests=\"0\" failures=\"0\" skipped=\"0\" disabled=\"0\"/>");
    const auto tampered = reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context());
    require(!tampered && tampered.error().code == Domain::ErrorCodes::IntegrityFailure, "CTest silently accepted report replacement");
    write(reportPath, original);
    const auto receiptPath = native(mixed.job.receiptPath); const auto originalReceipt = read(receiptPath);
    auto envelope = Json::parse(originalReceipt);
    envelope["payload"]["cmake_test"]["report_path"] = passed.job.cmakeTest->reportPath;
    writeReceipt(receiptPath, envelope);
    require(!reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()), "CTest trusted a substituted report path in a rehashed receipt");
    write(receiptPath, originalReceipt);
    envelope = Json::parse(originalReceipt);
    envelope["payload"]["cmake_test"]["test_result"]["timed_out"] = true;
    writeReceipt(receiptPath, envelope);
    require(!reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()), "Interrupted typed CTest result retained completed counts");
    write(receiptPath, originalReceipt);
    envelope = Json::parse(originalReceipt);
    envelope["payload"]["cmake_test"]["counts"]["passed"] = 2U;
    writeReceipt(receiptPath, envelope);
    require(!reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()), "Rehashed CTest receipt changed actual report counts");
    write(receiptPath, originalReceipt);
    envelope = Json::parse(originalReceipt); envelope["payload"]["result"]["exit_code"] = 0;
    writeReceipt(receiptPath, envelope);
    require(!reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()), "Outer receipt hid the actual failed test exit code");
    write(receiptPath, originalReceipt);
    envelope = Json::parse(originalReceipt); envelope["payload"]["result"]["timed_out"] = true;
    writeReceipt(receiptPath, envelope);
    require(!reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()), "Outer receipt changed actual phase termination flags");
    write(receiptPath, originalReceipt);
    envelope = Json::parse(originalReceipt); envelope["payload"].erase("result");
    writeReceipt(receiptPath, envelope);
    require(!reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()), "Outer receipt dropped a known actual test phase result");
    write(receiptPath, originalReceipt);
    envelope = Json::parse(originalReceipt); envelope["payload"]["state"] = static_cast<int>(Domain::ShellJobState::Completed);
    writeReceipt(receiptPath, envelope);
    require(!reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()), "Outer receipt claimed completion for failed tests");
    write(receiptPath, originalReceipt);
    const auto invalidOffset = reopened.getCMakeTestRun(mixed.job.jobId, 2U, 1U, scope, context());
    require(!invalidOffset && invalidOffset.error().code == Domain::ErrorCodes::InvalidRequest, "Failure offset past EOF was misreported as corrupt evidence");
    auto rejectedMetadata = *mixed.job.cmakeTest;
    write(reportPath, std::string(Detail::MaximumCTestReportBytes + 1U, 'x'));
    Detail::captureCTestReport(rejectedMetadata);
    envelope = Json::parse(originalReceipt); envelope["payload"]["cmake_test"] = Detail::encodeCMakeTestMetadata(rejectedMetadata);
    writeReceipt(receiptPath, envelope);
    const auto oversizedReplay = take(reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()));
    require(oversizedReplay.job.cmakeTest->reportUnverified && !oversizedReplay.job.cmakeTest->counts &&
        oversizedReplay.job.cmakeTest->reportSha256.empty() && oversizedReplay.job.cmakeTest->testResult->exitCode == mixed.job.cmakeTest->testResult->exitCode,
        "Rejected oversized report hid actual phase facts after reconnect");
    std::filesystem::remove(reportPath); std::filesystem::create_directory(reportPath);
    Detail::captureCTestReport(rejectedMetadata);
    envelope["payload"]["cmake_test"] = Detail::encodeCMakeTestMetadata(rejectedMetadata); writeReceipt(receiptPath, envelope);
    const auto unreadableReplay = take(reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()));
    require(unreadableReplay.job.cmakeTest->reportUnverified && !unreadableReplay.job.cmakeTest->counts && unreadableReplay.job.cmakeTest->testResult,
        "Rejected unreadable report hid actual phase facts after reconnect");
    std::filesystem::remove(reportPath); write(reportPath, original); write(receiptPath, originalReceipt);
    auto parseInterrupted = *mixed.job.cmakeTest; auto expired = context(); expired.deadline = std::chrono::steady_clock::now();
    Detail::captureCTestReport(parseInterrupted, &expired);
    envelope = Json::parse(originalReceipt); envelope["payload"]["cmake_test"] = Detail::encodeCMakeTestMetadata(parseInterrupted);
    envelope["payload"]["state"] = static_cast<int>(Domain::ShellJobState::TimedOut);
    envelope["payload"]["error"] = {{"code", parseInterrupted.reportError->code}, {"message", parseInterrupted.reportError->message}, {"retryable", false}};
    writeReceipt(receiptPath, envelope);
    const auto parseTimeoutReplay = take(reopened.getCMakeTestRun(mixed.job.jobId, 0U, 1U, scope, context()));
    require(parseTimeoutReplay.job.state == Domain::ShellJobState::TimedOut && !parseTimeoutReplay.job.cmakeTest->counts &&
        parseTimeoutReplay.job.cmakeTest->testResult->exitCode == mixed.job.cmakeTest->testResult->exitCode,
        "Report parsing timeout lost the actual failed test phase or claimed counts");
    write(receiptPath, originalReceipt);
    const auto unexpectedReport = native(buildFailed.job.cmakeTest->reportPath);
    write(unexpectedReport, original);
    const auto unexpectedReplay = reopened.getCMakeTestRun(buildFailed.job.jobId, 0U, 1U, scope, context());
    require(!unexpectedReplay && unexpectedReplay.error().code == Domain::ErrorCodes::IntegrityFailure,
        "Unexpected postterminal XML was confused with a recorded rejected capture");
    std::filesystem::remove(unexpectedReport);

    Domain::ShellJobSnapshot interrupted;
    interrupted.jobId = "50000000-0000-4000-8000-000000000001"; interrupted.state = Domain::ShellJobState::Running;
    interrupted.command = path(cmake).value(); interrupted.cwd = path(build).value(); interrupted.timeoutSeconds = 20U;
    interrupted.cmakeTest.emplace(); interrupted.cmakeTest->buildDirectory = interrupted.cwd;
    const auto unsealedReport = jobs / scope.projectId().value() / (interrupted.jobId + ".ctest.xml");
    const auto unsealedEncoded = unsealedReport.generic_u8string();
    interrupted.cmakeTest->reportPath = {reinterpret_cast<const char*>(unsealedEncoded.data()), unsealedEncoded.size()};
    auto storage = take(Detail::ShellJobStorage::create(path(jobs), scope.projectId(), interrupted, context()));
    storage->persist(interrupted); storage.reset();
    write(unsealedReport, original);
    const auto interruptedReceipt = native(interrupted.receiptPath);
    envelope = Json::parse(read(interruptedReceipt));
    envelope["payload"]["job_host_pid"] = 0U; envelope["payload"]["job_host_creation_time"] = 0U;
    writeReceipt(interruptedReceipt, envelope);
    const auto recovered = take(reopened.getCMakeTestRun(interrupted.jobId, 0U, 1U, scope, context()));
    require(recovered.job.state == Domain::ShellJobState::Failed && recovered.job.error &&
        recovered.job.error->code == Domain::ErrorCodes::ProcessTerminationUnconfirmed && !recovered.job.result &&
        !recovered.job.cmakeTest->testResult && !recovered.job.cmakeTest->counts && recovered.job.cmakeTest->reportSha256.empty(),
        "Host interruption inferred an exit code/counts or made the unsealed XML unrecoverable");
    require(take(Detail::ShellJobStorage::list(path(jobs), scope.projectId())).size() >= 2U, "Interrupted CTest report poisoned retained-job listing");
    require(take(reopened.getJob(initialized.jobId, scope, context())).state == Domain::ShellJobState::Completed,
        "New CTest metadata invalidated a legacy-shaped ordinary process receipt");
    reopened.shutdown();
    // Deliberately scripted supervisor error: no owner executable is altered to force a launch failure.
    auto scripted = std::make_shared<LaunchFailureSupervisor>();
    Native::WindowsShellService launchFailureService{path(powershell), scripted, path(jobs)};
    request.build = true; request.target = "success"; request.filter = "^pass$"; request.timeout = 20s;
    const auto launchFailed = run(launchFailureService, request, scope);
    require(scripted->calls.load() == 2U && launchFailed.job.state == Domain::ShellJobState::Failed && launchFailed.job.error &&
        launchFailed.job.error->code == Domain::ErrorCodes::ProcessLaunchFailed && !launchFailed.job.result &&
        launchFailed.job.cmakeTest->buildResult && !launchFailed.job.cmakeTest->testResult && !launchFailed.job.cmakeTest->counts,
        "Second-phase launch failure invented an outer/test result or discarded the completed build phase");
    launchFailureService.shutdown();
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        require(argc == 5, "CMake test service tests require PowerShell/CMake paths and the configured generator/instance");
        parserTests(); reportLifecycleTests(); unsealedReceiptByteTests(); restoredStatusContextTests(argv[1]); integration(argv[1], argv[2],
            take(InfrastructureDetail::strictUtf16ToUtf8(argv[3])), take(InfrastructureDetail::strictUtf16ToUtf8(argv[4])));
        std::cout << "Native CMake/CTest parser, phase, authority and durable receipt tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
