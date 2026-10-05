#include "TestSupport.h"

#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::LMStudioConfigurationCodec;
using Infrastructure::Windows::LMStudioConfigurationDocument;
using Infrastructure::Windows::LMStudioCluServerId;
using Infrastructure::Windows::LMStudioFallbackServerId;
using Infrastructure::Windows::LMStudioPrimaryServerId;
using Json = nlohmann::json;

static_assert(std::is_final_v<LMStudioConfigurationCodec>);

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
{
    std::vector<std::byte> encoded(value.size());
    if (!value.empty()) {
        std::memcpy(encoded.data(), value.data(), value.size());
    }
    return encoded;
}

[[nodiscard]] std::string text(const std::vector<std::byte>& value)
{
    std::string decoded(value.size(), '\0');
    if (!value.empty()) {
        std::memcpy(decoded.data(), value.data(), value.size());
    }
    return decoded;
}

[[nodiscard]] Domain::DeploymentId revision(const std::string_view value)
{
    return parse<Domain::DeploymentId>(value);
}

[[nodiscard]] LMStudioConfigurationDocument merged(
    const Domain::DeploymentId& deploymentId)
{
    const auto original = bytes(R"({
  "foreignRoot": {"retained": true},
    "mcpServers": {
    "keep-me": {"command": "C:\\foreign.exe", "args": [], "foreign": 17},
    "forge-serve": {"command": "C:\\Forge\\home\\bin\\forge-serve.cmd", "args": []},
    "foreign-prefix-collision": {"command": "C:\\Vendor\\forge-serve-custom.cmd", "args": []},
    "forge-serve-fallback": {"command": "C:\\Vendor\\forge-serve-fallback.cmd", "args": []},
    "forge-conductor": {
      "command": "C:\\old.exe",
      "args": ["old"],
      "env": {"FOREIGN_ENV": "keep", "FORGE_MCP_ROLE": "wrong"},
      "unknownForgeField": {"keep": true}
    }
  }
})");
    const auto document = take(LMStudioConfigurationCodec::parse(original));
    const auto encoded = take(LMStudioConfigurationCodec::mergeForgeServers(
        document,
        path("C:\\Forge\\forge-conductor.exe"),
        path("C:\\Forge\\home"),
        deploymentId));
    return take(LMStudioConfigurationCodec::parse(encoded));
}

void testMergePreservesForeignAndUnknownFields()
{
    const auto deploymentId = revision("p15-revision-1");
    const auto document = merged(deploymentId);
    const auto root = Json::parse(document.sourceUtf8());
    require(root.at("foreignRoot").at("retained").get<bool>(),
            "The codec removed an unknown root field.");
    const auto& servers = root.at("mcpServers");
    require(servers.at("keep-me").at("foreign").get<int>() == 17,
            "The codec changed a foreign server entry.");
    require(!servers.contains("forge-serve"),
            "An exact Forge-owned legacy launcher registration survived a successful merge.");
    require(servers.contains("foreign-prefix-collision") &&
                servers.contains("forge-serve-fallback"),
            "The codec removed a foreign prefix collision or an unowned reserved-looking entry.");
    const auto& primary = servers.at(LMStudioPrimaryServerId);
    require(primary.at("unknownForgeField").at("keep").get<bool>(),
            "The codec removed an unknown field from an existing Forge server entry.");
    require(primary.at("env").at("FOREIGN_ENV").get<std::string>() == "keep",
            "The codec removed an unknown environment field from a Forge entry.");
    for (const auto* const id : {
             LMStudioPrimaryServerId, LMStudioFallbackServerId, LMStudioCluServerId}) {
        const auto& entry = servers.at(id);
        require(entry.at("command").get<std::string>() ==
                    "C:\\Forge\\forge-conductor.exe",
                "A merged role has the wrong command.");
        require(entry.at("args") == Json::array({"serve"}),
                "A merged role has arguments other than exactly [serve].");
        const auto& timeout = entry.at("timeout");
        require(timeout.is_number_integer() &&
                    timeout.get<std::int64_t>() == 180'000,
                "A merged role does not have the exact 180000 ms timeout.");
        require(entry.at("env").at("FORGE_DEPLOYMENT_ID").get<std::string>() ==
                    deploymentId.value(),
                "A merged role has the wrong shared revision.");
    }
    const auto inspection = take(LMStudioConfigurationCodec::inspect(
        document,
        path("C:\\Forge\\forge-conductor.exe"),
        path("C:\\Forge\\home")));
    require(inspection.registered && inspection.deploymentId == deploymentId,
            "A valid merged configuration was not recognized.");
}

void testStrictMalformedAndShapeRejection()
{
    requireError(
        LMStudioConfigurationCodec::parse(bytes("{not-json")),
        Domain::ErrorCodes::MalformedMessage,
        "Malformed existing configuration was accepted.");
    requireError(
        LMStudioConfigurationCodec::parse(bytes("[]")),
        Domain::ErrorCodes::InvalidRequest,
        "A non-object configuration root was accepted.");
    requireError(
        LMStudioConfigurationCodec::parse(bytes(R"({"mcpServers":[]})")),
        Domain::ErrorCodes::InvalidRequest,
        "A non-object mcpServers field was accepted.");
    requireError(
        LMStudioConfigurationCodec::parse(bytes(
            R"({"mcpServers":{},"mcpServers":{"forge-conductor":{}}})")),
        Domain::ErrorCodes::MalformedMessage,
        "An ambiguous duplicate JSON key was accepted.");
    requireError(
        LMStudioConfigurationCodec::parse(std::span<const std::byte>{}),
        Domain::ErrorCodes::MalformedMessage,
        "An empty existing configuration was accepted.");

    std::string oversized(LMStudioConfigurationCodec::MaximumDocumentBytes + 1U, 'x');
    requireError(
        LMStudioConfigurationCodec::parse(bytes(oversized)),
        Domain::ErrorCodes::PayloadTooLarge,
        "An oversized configuration reached the JSON parser.");
}

void testDriftMatrixFailsClosed()
{
    const auto expectedBinary = path("C:\\Forge\\forge-conductor.exe");
    const auto expectedHome = path("C:\\Forge\\home");
    const auto good = merged(revision("p15-revision-good"));

    const auto inspectMutated = [&](const auto& mutation) {
        auto json = Json::parse(good.sourceUtf8());
        mutation(json);
        const auto encoded = bytes(json.dump());
        const auto document = take(LMStudioConfigurationCodec::parse(encoded));
        return take(LMStudioConfigurationCodec::inspect(
            document, expectedBinary, expectedHome));
    };

    require(!inspectMutated([](Json& root) {
                 root["mcpServers"].erase(LMStudioFallbackServerId);
             }).registered,
            "A missing fallback registration was accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioPrimaryServerId]["command"] = "C:\\stale.exe";
             }).registered,
            "A stale binary path was accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioFallbackServerId]["env"]["FORGE_MCP_ROLE"] =
                     "primary";
             }).registered,
            "A wrong role environment value was accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioFallbackServerId]["env"]["FORGE_DEPLOYMENT_ID"] =
                     "different-revision";
             }).registered,
            "Mismatched role revisions were accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioPrimaryServerId]["env"]["FORGE_DEPLOYMENT_ID"] = "";
             }).registered,
            "An empty revision was accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioPrimaryServerId]["args"] =
                     Json::array({"serve", "extra"});
             }).registered,
            "Extra process arguments were accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioPrimaryServerId]["env"]["FORGE_CONDUCTOR_HOME"] =
                     "C:\\wrong-home";
             }).registered,
            "A stale Forge home was accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioPrimaryServerId].erase("timeout");
             }).registered,
            "A registration without a request timeout was accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioFallbackServerId]["timeout"] = 60'000;
             }).registered,
            "A stale 60000 ms request timeout was accepted.");
    require(inspectMutated([](Json& root) {
                root["mcpServers"][LMStudioPrimaryServerId]["timeout"] =
                    Json::number_integer_t{180'000};
            }).registered,
            "An exact signed integer request timeout was rejected.");
    require(inspectMutated([](Json& root) {
                root["mcpServers"][LMStudioFallbackServerId]["timeout"] =
                    Json::number_unsigned_t{180'000};
            }).registered,
            "An exact unsigned integer request timeout was rejected.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioCluServerId]["timeout"] = 180'000.0;
             }).registered,
            "A non-integer request timeout was accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioCluServerId]["timeout"] = "180000";
             }).registered,
            "A string request timeout was accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioCluServerId]["timeout"] = nullptr;
             }).registered,
            "A null request timeout was accepted.");
    require(!inspectMutated([](Json& root) {
                 root["mcpServers"][LMStudioCluServerId]["timeout"] = true;
             }).registered,
            "A boolean request timeout was accepted.");
}

void testEveryMergePublishesFreshRevisionBytes()
{
    const auto source = LMStudioConfigurationCodec::empty();
    const auto binary = path("C:\\Forge\\forge-conductor.exe");
    const auto home = path("C:\\Forge\\home");
    const auto first = take(LMStudioConfigurationCodec::mergeForgeServers(
        source, binary, home, revision("p15-revision-first")));
    const auto secondSource = take(LMStudioConfigurationCodec::parse(first));
    const auto second = take(LMStudioConfigurationCodec::mergeForgeServers(
        secondSource, binary, home, revision("p15-revision-second")));
    require(first != second,
            "A repeated merge did not change configuration bytes for a fresh revision.");
    require(text(second).find("p15-revision-second") != std::string::npos &&
                text(second).find("p15-revision-first") == std::string::npos,
            "The new shared revision was not applied to both Forge roles.");
}

void testSelectedProjectBindingAndRepair()
{
    const auto binary = path("C:\\Forge\\forge-conductor.exe");
    const auto home = path("C:\\Forge\\home");
    const auto project = parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001");
    const auto projectRoot = path("D:\\Projects\\Selected workspace");
    const auto deployment = revision("selected-project-revision");
    const auto original = merged(revision("old-selected-project-revision"));
    const auto encoded = take(LMStudioConfigurationCodec::mergeForgeServers(
        original, binary, home, deployment, project, projectRoot));
    const auto document = take(LMStudioConfigurationCodec::parse(encoded));
    const auto json = Json::parse(document.sourceUtf8());
    for (const auto* const id : {
             LMStudioPrimaryServerId, LMStudioFallbackServerId, LMStudioCluServerId}) {
        const auto& entry = json.at("mcpServers").at(id);
        require(entry.at("args") == Json::array({"serve", "--project-id", project.value()}),
            "A selected-project role omitted its explicit registered project ID.");
        require(entry.at("cwd").get<std::string>() == projectRoot.value(),
            "A selected-project role omitted its canonical project working directory.");
    }
    require(json.at("mcpServers").at("keep-me").at("foreign") == 17,
        "Project binding changed an unrelated MCP server.");
    require(json.at("mcpServers").at(LMStudioPrimaryServerId)
            .at("env").at("FOREIGN_ENV") == "keep",
        "Project binding removed an unrelated environment variable.");
    require(take(LMStudioConfigurationCodec::inspect(
        document, binary, home, project, projectRoot)).registered,
        "Selected-project registrations were not recognized.");
    require(take(LMStudioConfigurationCodec::inspect(document, binary, home)).registered,
        "Generic inspection rejected valid explicit project registrations.");

    const auto nextProject = parse<Domain::ProjectId>("20000000-0000-4000-8000-000000000002");
    const auto nextRoot = path("D:\\Projects\\Next workspace");
    const auto repaired = take(LMStudioConfigurationCodec::parse(take(
        LMStudioConfigurationCodec::mergeForgeServers(document, binary, home,
            revision("next-project-revision"), nextProject, nextRoot))));
    require(take(LMStudioConfigurationCodec::inspect(
        repaired, binary, home, nextProject, nextRoot)).registered,
        "Repair did not replace all three prior project bindings.");
    require(!take(LMStudioConfigurationCodec::inspect(
        repaired, binary, home, project, projectRoot)).registered,
        "Inspection credited the old project after selected-project repair.");
}

void testSelectedProjectBindingDriftFailsClosed()
{
    const auto binary = path("C:\\Forge\\forge-conductor.exe");
    const auto home = path("C:\\Forge\\home");
    const auto project = parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001");
    const auto projectRoot = path("D:\\Projects\\Selected workspace");
    const auto good = take(LMStudioConfigurationCodec::parse(take(
        LMStudioConfigurationCodec::mergeForgeServers(LMStudioConfigurationCodec::empty(),
            binary, home, revision("selected-project-revision"), project, projectRoot))));
    for (const auto* const id : {
             LMStudioPrimaryServerId, LMStudioFallbackServerId, LMStudioCluServerId}) {
        const auto inspectMutated = [&](const auto& mutation) {
            auto json = Json::parse(good.sourceUtf8());
            mutation(json["mcpServers"][id]);
            return take(LMStudioConfigurationCodec::inspect(take(
                LMStudioConfigurationCodec::parse(bytes(json.dump()))),
                binary, home, project, projectRoot));
        };
        const auto wrongProject = inspectMutated([](Json& entry) {
            entry["args"][2] = "20000000-0000-4000-8000-000000000002";
        });
        require(!wrongProject.registered && wrongProject.detail.find("project binding") != std::string::npos,
            "A role bound to another project was not reported as project drift.");
        require(!inspectMutated([](Json& entry) { entry["args"] = Json::array({"serve"}); }).registered,
            "A missing explicit selected-project argument was accepted.");
        require(!inspectMutated([](Json& entry) { entry["args"][2] = "invalid-project"; }).registered,
            "An invalid project UUID was accepted.");
        require(!inspectMutated([](Json& entry) { entry["args"][1] = "--home"; }).registered,
            "A different CLI option was accepted as project binding.");
        require(!inspectMutated([](Json& entry) { entry["args"].push_back("extra"); }).registered,
            "Extra arguments were accepted on a bound registration.");
        const auto wrongRoot = inspectMutated([](Json& entry) { entry["cwd"] = "D:\\Other workspace"; });
        require(!wrongRoot.registered && wrongRoot.detail.find("working directory") != std::string::npos,
            "A wrong selected working directory was not reported as drift.");
        require(!inspectMutated([](Json& entry) { entry.erase("cwd"); }).registered,
            "A missing selected working directory was accepted.");
        require(!inspectMutated([](Json& entry) { entry["cwd"] = 4; }).registered,
            "A non-string selected working directory was accepted.");
    }
    requireError(LMStudioConfigurationCodec::inspect(good, binary, home, project),
        Domain::ErrorCodes::InvalidRequest, "Inspection accepted a project ID without a root.");
    requireError(LMStudioConfigurationCodec::inspect(good, binary, home, std::nullopt, projectRoot),
        Domain::ErrorCodes::InvalidRequest, "Inspection accepted a project root without an ID.");
    requireError(LMStudioConfigurationCodec::mergeForgeServers(good, binary, home,
        revision("partial-project-revision"), project),
        Domain::ErrorCodes::InvalidRequest, "Merge accepted a project ID without a root.");
    requireError(LMStudioConfigurationCodec::mergeForgeServers(good, binary, home,
        revision("partial-project-revision"), std::nullopt, projectRoot),
        Domain::ErrorCodes::InvalidRequest, "Merge accepted a project root without an ID.");
}

void testUnselectedRepairPreservesExplicitProjectBinding()
{
    const auto binary = path("C:\\Forge\\forge-conductor.exe");
    const auto home = path("C:\\Forge\\home");
    const auto project = parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001");
    const auto projectRoot = path("D:\\Projects\\Selected workspace");
    const auto selected = take(LMStudioConfigurationCodec::parse(take(
        LMStudioConfigurationCodec::mergeForgeServers(merged(revision("legacy-revision")),
            binary, home, revision("selected-project-revision"), project, projectRoot))));
    const auto repaired = take(LMStudioConfigurationCodec::parse(take(
        LMStudioConfigurationCodec::mergeForgeServers(selected, binary, home,
            revision("unselected-repair-revision")))));
    require(take(LMStudioConfigurationCodec::inspect(
        repaired, binary, home, project, projectRoot)).registered,
        "Repair without a selection silently removed the existing project binding.");

    auto malformed = Json::parse(selected.sourceUtf8());
    malformed["mcpServers"][LMStudioPrimaryServerId]["args"][2] = "invalid-project";
    const auto fixed = take(LMStudioConfigurationCodec::parse(take(
        LMStudioConfigurationCodec::mergeForgeServers(take(
            LMStudioConfigurationCodec::parse(bytes(malformed.dump()))), binary, home,
            revision("malformed-binding-repair")))));
    const auto fixedJson = Json::parse(fixed.sourceUtf8());
    require(fixedJson.at("mcpServers").at(LMStudioPrimaryServerId).at("args") == Json::array({"serve"}),
        "Unselected repair preserved malformed explicit project arguments.");
    require(fixedJson.at("mcpServers").at(LMStudioPrimaryServerId)
            .at("cwd").get<std::string>() == projectRoot.value(),
        "Unselected repair removed the existing working directory.");
    require(take(LMStudioConfigurationCodec::inspect(fixed, binary, home)).registered,
        "Unselected repair did not produce supported legacy or explicit role arguments.");
}

} // namespace

void registerLMStudioConfigurationCodecTests(TestRegistry& tests)
{
    addTest(tests, "lmstudio.codec.preserve-foreign-and-unknown",
            testMergePreservesForeignAndUnknownFields);
    addTest(tests, "lmstudio.codec.strict-malformed-shapes",
            testStrictMalformedAndShapeRejection);
    addTest(tests, "lmstudio.codec.drift-matrix",
            testDriftMatrixFailsClosed);
    addTest(tests, "lmstudio.codec.fresh-revision",
            testEveryMergePublishesFreshRevisionBytes);
    addTest(tests, "lmstudio.codec.selected-project-binding",
            testSelectedProjectBindingAndRepair);
    addTest(tests, "lmstudio.codec.selected-project-drift",
            testSelectedProjectBindingDriftFailsClosed);
    addTest(tests, "lmstudio.codec.preserve-project-without-selection",
            testUnselectedRepairPreservesExplicitProjectBinding);
}

} // namespace ForgeConductor::Tests
