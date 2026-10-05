#include "TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsReviewerRunStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

namespace ForgeConductor::Tests {
namespace {
using namespace Infrastructure::Windows;
using Json = nlohmann::json;
Domain::OperationContext context() { return TestContext{}.active(); }
Domain::ProjectId project() { return parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001"); }
Domain::PathText pathText(const std::filesystem::path& path) {
    return take(Domain::PathText::create(take(Detail::strictUtf16ToUtf8(path.native()))));
}
class Tree final {
public:
    Tree() {
        std::wstring temporary(32768U, L'\0');
        const DWORD length = ::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        require(length != 0U && length < temporary.size(), "Temporary directory lookup failed."); temporary.resize(length);
        WindowsUuidGenerator generator; const auto value = take(generator.next()).value();
        root = std::filesystem::path{temporary} / std::filesystem::path{"ForgeConductor.Reviewer." + value};
        require(std::filesystem::create_directory(root), "Reviewer fixture root creation failed.");
        storage = root / L"memory" / L"reviewer-runs";
        require(std::filesystem::create_directories(storage), "Private reviewer fixture directory creation failed.");
    }
    ~Tree() { std::error_code ignored; static_cast<void>(std::filesystem::remove_all(root, ignored)); }
    std::filesystem::path root, storage;
};
struct Fixture final {
    Tree tree;
    WindowsWorkspaceAuthority authority{{WindowsWorkspaceAuthorityPolicy{
        parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"), project(), parse<Domain::ClientId>("reviewer-store-test"),
        {pathText(tree.root)}, Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create}, {}, false, 1U}}};
    WindowsAtomicFileStore files;
    BCryptSha256Hasher hasher;
    SystemClock clock;
    WindowsUuidGenerator uuids;
    Contracts::WorkspaceAuthority token{take(authority.authorityFor(project(), context()))};
    Contracts::AuthorizedPath directory{take(authority.authorize(token, {pathText(tree.storage), std::nullopt, Domain::FileAccess::Read, true}, context()))};
    std::filesystem::path path(const Domain::SessionId& id) const { return tree.storage / std::filesystem::path{id.value() + ".json"}; }
    ReviewerRunStorageResolver resolver() {
        return [this](const Domain::SessionId& id, const Domain::OperationContext& operation) -> Domain::Result<ReviewerRunStoragePaths> {
            const auto requested = pathText(path(id));
            auto read = authority.authorize(token, {requested, std::nullopt, Domain::FileAccess::Read, true}, operation);
            auto write = authority.authorize(token, {requested, std::nullopt, Domain::FileAccess::Write, true}, operation);
            auto create = authority.authorize(token, {requested, std::nullopt, Domain::FileAccess::Create, true}, operation);
            if (!read) return Domain::Result<ReviewerRunStoragePaths>::failure(read.error());
            if (!write) return Domain::Result<ReviewerRunStoragePaths>::failure(write.error());
            if (!create) return Domain::Result<ReviewerRunStoragePaths>::failure(create.error());
            return Domain::Result<ReviewerRunStoragePaths>::success({read.value(), write.value(), create.value()});
        };
    }
    std::unique_ptr<WindowsReviewerRunStore> store() { return std::make_unique<WindowsReviewerRunStore>(files, hasher, clock, directory, resolver()); }
    Domain::ManagedRunRecord record() {
        Domain::ManagedRunRecord record{Domain::SessionId{take(uuids.next())}, project(), token.callerId(), "Review the complete original report.", 4U};
        record.state = Domain::ManagedRunState::Completed; record.readOnlyTools = true;
        record.providerResponseId = parse<Domain::ProviderSessionId>("actual-response-store-fixture");
        record.inputTokens = 991U; record.outputTokens = 311U; record.retainedContextTokens = 2001U;
        record.createdAt = clock.utcNow(); record.updatedAt = record.createdAt;
        record.outputText = "Verified review fixture report.";
        return record;
    }
};
std::string read(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary}; require(static_cast<bool>(stream), "Reviewer fixture read failed.");
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc}; require(static_cast<bool>(stream), "Reviewer fixture write failed.");
    stream.write(text.data(), static_cast<std::streamsize>(text.size())); require(static_cast<bool>(stream), "Reviewer fixture write failed.");
}
void preservesLongTaskFullOutputAndTypedFields() {
    Fixture fixture; auto store = fixture.store(); auto record = fixture.record();
    require(!take(store->load(record.runId, context())), "An empty reviewer store invented a receipt.");
    record.task = "opening\xE2\x9C\x93"; record.task.resize(64U * 1024U, '\x01');
    record.outputText = "report\xF0\x9F\xA7\xAA"; record.outputText->resize(Domain::MaximumManagedRunOutputBytes, '\x02');
    record.outputTruncated = true;
    record.lastError = Domain::makeError("fixture_diagnostic", "A preserved diagnostic\xE2\x9C\x93", true, "actual-evidence-reference");
    const auto digest = parse<Domain::Sha256Digest>(std::string(64U, 'a'));
    record.nativeTaskChecks.push_back({digest, digest, digest, 0, true, false, false, true, 981U, record.updatedAt});
    take(store->save(record, context()));
    require(std::filesystem::file_size(fixture.path(record.runId)) > 1024U * 1024U,
        "Escaped full reviewer fixture did not exercise the old 1 MiB envelope limit.");
    auto restarted = fixture.store(); const auto loaded = take(restarted->load(record.runId, context()));
    require(loaded.has_value(), "Completed reviewer receipt disappeared after restart.");
    require(loaded->task == record.task && loaded->outputText == record.outputText && loaded->outputTruncated,
        "Full task/report text or truncation provenance was lost.");
    require(loaded->projectId == record.projectId && loaded->clientId == record.clientId && loaded->runId == record.runId
        && loaded->providerResponseId == record.providerResponseId && loaded->authorityGeneration == record.authorityGeneration,
        "Reviewer request/provider identities were changed.");
    require(loaded->inputTokens == 991U && loaded->outputTokens == 311U && loaded->retainedContextTokens == record.retainedContextTokens
        && loaded->createdAt == record.createdAt && loaded->updatedAt == record.updatedAt,
        "Reviewer actual usage or timestamps were changed.");
    require(loaded->lastError == record.lastError && loaded->readOnlyTools && loaded->allowTools
        && loaded->nativeTaskChecks.size() == 1U && loaded->nativeTaskChecks[0].commandDigest == digest
        && loaded->nativeTaskChecks[0].checkedAt == record.nativeTaskChecks[0].checkedAt,
        "Reviewer diagnostics, read-only state or native evidence were changed.");
    require(loaded->evidenceIntegrity == Domain::ManagedRunEvidenceIntegrity::Verified && loaded->evidenceSeal.has_value(),
        "Completed reviewer receipt has no verified integrity envelope.");
}
void rejectsTamperedWrongKindAndNonReadonly() {
    Fixture fixture; auto store = fixture.store(); auto record = fixture.record();
    record.readOnlyTools = false;
    requireError(store->save(record, context()), Domain::ErrorCodes::Unauthorized, "An executor record entered the reviewer store.");
    record.readOnlyTools = true; take(store->save(record, context()));
    const auto original = read(fixture.path(record.runId)); auto envelope = Json::parse(original);
    envelope["record"]["output_text"] = "fabricated approval"; const auto altered = envelope.dump(); write(fixture.path(record.runId), altered);
    requireError(store->load(record.runId, context()), Domain::ErrorCodes::IntegrityFailure, "An altered reviewer report was accepted.");
    requireError(store->save(record, context()), Domain::ErrorCodes::IntegrityFailure, "An altered reviewer report was overwritten.");
    require(read(fixture.path(record.runId)) == altered, "An altered reviewer receipt was not preserved.");
    envelope = Json::parse(original); envelope["kind"] = "forge_executor"; write(fixture.path(record.runId), envelope.dump());
    requireError(store->load(record.runId, context()), Domain::ErrorCodes::IntegrityFailure, "A wrong-kind receipt was accepted.");
}
void recoversInterruptedWithoutReplay() {
    Fixture fixture; auto store = fixture.store(); auto record = fixture.record(); record.state = Domain::ManagedRunState::Running;
    record.pendingFunctionCalls.push_back({"pending-actual-call", "fs_read", "{\"path\":\"evidence.txt\"}"});
    record.lastError = Domain::makeError("prior_error", "Prior preserved diagnostic", true, "prior-evidence");
    take(store->save(record, context()));
    auto restarted = fixture.store(); const auto recovered = take(restarted->load(record.runId, context()));
    require(recovered && recovered->state == Domain::ManagedRunState::Failed && recovered->lastError
        && recovered->lastError->code == Domain::ErrorCodes::Conflict && recovered->lastError->message.find("Outcome is unknown") != std::string::npos,
        "An interrupted reviewer was replayed or reported as approved.");
    require(recovered->pendingFunctionCalls.size() == 1U && recovered->pendingFunctionCalls[0].canonicalArguments == record.pendingFunctionCalls[0].canonicalArguments
        && recovered->providerResponseId == record.providerResponseId && recovered->inputTokens == record.inputTokens
        && recovered->lastError->evidenceId == record.lastError->evidenceId, "Interrupted reviewer facts were lost.");
    const auto sealed = read(fixture.path(record.runId));
    const auto reloaded = take(restarted->load(record.runId, context()));
    require(reloaded && reloaded->lastError == recovered->lastError && read(fixture.path(record.runId)) == sealed,
        "Repeated recovered readback rewrote the unknown-outcome receipt.");
    requireError(store->save(record, context()), Domain::ErrorCodes::Conflict, "A terminal reviewer was reopened for replay.");
}
void boundsRetentionAndHonorsContext() {
    Fixture fixture; auto store = fixture.store(); auto first = fixture.record(); take(store->save(first, context()));
    for (std::size_t index = 1U; index < WindowsReviewerRunStore::MaximumRetainedRecords; ++index) take(store->save(fixture.record(), context()));
    const auto extra = fixture.record();
    requireError(store->save(extra, context()), Domain::ErrorCodes::LimitExceeded, "The reviewer store exceeded sixteen retained receipts.");
    require(!std::filesystem::exists(fixture.path(extra.runId)), "Rejected reviewer admission wrote a receipt.");
    take(store->save(first, context())); require(take(store->load(first.runId, context())).has_value(), "Full retention blocked an existing receipt.");
    TestContext cancelled; cancelled.cancellation.request_stop();
    requireError(store->save(extra, cancelled.active()), Domain::ErrorCodes::Cancelled, "Reviewer store ignored cancellation.");
    requireError(store->load(first.runId, TestContext{}.expired()), Domain::ErrorCodes::DeadlineExceeded, "Reviewer store ignored a deadline.");
}
} // namespace
} // namespace ForgeConductor::Tests
int main() {
    using namespace ForgeConductor::Tests; TestRegistry tests;
    addTest(tests, "reviewer_store.full_text_and_fields", preservesLongTaskFullOutputAndTypedFields);
    addTest(tests, "reviewer_store.tamper_kind_authority", rejectsTamperedWrongKindAndNonReadonly);
    addTest(tests, "reviewer_store.interrupted_recovery", recoversInterruptedWithoutReplay);
    addTest(tests, "reviewer_store.retention_context", boundsRetentionAndHonorsContext);
    std::size_t passed{};
    for (const auto& [name, run] : tests) { try { run(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& failure) { std::cerr << "FAIL " << name << ": " << failure.what() << '\n'; } }
    std::cout << "SUMMARY passed=" << passed << " failed=" << (tests.size() - passed) << '\n';
    return passed == tests.size() ? EXIT_SUCCESS : EXIT_FAILURE;
}
