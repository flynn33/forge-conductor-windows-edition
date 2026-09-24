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
    { std::ofstream file{folder / "README.md"}; file << "Review source and prohibit vendor copies."; }
    { std::ofstream file{folder / "diagram.png", std::ios::binary}; file << "fixture"; }
    const auto folderUtf8 = folder.generic_u8string();
    const std::string source{reinterpret_cast<const char*>(folderUtf8.data()), folderUtf8.size()};
    A::ProjectPolicyService service{reader, store, hasher, projects, paths};
    D::ToolAuthorizationRequest write{{{id<D::RequestId>("write"), context.correlationId, client, project, "test"},
        "fs_write", R"({"path":"src/main.cpp","content":"test"})"}, D::ToolEffect::Write, {scope.authorityId(), scope.generation()}};
    REQUIRE(service.check(write, scope, context));
    A::ProjectPolicyService overlapping{reader, store, hasher, projects, paths, "C:\\policy-test\\private-state"};
    REQUIRE(!overlapping.check(write, scope, context));
    REQUIRE(!overlapping.execute({project, C::ProjectPolicyAction::Inspect}, context));
    const auto preview = Json::parse(take(service.execute({project, C::ProjectPolicyAction::Preview, source}, context)));
    const auto revision = preview.at("revision").get<std::string>();
    REQUIRE(!preview.at("adopted").get<bool>());
    REQUIRE(preview.at("files").size() == 1);
    REQUIRE(preview.at("excluded_files").size() == 1);
    REQUIRE(store.content.empty());
    REQUIRE(!service.execute({project, C::ProjectPolicyAction::Adopt, {}, "wrong"}, context));
    REQUIRE(service.execute({project, C::ProjectPolicyAction::Adopt, {}, revision}, context));
    REQUIRE(!service.check(write, scope, context));
    auto read = write;
    read.effect = D::ToolEffect::Read;
    read.call.toolName = "fs_read";
    REQUIRE(service.check(read, scope, context));
    const auto document = Json::parse(take(service.execute({project, C::ProjectPolicyAction::ReadDocument, "README.md", revision}, context)));
    REQUIRE(document.at("content") == "Review source and prohibit vendor copies.");
    Json review{{"schema", 1}, {"accepted", true}, {"policy_revision", revision}, {"reviewer", "Fixture reviewer"},
        {"reviewed_at", "2026-09-23T00:00:00Z"}, {"evidence", "Fixture review record"},
        {"unresolved_obligations", Json::array()}, {"non_text_review", "diagram.png excluded: fixture data"},
        {"source_coverage", Json::array({{{"path", "README.md"}, {"status", "read"}, {"evidence_or_reason", "Fixture reading record"}}})},
        {"write_paths", Json::array({"src"})}, {"prohibited_paths", Json::array({"src/vendor"})}, {"approved_calls", Json::array()}};
    auto incomplete = review;
    incomplete["source_coverage"] = Json::array();
    REQUIRE(!service.execute({project, C::ProjectPolicyAction::Review, {}, revision, incomplete.dump()}, context));
    REQUIRE(service.execute({project, C::ProjectPolicyAction::Review, {}, revision, review.dump()}, context));
    REQUIRE(service.check(write, scope, context));
    write.call.canonicalArguments = R"({"path":"src/vendor/copied.cpp","content":"bad"})";
    REQUIRE(!service.check(write, scope, context));
    write.call.canonicalArguments = R"({"path":"src/../outside.cpp","content":"bad"})";
    REQUIRE(!service.check(write, scope, context));
    write.call.canonicalArguments = R"({"path":"src/VENDOR~1/copied.cpp","content":"bad"})";
    REQUIRE(!service.check(write, scope, context));
    write.call.canonicalArguments = R"({"path":"src/\u00c9xample.cpp","content":"bad"})";
    REQUIRE(!service.check(write, scope, context));
    write.call.toolName = "shell_exec";
    write.effect = D::ToolEffect::Execute;
    write.call.canonicalArguments = R"({"command":"build"})";
    REQUIRE(!service.check(write, scope, context));
    review["approved_calls"].push_back({{"tool", "shell_exec"}, {"arguments", {{"command", "build"}}}});
    REQUIRE(service.execute({project, C::ProjectPolicyAction::Review, {}, revision, review.dump()}, context));
    REQUIRE(service.check(write, scope, context));
    write.call.canonicalArguments = R"({"command":"build; delete"})";
    REQUIRE(!service.check(write, scope, context));
    A::ProjectPolicyService restarted{reader, store, hasher, projects, paths};
    REQUIRE(!restarted.check(write, scope, context));
    const auto before = store.content;
    store.failWrites = true;
    REQUIRE(!service.execute({project, C::ProjectPolicyAction::Review, {}, revision, review.dump()}, context));
    REQUIRE(store.content == before);
    store.failWrites = false;
    auto corrupted = Json::parse(reinterpret_cast<const char*>(store.content.data()), reinterpret_cast<const char*>(store.content.data()) + store.content.size());
    corrupted["bundle"]["files"][0]["content"] = "tampered";
    const auto encoded = corrupted.dump();
    store.content.assign(reinterpret_cast<const std::byte*>(encoded.data()), reinterpret_cast<const std::byte*>(encoded.data()) + encoded.size());
    REQUIRE(!restarted.check(read, scope, context));
    REQUIRE(!reader.read("https://example.com/policy", context));
    std::cout << "Policy adoption, review, scope, command, restart, corruption and failed-write checks passed.\n";
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
                << " text_files=" << source.files.size() << " excluded_files=" << source.excludedFiles.size() << '\n';
        } else policyAdoptionAndAuthorization();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
