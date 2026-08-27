#include "TestSupport.h"

#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::LMStudioConfigurationCodec;
using Infrastructure::Windows::LMStudioConfigurationDocument;
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
    for (const auto* const id : {LMStudioPrimaryServerId, LMStudioFallbackServerId}) {
        const auto& entry = servers.at(id);
        require(entry.at("command").get<std::string>() ==
                    "C:\\Forge\\forge-conductor.exe",
                "A merged role has the wrong command.");
        require(entry.at("args") == Json::array({"serve"}),
                "A merged role has arguments other than exactly [serve].");
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
}

} // namespace ForgeConductor::Tests
