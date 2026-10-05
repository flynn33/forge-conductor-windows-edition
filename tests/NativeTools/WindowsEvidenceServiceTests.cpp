#include "Infrastructure/TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/NativeTools/Windows/WindowsEvidenceService.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace ForgeConductor::Tests {
namespace {
using Json = nlohmann::json;
using namespace Infrastructure::Windows;
using NativeTools::Windows::WindowsEvidenceService;
Domain::ProjectId project() { return parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001"); }
Domain::ProjectId storageProject() { return parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000002"); }
Domain::PathText pathText(const std::filesystem::path& path) {
    return take(Domain::PathText::create(take(Detail::strictUtf16ToUtf8(path.native()))));
}
Domain::OperationContext context() { return TestContext{}.active(); }
class ScopedTree final {
public:
    ScopedTree() {
        std::wstring temporary(32768U, L'\0');
        const DWORD length = ::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        require(length != 0U && length < temporary.size(), "GetTempPathW failed.");
        temporary.resize(length);
        WindowsUuidGenerator generator;
        const auto identifier = take(generator.next()).value();
        root = std::filesystem::path{temporary} / std::filesystem::path{"ForgeConductor.Evidence." + identifier};
        workspace = root / L"workspace";
        external = root / L"evidence";
        storage = root / L"private";
        require(std::filesystem::create_directories(workspace) && std::filesystem::create_directories(external)
            && std::filesystem::create_directories(storage), "Test directories could not be created.");
    }
    ~ScopedTree() { std::error_code ignored; static_cast<void>(std::filesystem::remove_all(root, ignored)); }
    std::filesystem::path root, workspace, external, storage;
};
void write(const std::filesystem::path& path, std::string_view content) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    require(static_cast<bool>(stream), "Fixture could not open.");
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(static_cast<bool>(stream), "Fixture could not write.");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    require(static_cast<bool>(stream), "Fixture could not read.");
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
std::vector<WindowsWorkspaceAuthorityPolicy> policies(const ScopedTree& tree, bool readonly = false) {
    const auto caller = parse<Domain::ClientId>("evidence-test");
    const std::vector<Domain::FileAccess> writable{Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create};
    return {
        {parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"), project(), caller,
            {pathText(tree.workspace), pathText(tree.external)}, readonly ? Domain::FileAccess::Read : Domain::FileAccess::Write,
            readonly ? std::vector<Domain::FileAccess>{Domain::FileAccess::Read} : writable, {}, false, 1U},
        {parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000002"), storageProject(), caller,
            {pathText(tree.storage)}, Domain::FileAccess::Write, writable, {}, false, 1U}};
}
struct Fixture final {
    ScopedTree tree;
    WindowsWorkspaceAuthority authority{policies(tree)};
    WindowsAtomicFileStore files;
    BCryptSha256Hasher hasher;
    SystemClock clock;
    WindowsUuidGenerator uuids;
    Contracts::WorkspaceAuthority token{take(authority.authorityFor(project(), context()))};
    Contracts::EvidenceStorageResolver resolver() {
        return [this](const Domain::ProjectId& requested, const Domain::OperationContext& operation) -> Domain::Result<Contracts::EvidenceStoragePaths> {
            if (requested != project()) return Domain::Result<Contracts::EvidenceStoragePaths>::failure(
                Domain::makeError(Domain::ErrorCodes::ProjectScopeMismatch, "Wrong evidence project."));
            auto storageAuthority = authority.authorityFor(storageProject(), operation);
            if (!storageAuthority) return Domain::Result<Contracts::EvidenceStoragePaths>::failure(storageAuthority.error());
            const auto path = pathText(tree.storage / L"capture-log.json");
            auto readPath = authority.authorize(storageAuthority.value(), {path, std::nullopt, Domain::FileAccess::Read, true}, operation);
            auto writePath = authority.authorize(storageAuthority.value(), {path, std::nullopt, Domain::FileAccess::Write, true}, operation);
            auto createPath = authority.authorize(storageAuthority.value(), {path, std::nullopt, Domain::FileAccess::Create, true}, operation);
            if (!readPath) return Domain::Result<Contracts::EvidenceStoragePaths>::failure(readPath.error());
            if (!writePath) return Domain::Result<Contracts::EvidenceStoragePaths>::failure(writePath.error());
            if (!createPath) return Domain::Result<Contracts::EvidenceStoragePaths>::failure(createPath.error());
            return Domain::Result<Contracts::EvidenceStoragePaths>::success(
                {readPath.value(), writePath.value(), createPath.value()});
        };
    }
    std::unique_ptr<WindowsEvidenceService> service() {
        return std::make_unique<WindowsEvidenceService>(authority, files, hasher, clock, uuids, resolver(), pathText(tree.storage));
    }
    std::filesystem::path logPath() const { return tree.storage / L"capture-log.json"; }
};
void capturesBinaryAndDurableChain() {
    Fixture fixture;
    auto service = fixture.service();
    const auto empty = Json::parse(take(service->readLog(fixture.token, 0U, 4U, true, context())));
    require(empty.at("found") == false && empty.at("total") == 0U, "Missing capture log was not an empty verified page.");
    std::string binary(3U * 1024U * 1024U, '\0');
    for (std::size_t index{}; index < binary.size(); ++index) binary[index] = static_cast<char>(index % 256U);
    write(fixture.tree.external / L"binary.bin", binary);
    const auto first = Json::parse(take(service->digest({pathText(fixture.tree.external / L"binary.bin")}, fixture.token, context())));
    require(first.at("sequence") == 1U && first.at("files").at(0).at("bytes") == binary.size(), "Binary byte count or sequence was wrong.");
    require(first.at("files").at(0).at("sha256") == "f6dd7fec8584ad00219a447071c1fa368a1caee4d9c146083d233713ddccd2c0",
        "Streamed binary SHA-256 did not match the independently calculated fixture digest.");
    write(fixture.tree.workspace / L"abc.txt", "abc");
    auto restarted = fixture.service();
    const auto second = Json::parse(take(restarted->digest({pathText(fixture.tree.workspace / L"abc.txt")}, fixture.token, context())));
    require(second.at("sequence") == 2U && second.at("previous_digest") == first.at("digest"), "A recreated service did not append the persisted chain.");
    require(second.at("files").at(0).at("sha256") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "The abc SHA-256 fixture digest was wrong.");
    auto firstPage = Json::parse(take(restarted->readLog(fixture.token, 0U, 1U, true, context())));
    require(firstPage.at("total") == 2U && firstPage.at("entries").size() == 1U && firstPage.at("next_offset") == 1U
        && firstPage.at("head_digest") == second.at("digest") && firstPage.at("complete") == false, "Capture pagination was wrong.");
    const auto secondPage = Json::parse(take(restarted->readLog(fixture.token, 1U, 1U, true, context())));
    require(secondPage.at("complete") == true && secondPage.at("next_offset").is_null(), "Last capture page did not finish.");
}
void rejectsInvalidPathsAndHonorsContext() {
    Fixture fixture;
    auto service = fixture.service();
    write(fixture.tree.workspace / L"one.txt", "one");
    const auto valid = pathText(fixture.tree.workspace / L"one.txt");
    write(fixture.tree.root / L"outside.txt", "outside");
    requireError(service->digest({pathText(fixture.tree.root / L"outside.txt")}, fixture.token, context()),
        Domain::ErrorCodes::PathOutsideAuthority, "An unapproved path was captured.");
    requireError(service->digest({valid, valid}, fixture.token, context()), Domain::ErrorCodes::InvalidRequest,
        "Duplicate evidence paths were captured.");
    requireError(service->digest({}, fixture.token, context()), Domain::ErrorCodes::LimitExceeded, "An empty evidence capture was accepted.");
    requireError(service->digest(std::vector<Domain::PathText>(65U, valid), fixture.token, context()), Domain::ErrorCodes::LimitExceeded,
        "More than 64 evidence paths were accepted.");
    requireError(service->digest({pathText(fixture.tree.workspace)}, fixture.token, context()), Domain::ErrorCodes::InvalidRequest,
        "A directory was captured as a regular file.");
    TestContext cancelled;
    cancelled.cancellation.request_stop();
    requireError(service->digest({valid}, fixture.token, cancelled.active()), Domain::ErrorCodes::Cancelled, "Evidence capture ignored cancellation.");
    requireError(service->digest({valid}, fixture.token, TestContext{}.expired()), Domain::ErrorCodes::DeadlineExceeded,
        "Evidence capture ignored its deadline.");
    requireError(service->readLog(fixture.token, 0U, 0U, true, context()), Domain::ErrorCodes::LimitExceeded,
        "A zero-record capture page was accepted.");
    requireError(service->readLog(fixture.token, 0U, 17U, true, context()), Domain::ErrorCodes::LimitExceeded,
        "A capture page above sixteen records was accepted.");
    requireError(service->readLog(fixture.token, 1U, 4U, true, context()), Domain::ErrorCodes::InvalidRequest,
        "An offset beyond the capture count was accepted.");
    require(!std::filesystem::exists(fixture.logPath()), "A failed capture wrote a log.");
}
void detectsAndPreservesAlteredLogs() {
    Fixture fixture;
    auto service = fixture.service();
    write(fixture.tree.workspace / L"one.txt", "one");
    const auto path = pathText(fixture.tree.workspace / L"one.txt");
    static_cast<void>(take(service->digest({path}, fixture.token, context())));
    auto log = Json::parse(read(fixture.logPath()));
    log["records"][0]["files"][0]["bytes"] = 123456U;
    const auto altered = log.dump();
    write(fixture.logPath(), altered);
    requireError(service->readLog(fixture.token, 0U, 4U, true, context()), Domain::ErrorCodes::IntegrityFailure,
        "An altered capture record passed chain verification.");
    requireError(service->digest({path}, fixture.token, context()), Domain::ErrorCodes::IntegrityFailure,
        "Evidence capture appended to an altered log.");
    require(read(fixture.logPath()) == altered, "An altered log was overwritten.");
    const auto unverified = Json::parse(take(service->readLog(fixture.token, 0U, 4U, false, context())));
    require(unverified.at("integrity") == "not_verified", "Skipping hashes claimed a verified capture chain.");
    write(fixture.logPath(), "");
    requireError(service->readLog(fixture.token, 0U, 4U, true, context()), Domain::ErrorCodes::IntegrityFailure,
        "An empty existing capture log was silently reset.");
    require(read(fixture.logPath()).empty(), "An empty corrupt log was rewritten.");
}
void preservesFullLog() {
    Fixture fixture;
    auto service = fixture.service();
    write(fixture.tree.workspace / L"one.txt", "one");
    const auto path = pathText(fixture.tree.workspace / L"one.txt");
    static_cast<void>(take(service->digest({path}, fixture.token, context())));
    auto log = Json::parse(read(fixture.logPath()));
    // A valid chain close to the physical log bound, with a hashed extension
    // field, exercises append admission without hundreds of native captures.
    auto& record = log["records"][0];
    record["extension"] = std::string(WindowsEvidenceService::MaximumLogBytes - log.dump().size() - 200U, 'x');
    auto payload = record;
    payload.erase("digest");
    const auto encodedPayload = payload.dump();
    record["digest"] = take(fixture.hasher.sha256(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(encodedPayload.data()), encodedPayload.size()})).value();
    log["head_digest"] = record.at("digest");
    const auto full = log.dump();
    require(full.size() <= WindowsEvidenceService::MaximumLogBytes, "The full-log fixture exceeded read admission.");
    write(fixture.logPath(), full);
    requireError(service->digest({path}, fixture.token, context()), Domain::ErrorCodes::LimitExceeded,
        "A full capture log accepted an append.");
    require(read(fixture.logPath()) == full, "A full capture log was replaced.");
}
void serializesConcurrentCapturesAndAllowsReadOnlyReadback() {
    Fixture fixture;
    auto first = fixture.service();
    auto second = fixture.service();
    write(fixture.tree.workspace / L"one.txt", "one");
    const auto path = pathText(fixture.tree.workspace / L"one.txt");
    std::optional<Domain::Result<std::string>> firstResult, secondResult;
    std::thread left{[&] { firstResult.emplace(first->digest({path}, fixture.token, context())); }};
    std::thread right{[&] { secondResult.emplace(second->digest({path}, fixture.token, context())); }};
    left.join(); right.join();
    require(firstResult.has_value() && secondResult.has_value(), "Concurrent capture did not return.");
    static_cast<void>(take(std::move(*firstResult)));
    static_cast<void>(take(std::move(*secondResult)));
    const auto page = Json::parse(take(first->readLog(fixture.token, 0U, 4U, true, context())));
    require(page.at("total") == 2U && page.at("entries")[0].at("sequence") == 1U
        && page.at("entries")[1].at("sequence") == 2U, "Concurrent captures lost a durable chain record.");
    WindowsWorkspaceAuthority readonly{policies(fixture.tree, true)};
    auto readonlyToken = take(readonly.authorityFor(project(), context()));
    WindowsEvidenceService readonlyService{readonly, fixture.files, fixture.hasher, fixture.clock, fixture.uuids,
        fixture.resolver(), pathText(fixture.tree.storage)};
    requireError(readonlyService.digest({path}, readonlyToken, context()), Domain::ErrorCodes::Unauthorized,
        "A read-only authority appended a capture log.");
    const auto readback = Json::parse(take(readonlyService.readLog(readonlyToken, 0U, 4U, true, context())));
    require(readback.at("total") == 2U, "Read-only authority could not inspect the verified capture log.");
}
} // namespace
} // namespace ForgeConductor::Tests
int main() {
    using namespace ForgeConductor::Tests;
    TestRegistry tests;
    addTest(tests, "evidence.binary_and_durable_chain", capturesBinaryAndDurableChain);
    addTest(tests, "evidence.invalid_paths_context", rejectsInvalidPathsAndHonorsContext);
    addTest(tests, "evidence.tamper_preserved", detectsAndPreservesAlteredLogs);
    addTest(tests, "evidence.full_log_preserved", preservesFullLog);
    addTest(tests, "evidence.concurrent_readonly", serializesConcurrentCapturesAndAllowsReadOnlyReadback);
    std::size_t passed{};
    for (const auto& [name, run] : tests) {
        try { run(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << "SUMMARY passed=" << passed << " failed=" << (tests.size() - passed) << '\n';
    return passed == tests.size() ? EXIT_SUCCESS : EXIT_FAILURE;
}