#include "Mcp/McpToolContractTestSupport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Contract = ForgeConductor::Tests::McpContract;
using Json = nlohmann::json;

std::size_t assertions{};

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error{message};
}

void require(const bool condition, const std::string& message)
{
    ++assertions;
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] std::string caseLabel(
    const std::string_view tool,
    const std::string_view category)
{
    return std::string{tool} + "/" + std::string{category};
}

[[nodiscard]] bool containsJson(
    const Json& actual,
    const Json& expected)
{
    if (expected.is_object()) {
        if (!actual.is_object()) {
            return false;
        }
        for (const auto& [key, value] : expected.items()) {
            const auto found = actual.find(key);
            if (found == actual.end() || !containsJson(*found, value)) {
                return false;
            }
        }
        return true;
    }
    if (expected.is_array()) {
        if (!actual.is_array() || actual.size() != expected.size()) {
            return false;
        }
        for (std::size_t index{}; index < expected.size(); ++index) {
            if (!containsJson(actual.at(index), expected.at(index))) {
                return false;
            }
        }
        return true;
    }
    return actual == expected;
}

[[nodiscard]] Contract::DependencyMode dependencyMode(const Json& testCase)
{
    const auto encoded = testCase.value("dependency_mode", "happy");
    if (encoded == "happy") {
        return Contract::DependencyMode::Happy;
    }
    if (encoded == "database_busy") {
        return Contract::DependencyMode::DatabaseBusy;
    }
    if (encoded == "process_nonzero") {
        return Contract::DependencyMode::ProcessNonzero;
    }
    fail("Unknown dependency_mode: " + encoded);
}

[[nodiscard]] Json argumentsFor(
    const Json& cases,
    const Json& testCase,
    const std::string& label)
{
    if (const auto found = testCase.find("arguments");
        found != testCase.end()) {
        return *found;
    }
    const auto source = testCase.value("arguments_from", "");
    require(!source.empty(), label + " does not define arguments");
    const auto inherited = cases.find(source);
    require(
        inherited != cases.end() && inherited->contains("arguments"),
        label + " names an arguments_from case without arguments");
    return inherited->at("arguments");
}

void verifyEffects(
    const Contract::InvocationResult& actual,
    const Json& expectation,
    const std::string& label)
{
    require(
        expectation.contains("effects_exact") &&
            expectation.at("effects_exact").is_array(),
        label + " does not define effects_exact");
    auto expected = expectation.at("effects_exact")
                        .get<std::vector<std::string>>();
    std::sort(expected.begin(), expected.end());
    require(
        actual.effects == expected,
        label + " effects mismatch; expected " + Json(expected).dump() +
            ", actual " + Json(actual.effects).dump());
}

void verifyOutcome(
    const Contract::InvocationResult& actual,
    const Json& expectation,
    const std::string& label)
{
    require(
        actual.hasOutcome,
        label + " expected an outcome; actual error " + actual.errorCode);
    const auto expectedReceipt = expectation.value("receipt_ok", true);
    require(
        actual.receiptOk == expectedReceipt,
        label + " receipt.ok mismatch");
    if (const auto found = expectation.find("receipt_error_code");
        found != expectation.end()) {
        require(
            actual.receiptErrorCode == found->get<std::string>(),
            label + " receipt error mismatch; actual " +
                actual.receiptErrorCode);
    } else {
        require(
            actual.receiptErrorCode.empty(),
            label + " unexpectedly returned receipt error " +
                actual.receiptErrorCode);
    }
    if (const auto found = expectation.find("payload_contains");
        found != expectation.end()) {
        require(
            containsJson(actual.payload, *found),
            label + " payload mismatch; expected subset " + found->dump() +
                ", actual " + actual.payload.dump());
    }
    if (const auto found = expectation.find("payload_absent");
        found != expectation.end()) {
        require(
            found->is_array(),
            label + " payload_absent must be an array");
        require(
            actual.payload.is_object(),
            label + " payload_absent requires an object payload");
        for (const auto& key : *found) {
            require(
                key.is_string(),
                label + " payload_absent entries must be strings");
            require(
                !actual.payload.contains(key.get<std::string>()),
                label + " payload unexpectedly contains " + key.dump());
        }
    }
    if (const auto found = expectation.find("observations_contains");
        found != expectation.end()) {
        require(
            containsJson(actual.observations, *found),
            label + " capture observation mismatch; expected subset " +
                found->dump() + ", actual " + actual.observations.dump());
    }
}

void verifyError(
    const Contract::InvocationResult& actual,
    const Json& expectation,
    const std::string& label)
{
    require(!actual.hasOutcome, label + " expected a top-level error");
    require(
        expectation.contains("error_code"),
        label + " does not define error_code");
    require(
        actual.errorCode == expectation.at("error_code").get<std::string>(),
        label + " error code mismatch; actual " + actual.errorCode);
    require(
        actual.errorRetryable == expectation.value("retryable", false),
        label + " retryability mismatch");
}

void runCase(
    const std::string& toolName,
    const std::string& category,
    const Json& cases,
    const Json& testCase)
{
    const auto label = caseLabel(toolName, category);
    require(
        testCase.is_object() && testCase.contains("expect"),
        label + " is not a complete case object");
    Contract::McpToolContractFixture fixture{dependencyMode(testCase)};
    const auto actual = fixture.invoke(
        toolName, argumentsFor(cases, testCase, label));
    const auto& expectation = testCase.at("expect");
    const auto kind = expectation.value("kind", "outcome");
    if (kind == "outcome") {
        verifyOutcome(actual, expectation, label);
    } else if (kind == "error") {
        verifyError(actual, expectation, label);
    } else {
        fail(label + " has unknown expectation kind " + kind);
    }
    verifyEffects(actual, expectation, label);
}

[[nodiscard]] Json resolveCase(
    const Json& defaults,
    const Json& cases,
    const std::string& category)
{
    const auto overlay = [](Json base, const Json& replacement, const auto& self)
        -> Json {
        if (!base.is_object() || !replacement.is_object()) {
            return replacement;
        }
        for (const auto& [key, value] : replacement.items()) {
            const auto found = base.find(key);
            if (found != base.end() && found->is_object() && value.is_object()) {
                *found = self(std::move(*found), value, self);
            } else {
                base[key] = value;
            }
        }
        return base;
    };

    Json result = overlay(
        defaults.value(category, Json::object()),
        cases.at(category),
        overlay);
    const auto expectationSource = result.value("expect_from", "");
    if (!expectationSource.empty()) {
        const auto source = overlay(
            defaults.value(expectationSource, Json::object()),
            cases.at(expectationSource),
            overlay);
        result["expect"] = overlay(
            source.value("expect", Json::object()),
            result.value("expect", Json::object()),
            overlay);
    }
    return result;
}

[[nodiscard]] Json readFixture(const std::string& path)
{
    std::ifstream stream{path, std::ios::binary};
    require(stream.good(), "Unable to open MCP contract fixture: " + path);
    Json fixture;
    stream >> fixture;
    return fixture;
}

void validateLineage(const Json& fixture)
{
    require(fixture.contains("lineage"), "Fixture lineage is missing");
    const auto& lineage = fixture.at("lineage");
    const auto& mac = lineage.at("canonical_macos");
    require(
        mac.at("tool_count") == 53U,
        "Canonical macOS lineage must bind all 53 tools");
    require(
        mac.at("sources").is_array() && !mac.at("sources").empty(),
        "Canonical macOS source paths are missing");
    const auto& extensions = lineage.at("windows_host_extensions");
    require(
        extensions.contains("fs_read_byte_pagination"),
        "The bounded fs_read byte-pagination extension is not recorded");
    require(
        extensions.contains("continuity_request_rollover_host_capability"),
        "The request_rollover host capability is not recorded");
}

void runMatrix(const Json& fixture)
{
    require(
        fixture.value("schema_version", 0U) == 1U,
        "Unexpected MCP contract fixture schema version");
    require(
        fixture.value("expected_tool_count", 0U) == 53U,
        "The fixture must require exactly 53 tools");
    validateLineage(fixture);
    const auto& tools = fixture.at("tools");
    const auto& caseDefaults = fixture.at("case_defaults");
    require(tools.is_array(), "Fixture tools must be an array");
    require(tools.size() == 53U, "Fixture must contain exactly 53 tool rows");

    Contract::McpToolContractFixture catalogFixture{
        Contract::DependencyMode::Happy};
    auto catalogNames = catalogFixture.catalogToolNames();
    std::sort(catalogNames.begin(), catalogNames.end());

    std::vector<std::string> fixtureNames;
    fixtureNames.reserve(tools.size());
    const std::set<std::string> expectedCategories{
        "boundary", "dependency_error", "invalid", "valid"};
    std::size_t executedCases{};
    for (const auto& tool : tools) {
        const auto name = tool.at("name").get<std::string>();
        fixtureNames.push_back(name);
        const auto& cases = tool.at("cases");
        require(cases.is_object(), name + " cases must be an object");
        std::set<std::string> categories;
        for (const auto& [category, testCase] : cases.items()) {
            categories.insert(category);
            runCase(
                name,
                category,
                cases,
                resolveCase(caseDefaults, cases, category));
            ++executedCases;
        }
        require(
            categories == expectedCategories,
            name + " must define valid, invalid, dependency_error, and boundary exactly");
    }
    std::sort(fixtureNames.begin(), fixtureNames.end());
    require(
        std::adjacent_find(fixtureNames.begin(), fixtureNames.end()) ==
            fixtureNames.end(),
        "Fixture tool names must be unique");
    require(
        fixtureNames == catalogNames,
        "Fixture names do not exactly match the real MCP catalog");
    require(executedCases == 212U, "The matrix did not execute 212 cases");
}

} // namespace

int main(const int argc, const char* const argv[])
{
    try {
        require(argc == 2, "Expected one fixture path argument");
        runMatrix(readFixture(argv[1]));
        std::cout << "MCP tool contract matrix passed " << assertions
                  << " assertions across 53 tools and 212 cases.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
