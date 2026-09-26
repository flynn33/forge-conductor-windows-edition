#include "ForgeConductor/Application/ProjectPolicyService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsPolicySourceReader.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/ProjectRepositoryFakes.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace D = ForgeConductor::Domain;
namespace C = ForgeConductor::Contracts;
namespace A = ForgeConductor::Application;
namespace W = ForgeConductor::Infrastructure::Windows;
namespace F = ForgeConductor::Tests::Fakes;
using Json = nlohmann::json;
#define REQUIRE(value) do { if (!(value)) throw std::runtime_error{#value}; } while (false)
template<class T> T take(D::Result<T> value) { if (!value) throw std::runtime_error{value.error().message}; return std::move(value).value(); }
template<class T> T id(const char* value) { return take(T::parse(value)); }

class Store final : public C::IAtomicFileStore {
public:
    std::vector<std::byte> content;
    bool failWrites{};
    D::Result<std::vector<std::byte>> read(const C::AuthorizedPath&, std::size_t, const D::OperationContext&) noexcept override {
        if (content.empty()) return D::Result<std::vector<std::byte>>::failure(D::makeError(D::ErrorCodes::RecordNotFound, "missing"));
        return D::Result<std::vector<std::byte>>::success(content);
    }
    D::Result<void> replace(const C::AuthorizedPath&, std::span<const std::byte> value, bool, const D::OperationContext&) noexcept override {
        if (failWrites) return D::Result<void>::failure(D::makeError(D::ErrorCodes::InternalFailure, "injected disk failure"));
        content.assign(value.begin(), value.end());
        return D::Result<void>::success();
    }
};

void policyAdoptionAndAuthorization()
{
    const auto now = std::chrono::steady_clock::now();
    D::OperationContext context{id<D::OperationId>("12121212-1212-4212-8212-121212121212"),
        now + std::chrono::minutes{2}, {}, id<D::CorrelationId>("policy-test")};
    const auto project = id<D::ProjectId>("13131313-1313-4313-8313-131313131313");
    const auto client = id<D::ClientId>("policy-client");
    const auto root = take(D::PathText::create("C:\\policy-test"));
    F::DeterministicWorkspaceAuthority authority{id<D::AuthorityId>("14141414-1414-4414-8414-141414141414"),
        client, {root}, D::FileAccess::Write, {D::FileAccess::Read, D::FileAccess::Write, D::FileAccess::Create}, {}, true, 1U};
    const auto scope = take(authority.authorityFor(project, context));
    const auto paths = [&](const D::ProjectId&, const D::OperationContext& operation) {
        const auto path = take(D::PathText::create("C:\\policy-test\\snapshot.json"));
        return D::Result<A::PolicyStoragePaths>::success({
            take(authority.authorize(scope, {path, root, D::FileAccess::Read}, operation)),
            take(authority.authorize(scope, {path, root, D::FileAccess::Write}, operation)),
            take(authority.authorize(scope, {path, root, D::FileAccess::Create}, operation))});
    };
    F::ProjectRegistryRepositoryFake projects{4, now};
    REQUIRE(projects.seedDescriptor({project, "Test", {}, {root}}));
    Store store;
    W::BCryptSha256Hasher hasher;
    W::WindowsPolicySourceReader reader;
    const auto folder = std::filesystem::current_path() / "out" /
        ("policy-test-" + std::to_string(now.time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    { std::ofstream file{folder / "README.md"}; file << "Development policy evidence."; }
    { std::ofstream file{folder / "policy.json"}; file << R"({"forge_clu_rules":[{"id":"no-shell","tool":"shell_exec","severity":"error","correction":"Use the bounded build runner."}]})"; }
    { std::ofstream file{folder / "diagram.png", std::ios::binary};
      constexpr char bytes[]{'f', 'i', 'x', '\0', 'b'};
      file.write(bytes, sizeof(bytes)); }
    const auto folderUtf8 = folder.generic_u8string();
    const std::string source{reinterpret_cast<const char*>(folderUtf8.data()), folderUtf8.size()};
    A::ProjectPolicyService service{reader, store, hasher, projects, paths};
    D::ToolAuthorizationRequest write{{{id<D::RequestId>("write"), context.correlationId, client, project, "test"},
        "fs_write", R"({"path":"src/main.cpp","content":"test"})"}, D::ToolEffect::Write, {scope.authorityId(), scope.generation()}};
    REQUIRE(service.check(write, scope, context));

    const Json legacy{{"schema", 1}, {"project", project.value()},
        {"revision", std::string(64U, '1')},
        {"bundle", {{"source", "legacy-policy"}, {"commit", "legacy-commit"},
            {"files", Json::array({{{"path", "POLICY.md"},
                {"content", "FORBID_TOOL legacy_shell"}}})},
            {"excluded_files", Json::array({"opaque.bin"})}}},
        {"review", {{"reviewer", "owner"}, {"attestation", "accepted"}}}};
    const auto legacyEncoded = legacy.dump();
    store.content.assign(reinterpret_cast<const std::byte*>(legacyEncoded.data()),
        reinterpret_cast<const std::byte*>(legacyEncoded.data()) + legacyEncoded.size());
    const auto migrated = Json::parse(take(service.execute(
        {project, C::ProjectPolicyAction::Inspect}, context)));
    REQUIRE(migrated.at("active").get<bool>());
    REQUIRE(migrated.at("coverage_gap_count").get<std::size_t>() == 1U);
    const auto persistedMigration = Json::parse(
        reinterpret_cast<const char*>(store.content.data()),
        reinterpret_cast<const char*>(store.content.data()) + store.content.size());
    REQUIRE(persistedMigration.at("schema") == 2);
    REQUIRE(persistedMigration.at("history").at(0).at("kind") == "legacy_policy_review");
    REQUIRE(persistedMigration.at("rules").size() == 1U);
    store.content.clear();

    A::ProjectPolicyService overlapping{reader, store, hasher, projects, paths, "C:\\policy-test\\private-state"};
    REQUIRE(overlapping.check(write, scope, context));
    REQUIRE(!overlapping.execute({project, C::ProjectPolicyAction::Inspect}, context));
    const auto bound = Json::parse(take(service.execute(
        {project, C::ProjectPolicyAction::Bind, source}, context)));
    const auto revision = bound.at("revision").get<std::string>();
    REQUIRE(bound.at("active").get<bool>());
    REQUIRE(bound.at("entry_count").get<std::size_t>() == 3U);
    REQUIRE(bound.at("coverage_gap_count").get<std::size_t>() == 1U);
    const auto document = Json::parse(take(service.execute({project, C::ProjectPolicyAction::ReadDocument, "README.md", revision}, context)));
    REQUIRE(document.at("content") == "Development policy evidence.");

    const Json evidence{{"phase", "post_operation"}, {"tool_name", "shell_exec"},
        {"arguments", R"({"command":"build","access_token":"native-secret"})"},
        {"credential", "native-secret"}, {"effect", "execute"}};
    const auto evaluation = Json::parse(take(service.execute({project,
        C::ProjectPolicyAction::Evaluate, {}, revision, evidence.dump()}, context)));
    REQUIRE(evaluation.at("finding_id").is_string());
    const auto findingId = evaluation.at("finding_id").get<std::string>();
    const auto findings = Json::parse(take(service.execute({project,
        C::ProjectPolicyAction::ListFindings, {}, revision}, context)));
    REQUIRE(findings.at("findings").size() == 1U);
    REQUIRE(findings.at("notifications").size() == 1U);
    const Json compliantEvidence{{"phase", "post_operation"},
        {"tool_name", "project_policy.read"}, {"arguments", "{}"},
        {"effect", "read"}};
    const auto compliant = Json::parse(take(service.execute({project,
        C::ProjectPolicyAction::Evaluate, {}, revision,
        compliantEvidence.dump()}, context)));
    REQUIRE(compliant.at("finding_id").is_null());
    const auto activity = Json::parse(take(service.execute({project,
        C::ProjectPolicyAction::ListFindings, {}, revision}, context)));
    REQUIRE(activity.at("activity").back().at("finding_id") == "");

    write.call.toolName = "shell_exec";
    write.effect = D::ToolEffect::Execute;
    write.call.canonicalArguments = R"({"command":"build"})";
    REQUIRE(service.check(write, scope, context));
    const Json resolution{{"finding_id", findingId},
        {"correction_evidence", {{"kind", "rerun"}, {"result", "clean"},
            {"private_key", "native-private-material"}}}};
    const auto resolved = Json::parse(take(service.execute({project,
        C::ProjectPolicyAction::Resolve, {}, revision, resolution.dump()}, context)));
    REQUIRE(resolved.at("finding").at("state") == "resolved");
    const auto exported = Json::parse(take(service.execute({project,
        C::ProjectPolicyAction::ExportLog, {}, revision}, context)));
    REQUIRE(exported.at("schema") == "forge-clu-governance-log-v1");
    for (const auto& coverage : exported.at("coverage")) REQUIRE(!coverage.contains("content"));
    const auto exportedText = exported.dump();
    REQUIRE(exportedText.find("native-secret") == std::string::npos);
    REQUIRE(exportedText.find("native-private-material") == std::string::npos);
    REQUIRE(exportedText.find("[REDACTED]") != std::string::npos);
    REQUIRE(service.execute({project, C::ProjectPolicyAction::Refresh}, context));
    A::ProjectPolicyService restarted{reader, store, hasher, projects, paths};
    REQUIRE(restarted.check(write, scope, context));
    const auto before = store.content;
    store.failWrites = true;
    REQUIRE(!service.execute({project, C::ProjectPolicyAction::Evaluate, {}, {}, evidence.dump()}, context));
    REQUIRE(store.content == before);
    store.failWrites = false;
    auto corrupted = Json::parse(reinterpret_cast<const char*>(store.content.data()), reinterpret_cast<const char*>(store.content.data()) + store.content.size());
    corrupted["schema"] = 99;
    const auto encoded = corrupted.dump();
    store.content.assign(reinterpret_cast<const std::byte*>(encoded.data()), reinterpret_cast<const std::byte*>(encoded.data()) + encoded.size());
    REQUIRE(!restarted.execute({project, C::ProjectPolicyAction::Inspect}, context));
    REQUIRE(restarted.check(write, scope, context));
    REQUIRE(!reader.read("https://example.com/policy", context));
    std::cout << "Policy binding, legacy migration, native finding correction, and redacted export receipt passed: finding="
        << findingId << " state=resolved redacted=true.\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view{argv[1]} == "--policy-source") {
            W::WindowsPolicySourceReader reader;
            D::OperationContext context{id<D::OperationId>("12121212-1212-4212-8212-121212121212"),
                std::chrono::steady_clock::now() + std::chrono::minutes{5}, {}, id<D::CorrelationId>("policy-source-check")};
            const auto source = take(reader.read(argv[2], context));
            std::cout << "Policy source verified: " << source.source << " commit=" << source.commit
                << " inventoried_entries=" << source.files.size() << '\n';
        } else policyAdoptionAndAuthorization();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
