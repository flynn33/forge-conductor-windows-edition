#include "CMakeTestSupport.h"
#include "NativeToolValidation.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <Windows.h>
#include <objbase.h>
#include <xmllite.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ForgeConductor::NativeTools::Windows::Detail {
namespace {
using Json = nlohmann::json;
namespace Utf = Infrastructure::Windows::Detail;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error{message};
}
std::filesystem::path native(std::string_view text) {
    return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
void ordinary(const std::filesystem::path& path, bool directory) {
    for (auto current = path; !current.empty(); current = current.parent_path()) {
        const auto attributes = ::GetFileAttributesW(current.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U,
            "CTest paths must exist without reparse points.");
        if (current == path) require(((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) == directory,
            "CTest path has the wrong file type.");
        if (current == current.parent_path()) break;
    }
}
std::string bytes(const std::filesystem::path& path) {
    ordinary(path, false);
    const auto size = std::filesystem::file_size(path);
    require(size <= MaximumCTestReportBytes, "CTest JUnit report exceeds 8 MiB.");
    std::ifstream input{path, std::ios::binary};
    require(static_cast<bool>(input), "CTest JUnit report cannot be opened.");
    std::string result(static_cast<std::size_t>(size), '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    require(input.gcount() == static_cast<std::streamsize>(result.size()), "CTest JUnit report was not read completely.");
    return result;
}
std::string digest(std::string_view text) {
    Infrastructure::Windows::BCryptSha256Hasher hasher;
    auto value = hasher.sha256(std::as_bytes(std::span{text.data(), text.size()}));
    if (!value) throw std::runtime_error{value.error().message};
    return value.value().value();
}
std::string text(IXmlReader* reader, bool name = false) {
    const wchar_t* value{}; UINT length{};
    require(SUCCEEDED(name ? reader->GetLocalName(&value, &length) : reader->GetValue(&value, &length)),
        "CTest XML text could not be read.");
    auto encoded = Utf::strictUtf16ToUtf8(std::wstring_view{value, length});
    if (!encoded) throw std::runtime_error{encoded.error().message};
    return std::move(encoded).value();
}
std::map<std::string, std::string> attributes(IXmlReader* reader) {
    std::map<std::string, std::string> result;
    auto status = reader->MoveToFirstAttribute();
    if (status == S_FALSE) return result;
    require(status == S_OK, "CTest XML attributes could not be read.");
    do {
        require(result.size() < 32U, "CTest XML has too many attributes.");
        const wchar_t* namespaceUri{}; UINT namespaceLength{};
        require(SUCCEEDED(reader->GetNamespaceUri(&namespaceUri, &namespaceLength)) && namespaceLength == 0U,
            "CTest JUnit attributes must have the producer's empty namespace.");
        const auto key = text(reader, true);
        require(result.emplace(key, text(reader)).second, "CTest XML repeats an attribute.");
        status = reader->MoveToNextAttribute();
    } while (status == S_OK);
    require(status == S_FALSE && reader->MoveToElement() == S_OK, "CTest XML attribute traversal failed.");
    return result;
}
std::uint64_t count(const std::map<std::string, std::string>& values, const char* key) {
    const auto found = values.find(key);
    require(found != values.end(), "CTest JUnit suite is missing a count.");
    std::uint64_t value{};
    const auto& encoded = found->second;
    const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == encoded.data() + encoded.size() && value <= 100'000U,
        "CTest JUnit count is invalid or exceeds 100000 cases.");
    return value;
}
std::string bounded(std::string value, std::size_t maximum, bool* truncated = nullptr) {
    const auto changed = value.size() > maximum;
    if (changed) {
        while (maximum && (static_cast<unsigned char>(value[maximum]) & 0xc0U) == 0x80U) --maximum;
        value.resize(maximum);
    }
    if (truncated) *truncated = *truncated || changed;
    return value;
}
Json process(const Domain::ProcessResult& value) {
    return Json{{"exit_code", value.exitCode}, {"stdout", value.stdoutUtf8}, {"stderr", value.stderrUtf8},
        {"timed_out", value.timedOut}, {"cancelled", value.cancelled},
        {"stdout_truncated", value.stdoutTruncated}, {"stderr_truncated", value.stderrTruncated},
        {"termination_confirmed", value.terminationConfirmed}, {"elapsed_ms", value.elapsed.count()}};
}
Domain::ProcessResult process(const Json& value) {
    Domain::ProcessResult result{value.at("exit_code").get<std::int32_t>(), value.at("stdout").get<std::string>(),
        value.at("stderr").get<std::string>(), value.at("timed_out").get<bool>(), value.at("cancelled").get<bool>(),
        value.at("stdout_truncated").get<bool>(), value.at("stderr_truncated").get<bool>(),
        value.at("termination_confirmed").get<bool>(), std::chrono::milliseconds{value.at("elapsed_ms").get<std::int64_t>()}};
    require(result.stdoutUtf8.size() <= 16U * 1024U && result.stderrUtf8.size() <= 4U * 1024U && result.elapsed.count() >= 0,
        "CTest phase receipt exceeds its capture bounds.");
    return result;
}
Json counts(const Domain::CMakeTestCounts& value) {
    return Json{{"tests", value.tests}, {"passed", value.passed}, {"failed", value.failed},
        {"skipped", value.skipped}, {"disabled", value.disabled}};
}
std::optional<std::string> optionalText(const Json& value, const char* key) {
    return value.at(key).is_null() ? std::nullopt : std::optional{value.at(key).get<std::string>()};
}
bool sameProcessResult(const Domain::ProcessResult& left, const Domain::ProcessResult& right) {
    return left.exitCode == right.exitCode && left.stdoutUtf8 == right.stdoutUtf8 && left.stderrUtf8 == right.stderrUtf8 &&
        left.timedOut == right.timedOut && left.cancelled == right.cancelled && left.stdoutTruncated == right.stdoutTruncated &&
        left.stderrTruncated == right.stderrTruncated && left.terminationConfirmed == right.terminationConfirmed && left.elapsed == right.elapsed;
}
} // namespace

Domain::Result<void> validateCMakeTestRequest(const Domain::CMakeTestRequest& request,
    const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& context) noexcept {
    try {
        auto active = checkContext(context, "CMake/CTest admission");
        if (!active) return active;
        if (!authority.shellEnabled()) return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::ShellDisabled, "CMake/CTest execution requires enabled shell policy."));
        for (auto access : {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Execute})
            if (!containsAccess(authority.grants(), access) || containsAccess(authority.denials(), access))
                return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized,
                    "CMake/CTest requires read, write and execute authority."));
        auto cwd = validateWorkingDirectory(request.buildDirectory, authority);
        if (!cwd) return cwd;
        if (request.timeout < std::chrono::milliseconds{1} || request.timeout > std::chrono::hours{1})
            return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                "CMake/CTest timeout must be positive and at most 3600 seconds."));
        if (!request.build && request.target) return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "A build target requires build_and_test mode."));
        for (const auto* option : {&request.target, &request.filter, &request.configuration}) {
            if (*option && ((*option)->empty() || (*option)->size() > 1024U || !Domain::isValidUtf8(**option) ||
                    (*option)->find_first_of("\r\n\0", 0U, 3U) != std::string::npos))
                return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                    "CMake/CTest options must be nonempty UTF-8 strings of at most 1024 bytes without control delimiters."));
        }
        const auto directory = native(request.buildDirectory.value());
        ordinary(directory, true);
        ordinary(directory / L"CMakeCache.txt", false);
        ordinary(directory / L"CTestTestfile.cmake", false);
        return Domain::Result<void>::success();
    } catch (const std::exception& error) {
        return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, error.what()));
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Invalid initialized CMake test directory."));
    }
}

Domain::Result<void> validateFreshCTestReport(const Domain::CMakeTestMetadata& metadata) noexcept {
    try {
        const auto report = native(metadata.reportPath);
        ordinary(report.parent_path(), true);
        require(!std::filesystem::exists(report), "CTest report is no longer a fresh path owned by this run.");
        return Domain::Result<void>::success();
    } catch (const std::exception& error) {
        return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure, error.what()));
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure, "CTest report destination cannot be verified."));
    }
}

Domain::Result<CTestReportPage> parseCTestJUnit(std::string_view xml, std::uint64_t offset,
    std::size_t limit, const Domain::OperationContext* context) noexcept {
    using Outcome = Domain::Result<CTestReportPage>;
    try {
        if (context) {
            auto active = checkContext(*context, "CTest report parsing");
            if (!active) return Outcome::failure(std::move(active).error());
        }
        require(xml.size() <= MaximumCTestReportBytes && !xml.empty(), "CTest XML is empty or oversized.");
        require(limit <= MaximumCTestFailuresPerPage && Domain::isValidUtf8(xml) && xml.find('\0') == std::string_view::npos,
            "CTest XML/page is not bounded NUL-free UTF-8.");
        Microsoft::WRL::ComPtr<IStream> stream;
        require(SUCCEEDED(::CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf())), "CTest XML stream failed.");
        ULONG written{};
        require(SUCCEEDED(stream->Write(xml.data(), static_cast<ULONG>(xml.size()), &written)) && written == xml.size(), "CTest XML stream write failed.");
        LARGE_INTEGER beginning{};
        require(SUCCEEDED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr)), "CTest XML stream seek failed.");
        Microsoft::WRL::ComPtr<IXmlReader> reader;
        require(SUCCEEDED(::CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)), "CTest XML reader failed.");
        Microsoft::WRL::ComPtr<IXmlReaderInput> input;
        require(SUCCEEDED(::CreateXmlReaderInputWithEncodingName(stream.Get(), nullptr, L"UTF-8", FALSE, nullptr, input.GetAddressOf())),
            "CTest UTF-8 XML input failed.");
        require(SUCCEEDED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) &&
            SUCCEEDED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 16U)) &&
            SUCCEEDED(reader->SetInput(input.Get())), "CTest XML safety policy failed.");
        CTestReportPage result;
        Domain::CMakeTestCounts declared;
        bool root{}, closed{}, failureChild{}, skippedChild{}, propertiesChild{}, outputChild{}, pageFull{};
        std::size_t pageBytes{2U};
        std::string nestedParent;
        std::optional<Domain::CMakeTestFailure> current;
        std::string outputElement;
        const auto finish = [&] {
            require(current.has_value(), "CTest XML testcase closure is invalid.");
            auto& item = *current;
            ++result.counts.tests;
            require(result.counts.tests <= 100'000U, "CTest XML has too many cases.");
            if (item.status == "run") {
                require(!failureChild && !skippedChild, "CTest passing case has a failure/skip element.");
                ++result.counts.passed;
            } else if (item.status == "disabled") {
                require(!failureChild && !skippedChild, "CTest disabled case has an inconsistent element.");
                ++result.counts.disabled;
            } else if (item.status == "notrun") {
                require(skippedChild && !failureChild, "CTest skipped case lacks its skip element.");
                ++result.counts.skipped;
            } else {
                require(item.status == "fail" && failureChild && !skippedChild, "CTest failure lacks its failure element.");
                const auto position = result.counts.failed++;
                if (position >= offset && !pageFull && result.failures.size() < limit) {
                    const auto encodedBytes = Json{{"name", item.name}, {"status", item.status}, {"message", item.message},
                        {"output", item.output}, {"output_truncated", item.outputTruncated}}.dump().size() + 1U;
                    if (pageBytes + encodedBytes <= 24U * 1024U) {
                        pageBytes += encodedBytes; result.failures.push_back(std::move(item));
                    } else pageFull = true;
                }
            }
            current.reset(); outputElement.clear(); failureChild = skippedChild = propertiesChild = outputChild = false;
        };
        XmlNodeType node{}; HRESULT status{}; std::size_t events{};
        while ((status = reader->Read(&node)) == S_OK) {
            if (++events % 128U == 0U && context) {
                auto active = checkContext(*context, "CTest report parsing");
                if (!active) return Outcome::failure(std::move(active).error());
            }
            UINT depth{}; require(SUCCEEDED(reader->GetDepth(&depth)), "CTest XML depth failed.");
            if (node == XmlNodeType_Element) {
                const auto name = text(reader.Get(), true);
                const wchar_t* namespaceUri{}; UINT namespaceLength{};
                require(SUCCEEDED(reader->GetNamespaceUri(&namespaceUri, &namespaceLength)) && namespaceLength == 0U,
                    "CTest JUnit elements must have the producer's empty namespace.");
                const auto values = attributes(reader.Get());
                if (depth == 0U) {
                    require(!root && !closed && name == "testsuite", "CTest report requires one testsuite root.");
                    root = true;
                    declared.tests = count(values, "tests"); declared.failed = count(values, "failures");
                    declared.skipped = count(values, "skipped"); declared.disabled = count(values, "disabled");
                    if (reader->IsEmptyElement()) closed = true;
                } else if (depth == 1U) {
                    require(root && !closed && !current && name == "testcase", "CTest report has an unexpected suite child.");
                    current.emplace();
                    current->name = values.at("name"); current->status = values.at("status");
                    require(!current->name.empty() && current->name.size() <= 1024U, "CTest case name exceeds its bound.");
                    require(current->status == "run" || current->status == "fail" || current->status == "notrun" || current->status == "disabled",
                        "CTest case status is unknown.");
                    if (reader->IsEmptyElement()) finish();
                } else if (depth == 2U && current) {
                    outputElement.clear();
                    nestedParent = name;
                    if (name == "failure" || name == "skipped") {
                        require(!(name == "failure" ? failureChild : skippedChild), "CTest XML repeats a result element.");
                        (name == "failure" ? failureChild : skippedChild) = true;
                        current->message = bounded(values.contains("message") ? values.at("message") : "", 1024U);
                    } else if (name == "system-out") {
                        require(!outputChild, "CTest XML repeats its output element."); outputChild = true; outputElement = name;
                    } else {
                        require(name == "properties" && !propertiesChild, "CTest XML testcase child is unsupported or repeated.");
                        propertiesChild = true;
                    }
                    if (reader->IsEmptyElement()) { outputElement.clear(); nestedParent.clear(); }
                } else require(current.has_value() && depth == 3U && nestedParent == "properties" && name == "property" && reader->IsEmptyElement(),
                    "CTest XML has an unsupported nested element.");
            } else if (node == XmlNodeType_EndElement) {
                // XmlLite reports depth before popping an end element.
                require(depth > 0U, "CTest XML end depth is invalid."); --depth;
                if (depth == 1U) finish();
                if (depth == 2U) { outputElement.clear(); nestedParent.clear(); }
                if (depth == 0U) { require(root && !current, "CTest report root closure is invalid."); closed = true; }
            } else if (node == XmlNodeType_Text || node == XmlNodeType_CDATA || node == XmlNodeType_Whitespace) {
                auto value = text(reader.Get());
                if (current && !outputElement.empty()) current->output = bounded(current->output + value, 2048U, &current->outputTruncated);
                else require(value.find_first_not_of(" \t\r\n") == std::string::npos, "CTest XML has unexpected mixed text.");
            }
        }
        require(status == S_FALSE && root && closed && !current && result.counts.tests == declared.tests &&
            result.counts.failed == declared.failed && result.counts.skipped == declared.skipped && result.counts.disabled == declared.disabled,
            "CTest JUnit report is malformed or its counts disagree with cases.");
        require(offset <= result.counts.failed, "CTest failure offset exceeds the report.");
        if (context) {
            auto active = checkContext(*context, "CTest report parsing");
            if (!active) return Outcome::failure(std::move(active).error());
        }
        return Outcome::success(std::move(result));
    } catch (const std::exception& error) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure, error.what()));
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure, "CTest JUnit report could not be parsed."));
    }
}

void captureCTestReport(Domain::CMakeTestMetadata& metadata, const Domain::OperationContext* context) {
    metadata.counts.reset(); metadata.reportError.reset(); metadata.reportSha256.clear(); metadata.reportBytes = 0U;
    metadata.reportUnverified = false;
    try {
        const bool completed = metadata.testResult && !metadata.testResult->timedOut && !metadata.testResult->cancelled && metadata.testResult->terminationConfirmed;
        if (!std::filesystem::exists(native(metadata.reportPath))) {
            metadata.reportError = Domain::makeError(Domain::ErrorCodes::RecordNotFound,
                completed ? "CTest did not produce a JUnit report." : "CTest did not finish; no completed test report is claimed.");
            return;
        }
        const auto raw = bytes(native(metadata.reportPath));
        metadata.reportSha256 = digest(raw); metadata.reportBytes = raw.size();
        if (!completed) {
            metadata.reportError = Domain::makeError(Domain::ErrorCodes::ProcessTerminationUnconfirmed,
                "An interrupted CTest report is retained without claiming complete counts.");
            return;
        }
        auto parsed = parseCTestJUnit(raw, 0U, 0U, context);
        if (parsed && context) {
            auto active = checkContext(*context, "CTest report capture");
            if (!active) { metadata.reportError = std::move(active).error(); return; }
        }
        if (parsed) metadata.counts = parsed.value().counts;
        else metadata.reportError = std::move(parsed).error();
    } catch (const std::exception& error) {
        metadata.reportError = Domain::makeError(Domain::ErrorCodes::IntegrityFailure, error.what());
        metadata.reportUnverified = metadata.reportSha256.empty();
    }
}

Domain::Result<CTestReportPage> readCTestReport(const Domain::CMakeTestMetadata& metadata, std::uint64_t offset,
    std::size_t limit, const Domain::OperationContext* context) noexcept {
    try {
        const auto raw = bytes(native(metadata.reportPath));
        require(!metadata.reportSha256.empty() && raw.size() == metadata.reportBytes && digest(raw) == metadata.reportSha256,
            "CTest report identity changed after completion.");
        auto result = parseCTestJUnit(raw, offset, limit, context);
        if (result && (!metadata.counts || result.value().counts != *metadata.counts))
            return Domain::Result<CTestReportPage>::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure, "CTest report summary changed."));
        return result;
    } catch (const std::exception& error) {
        return Domain::Result<CTestReportPage>::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure, error.what()));
    } catch (...) {
        return Domain::Result<CTestReportPage>::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure, "CTest report could not be verified."));
    }
}

Json encodeCMakeTestMetadata(const Domain::CMakeTestMetadata& value) {
    Json result{{"schema_version", 1U}, {"build_dir", value.buildDirectory}, {"build_requested", value.buildRequested},
        {"target", value.target ? Json(*value.target) : Json(nullptr)}, {"filter", value.filter ? Json(*value.filter) : Json(nullptr)},
        {"configuration", value.configuration ? Json(*value.configuration) : Json(nullptr)},
        {"report_path", value.reportPath}, {"report_sha256", value.reportSha256}, {"report_bytes", value.reportBytes},
        {"build_result", value.buildResult ? process(*value.buildResult) : Json(nullptr)},
        {"test_result", value.testResult ? process(*value.testResult) : Json(nullptr)},
        {"counts", value.counts ? counts(*value.counts) : Json(nullptr)}, {"report_error", nullptr}, {"report_unverified", value.reportUnverified}};
    if (value.reportError) result["report_error"] = Json{{"code", value.reportError->code},
        {"message", value.reportError->message}, {"retryable", value.reportError->retryable}};
    return result;
}
Domain::CMakeTestMetadata decodeCMakeTestMetadata(const Json& value) {
    require(value.at("schema_version") == 1U, "Unsupported CTest metadata version.");
    Domain::CMakeTestMetadata result;
    result.buildDirectory = value.at("build_dir").get<std::string>(); result.buildRequested = value.at("build_requested").get<bool>();
    result.target = optionalText(value, "target"); result.filter = optionalText(value, "filter"); result.configuration = optionalText(value, "configuration");
    result.reportPath = value.at("report_path").get<std::string>(); result.reportSha256 = value.at("report_sha256").get<std::string>();
    result.reportBytes = value.at("report_bytes").get<std::uint64_t>();
    result.reportUnverified = value.value("report_unverified", false);
    if (!value.at("build_result").is_null()) result.buildResult = process(value.at("build_result"));
    if (!value.at("test_result").is_null()) result.testResult = process(value.at("test_result"));
    if (!value.at("counts").is_null()) {
        const auto& record = value.at("counts");
        result.counts = Domain::CMakeTestCounts{record.at("tests").get<std::uint64_t>(), record.at("passed").get<std::uint64_t>(),
            record.at("failed").get<std::uint64_t>(), record.at("skipped").get<std::uint64_t>(), record.at("disabled").get<std::uint64_t>()};
    }
    if (!value.at("report_error").is_null()) { const auto& error = value.at("report_error");
        result.reportError = Domain::makeError(error.at("code").get<std::string>(), error.at("message").get<std::string>(), error.at("retryable").get<bool>()); }
    return result;
}
void validateCTestReceipt(const Domain::ShellJobSnapshot& snapshot, const std::filesystem::path& directory) {
    if (!snapshot.cmakeTest) return;
    const auto& metadata = *snapshot.cmakeTest;
    require(!metadata.reportSha256.empty() || metadata.reportBytes == 0U, "Unsealed CTest receipt claims a report size.");
    require(metadata.buildDirectory == snapshot.cwd && metadata.reportPath == utf8(directory / (snapshot.jobId + ".ctest.xml")),
        "CTest receipt report path or build-directory ownership differs.");
    require(!metadata.target || metadata.buildRequested, "CTest receipt target has no build phase.");
    require(!metadata.buildResult || metadata.buildRequested, "CTest receipt has an unrequested build phase.");
    require(!metadata.testResult || !metadata.buildRequested || (metadata.buildResult && metadata.buildResult->exitCode == 0 &&
        metadata.buildResult->terminationConfirmed && !metadata.buildResult->timedOut && !metadata.buildResult->cancelled),
        "CTest receipt started tests after an unsuccessful build.");
    require(!metadata.counts || (metadata.testResult && metadata.testResult->terminationConfirmed &&
        !metadata.testResult->timedOut && !metadata.testResult->cancelled && !metadata.reportSha256.empty() && !metadata.reportError),
        "CTest counts have no completed report provenance.");
    require(!metadata.reportUnverified || (!metadata.counts && metadata.reportSha256.empty() && metadata.reportBytes == 0U &&
        metadata.reportError && metadata.reportError->code == Domain::ErrorCodes::IntegrityFailure), "Unverified CTest report disposition claims sealed facts.");
    if (snapshot.state == Domain::ShellJobState::Running) {
        require(!snapshot.result && !metadata.buildResult && !metadata.testResult && !metadata.counts && metadata.reportSha256.empty() &&
            !metadata.reportError && !metadata.reportUnverified, "Running CTest receipt claims unpublished final facts.");
        return;
    }
    const auto* last = metadata.testResult ? &*metadata.testResult : metadata.buildResult ? &*metadata.buildResult : nullptr;
    if (snapshot.result) {
        require(last && sameProcessResult(*snapshot.result, *last), "CTest outer result differs from the actual final phase.");
        auto expected = last->cancelled ? Domain::ShellJobState::Cancelled : last->timedOut ? Domain::ShellJobState::TimedOut
            : last->exitCode == 0 && last->terminationConfirmed ? Domain::ShellJobState::Completed : Domain::ShellJobState::Failed;
        if (metadata.reportError) {
            if (metadata.reportError->code == Domain::ErrorCodes::Cancelled) expected = Domain::ShellJobState::Cancelled;
            else if (metadata.reportError->code == Domain::ErrorCodes::DeadlineExceeded) expected = Domain::ShellJobState::TimedOut;
            else if (expected == Domain::ShellJobState::Completed) expected = Domain::ShellJobState::Failed;
        }
        require(snapshot.state == expected, "CTest outer state differs from its phase/report disposition.");
    } else {
        require(!metadata.testResult && (!metadata.buildResult || (metadata.buildResult->exitCode == 0 && metadata.buildResult->terminationConfirmed &&
            !metadata.buildResult->timedOut && !metadata.buildResult->cancelled)), "CTest final phase result disappeared from its outer receipt.");
        require(snapshot.error && snapshot.state != Domain::ShellJobState::Completed && !metadata.counts,
            "CTest launch/interruption failure claims completion or lacks its actual error.");
    }
    require(snapshot.state != Domain::ShellJobState::Completed || (metadata.testResult && metadata.counts && !metadata.reportError),
        "Completed CTest receipt lacks completed test/report facts.");
    if (metadata.reportSha256.empty()) {
        // A rejected capture has no trusted report data to replay; phase facts remain usable.
        if (metadata.reportUnverified) return;
        require(!metadata.counts && !std::filesystem::exists(native(metadata.reportPath)), "An unsealed CTest report appeared after completion.");
        return;
    }
    const auto raw = bytes(native(metadata.reportPath));
    require(raw.size() == metadata.reportBytes && digest(raw) == metadata.reportSha256, "CTest report hash mismatch.");
    auto parsed = parseCTestJUnit(raw, 0U, 0U);
    require(metadata.counts ? (parsed && parsed.value().counts == *metadata.counts) :
        (metadata.reportError && (!parsed || !metadata.testResult || metadata.testResult->timedOut || metadata.testResult->cancelled || !metadata.testResult->terminationConfirmed ||
            metadata.reportError->code == Domain::ErrorCodes::Cancelled || metadata.reportError->code == Domain::ErrorCodes::DeadlineExceeded)),
        "CTest report counts or malformed-report disposition differs from the sealed receipt.");
}
} // namespace ForgeConductor::NativeTools::Windows::Detail
