#include "Infrastructure/TestSupport.h"
#include "ForgeConductor/Application/ScheduledTaskService.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Mcp/McpJsonCodec.h"
#include "ForgeConductor/Mcp/McpServer.h"
#include "ForgeConductor/Mcp/McpToolCatalog.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace ForgeConductor::Tests {
namespace {
using Json = nlohmann::json;
using Service = Application::ScheduledTaskService;
class Clock final : public Contracts::IClock {
public:
    Domain::UtcTimePoint utcNow() const noexcept override { std::lock_guard lock{mutex}; return now; }
    Domain::MonotonicTimePoint monotonicNow() const noexcept override { return std::chrono::steady_clock::now(); }
    void advance(std::chrono::seconds amount) { std::lock_guard lock{mutex}; now += amount; }
private:
    mutable std::mutex mutex;
    Domain::UtcTimePoint now{std::chrono::sys_days{std::chrono::year{2026}/10/7}};
};
class Uuid final : public Contracts::IUuidGenerator {
public:
    Domain::Result<Domain::Uuid> next() noexcept override {
        std::lock_guard lock{mutex}; char value[40]{};
        std::snprintf(value, sizeof(value), "30000000-0000-4000-8000-%012u", ++sequence);
        return Domain::Uuid::parse(value);
    }
private:
    std::mutex mutex; unsigned sequence{};
};
class Authority final : public Contracts::IWorkspaceAuthority {
public:
    std::uint64_t generation{1U};
    std::vector<Domain::PathText> roots{take(Domain::PathText::create("D:\\workspace"))};
    std::vector<Domain::FileAccess> grants{Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create};
    bool shell{};
    Domain::ProjectId project{parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001")};
    Domain::ClientId client{parse<Domain::ClientId>("scheduler-owner")};
    Domain::Result<Contracts::WorkspaceAuthority> authorityFor(const Domain::ProjectId& requested, const Domain::OperationContext&) noexcept override {
        if (requested != project) return Domain::Result<Contracts::WorkspaceAuthority>::failure(Domain::makeError(Domain::ErrorCodes::ProjectScopeMismatch, "Unknown project"));
        return issueAuthority(parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"), project, client, roots,
            grants.front(), grants, {}, shell, generation);
    }
    Domain::Result<Contracts::WorkspaceAuthority> narrow(const Contracts::WorkspaceAuthority& value, const std::vector<Domain::PathText>& selected,
        const std::vector<Domain::FileAccess>& selectedGrants, bool enabled, std::uint64_t nextGeneration, const Domain::OperationContext&) noexcept override {
        return narrowAuthority(value, selected, selectedGrants, enabled, nextGeneration);
    }
    Domain::Result<Contracts::AuthorizedPath> authorize(const Contracts::WorkspaceAuthority& value, const Domain::PathAuthorizationRequest& request,
        const Domain::OperationContext&) noexcept override { return issueAuthorizedPath(value, request.requestedPath, value.trustedRoots().front(), request.access); }
    Contracts::AuthorizedPath storage(Domain::FileAccess access) {
        const auto dataRoot = take(Domain::PathText::create("C:\\owner-home\\forge"));
        auto data = take(issueAuthority(parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000002"), project, client, {dataRoot}, Domain::FileAccess::Read,
            {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create}, {}, false, 1U));
        return take(issueAuthorizedPath(data, take(Domain::PathText::create("C:\\owner-home\\forge\\schedules.json")), dataRoot, access));
    }
};
class Store final : public Contracts::IAtomicFileStore {
public:
    std::vector<std::byte> bytes;
    bool failWrites{}, ambiguousWrites{};
    std::string lastPath;
    Domain::Result<std::vector<std::byte>> read(const Contracts::AuthorizedPath& path, std::size_t maximum,
        const Domain::OperationContext&) noexcept override {
        lastPath = path.canonicalPath().value();
        if (bytes.empty()) return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(Domain::ErrorCodes::RecordNotFound, "No schedule store"));
        if (bytes.size() > maximum) return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(Domain::ErrorCodes::PayloadTooLarge, "Too large"));
        return Domain::Result<std::vector<std::byte>>::success(bytes);
    }
    Domain::Result<void> replace(const Contracts::AuthorizedPath& path, std::span<const std::byte> content, bool backup,
        const Domain::OperationContext&) noexcept override {
        lastPath = path.canonicalPath().value(); require(backup, "Schedule writes must retain the atomic store's durable backup.");
        if (!failWrites || ambiguousWrites) bytes.assign(content.begin(), content.end());
        if (failWrites) return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::Conflict, "Simulated persistence failure"));
        return Domain::Result<void>::success();
    }
};
class Catalog final : public Contracts::IToolCatalog {
public:
    std::span<const Domain::McpToolDescriptor> tools() const noexcept override { return descriptors; }
private:
    std::vector<Domain::McpToolDescriptor> descriptors{
        {{"fs_read", "read", "fs", Domain::ToolEffect::Read, Domain::ToolAvailability::Available, true, false}, "{}"},
        {{"fs_write", "write", "fs", Domain::ToolEffect::Write, Domain::ToolAvailability::Available, true, false}, "{}"},
        {{"agent_spawn", "spawn", "agents", Domain::ToolEffect::Execute, Domain::ToolAvailability::Available, true, false}, "{}"},
        {{"schedule_create", "schedule", "schedule", Domain::ToolEffect::Write, Domain::ToolAvailability::Available, true, false}, "{}"}};
};
class Runs final : public Contracts::IManagedRunService {
public:
    std::optional<Domain::Error> startError, statusError;
    Domain::Result<Domain::ManagedRunSnapshot> start(const Domain::ManagedRunStartRequest& request, const Domain::OperationContext&) noexcept override {
        std::lock_guard lock{mutex};
        ++attempted;
        if (startError) return Domain::Result<Domain::ManagedRunSnapshot>::failure(*startError);
        requests.push_back(request);
        Domain::ManagedRunRecord record{request.runId, request.projectId, request.clientId, request.task, request.authorityGeneration,
            Domain::ManagedRunState::Running, {}, 0U, 0U, {}, {}, {}, {}, {}, {}, request.allowTools};
        record.readOnlyTools = request.readOnlyTools; record.workerScope = request.workerScope;
        records.emplace(request.runId, record);
        return Domain::Result<Domain::ManagedRunSnapshot>::success({record, true, false, false});
    }
    Domain::Result<Domain::ManagedRunSnapshot> status(const Domain::SessionId& id, const Domain::OperationContext&) noexcept override {
        std::lock_guard lock{mutex};
        if (statusError) return Domain::Result<Domain::ManagedRunSnapshot>::failure(*statusError);
        const auto found = records.find(id);
        if (found == records.end()) return Domain::Result<Domain::ManagedRunSnapshot>::failure(Domain::makeError(Domain::ErrorCodes::SessionNotFound, "No durable run"));
        return Domain::Result<Domain::ManagedRunSnapshot>::success({found->second, true, false, false});
    }
    Domain::Result<Domain::ManagedRunSnapshot> cancel(const Domain::SessionId& id, const Domain::OperationContext& context) noexcept override {
        { std::lock_guard lock{mutex}; const auto found = records.find(id); if (found != records.end() && found->second.state == Domain::ManagedRunState::Running) found->second.state = Domain::ManagedRunState::Cancelled; }
        return status(id, context);
    }
    Domain::Result<Domain::ManagedRunSnapshot> pause(const Domain::SessionId& id, const Domain::OperationContext& context) noexcept override { return status(id, context); }
    Domain::Result<Domain::ManagedRunSnapshot> resume(const Domain::SessionId& id, const Domain::OperationContext& context) noexcept override { return status(id, context); }
    void shutdown() noexcept override {}
    std::size_t count() const { std::lock_guard lock{mutex}; return requests.size(); }
    std::size_t attempts() const { std::lock_guard lock{mutex}; return attempted; }
    Domain::ManagedRunStartRequest last() const { std::lock_guard lock{mutex}; return requests.back(); }
    void finish(bool interrupted = false, std::string output = "actual completed output") {
        std::lock_guard lock{mutex}; auto& record = records.at(requests.back().runId);
        record.state = interrupted ? Domain::ManagedRunState::Failed : Domain::ManagedRunState::Completed;
        record.workerInterrupted = interrupted; record.outputText = interrupted ? "uncertain effects" : std::move(output);
        record.inputTokens = 10U; record.outputTokens = 5U;
        if (interrupted) record.lastError = Domain::makeError(Domain::ErrorCodes::Conflict, "Interrupted with uncertain effects");
    }
private:
    mutable std::mutex mutex;
    std::size_t attempted{};
    std::vector<Domain::ManagedRunStartRequest> requests;
    std::map<Domain::SessionId, Domain::ManagedRunRecord> records;
};
class Fixture final {
public:
    Clock clock; Uuid uuid; Authority authority; Store store; Catalog catalog; Runs runs;
    std::vector<std::string> events;
    bool failNotifications{};
    bool failNotificationPersistence{};
    std::unique_ptr<Service> service;
    Fixture() { service = make(); require(service->initialize(context()).hasValue(), "Initialize failed."); }
    std::unique_ptr<Service> make() {
        return std::make_unique<Service>(runs, authority, store, clock, uuid, catalog,
            [&](const Domain::OperationContext&) { return Domain::Result<Application::ScheduleStoragePaths>::success({authority.storage(Domain::FileAccess::Read), authority.storage(Domain::FileAccess::Write), authority.storage(Domain::FileAccess::Create)}); },
            [&](std::string_view value, const Domain::OperationContext&) {
                events.emplace_back(value);
                if (failNotificationPersistence) store.failWrites = true;
                if (failNotifications) return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable, "Simulated Windows notification block"));
                return Domain::Result<std::string>::success(Json{{"channel", "test"}, {"submission_accepted", true}, {"display_confirmed", false}}.dump());
            });
    }
    static Domain::OperationContext context() { return TestContext{}.active(); }
    Contracts::WorkspaceAuthority caller() { return take(authority.authorityFor(authority.project, context())); }
    Json execute(std::string_view name, const Json& input, Service* target = nullptr) { return Json::parse(take((target ? target : service.get())->execute(name, input.dump(), caller(), context()))); }
    Json create(std::string owner = "authorized user task") {
        const auto created = execute("schedule_create", Json{{"name", "Check project"}, {"task", "Inspect changed files and report the actual findings."}, {"owner_reference", owner},
            {"authorization", "User explicitly requested this recurring project check."}, {"interval_sec", 60}}).at("schedule");
        return record(created.at("schedule_id").get<std::string>());
    }
    Json record(const std::string& id, Service* target = nullptr, std::vector<std::string>* receipts = nullptr) {
        std::string assembled; Json input{{"schedule_id", id}};
        while (true) {
            const auto result = execute("schedule_list", input, target);
            if (receipts) receipts->push_back(result.dump());
            require(result.at("offset").get<std::size_t>() == assembled.size() && Domain::isValidUtf8(result.at("record_json_page").get<std::string>()), "Invalid record page boundary.");
            assembled += result.at("record_json_page").get<std::string>();
            if (result.at("eof").get<bool>()) { require(assembled.size() == result.at("total_bytes"), "Record pages lost bytes."); break; }
            input["offset"] = result.at("next_offset"); input["revision"] = result.at("revision");
        }
        return Json::parse(assembled);
    }
    Json list(Service* target = nullptr) {
        auto result = Json::array();
        const auto listed = execute("schedule_list", Json::object(), target);
        for (const auto& entry : listed.at("schedules"))
            result.push_back(record(entry.at("schedule_id").get<std::string>(), target));
        return result;
    }
};
// Exercise the actual MCP serializer with receipts produced by this service,
// rather than estimating the escaping and duplicated-content wire overhead.
class PayloadRouter final : public Contracts::IToolRouter {
public:
    explicit PayloadRouter(const std::vector<std::string>& payloads) : payloads_{payloads} {}
    Domain::Result<Domain::ToolCallOutcome> invoke(const Domain::ToolCallRequest& request,
        const Contracts::WorkspaceAuthority&, const Domain::OperationContext&) noexcept override {
        try {
            const auto index = Json::parse(request.canonicalArguments).at("offset").get<std::size_t>();
            return Domain::Result<Domain::ToolCallOutcome>::success({
                {request.metadata.requestId, request.toolName, true, {}, std::chrono::milliseconds{1}}, payloads_.at(index)});
        } catch (...) { return Domain::Result<Domain::ToolCallOutcome>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Receipt lookup failed.")); }
    }
    void cancel(const Domain::OperationId&) noexcept override {}
    void shutdown() noexcept override {}
private:
    const std::vector<std::string>& payloads_;
};
class CallerResolver final : public Contracts::IMcpExecutionContextResolver {
public:
    explicit CallerResolver(Authority& authority) : authority_{authority} {}
    Domain::Result<Contracts::WorkspaceAuthority> resolve(const Domain::ToolCallRequest&, Domain::ToolEffect,
        const Domain::OperationContext& context) noexcept override { return authority_.authorityFor(authority_.project, context); }
private:
    Authority& authority_;
};
class ReceiptTransport final : public Contracts::IMcpTransport {
public:
    explicit ReceiptTransport(std::size_t count) : expected_{count + 1U} {
        inbound_.push_back(Json{{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {{"protocolVersion", "2025-11-25"}}}}.dump());
        inbound_.push_back(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump());
        for (std::size_t index = 0U; index < count; ++index)
            inbound_.push_back(Json{{"jsonrpc", "2.0"}, {"id", index + 1U}, {"method", "tools/call"},
                {"params", {{"name", "schedule_list"}, {"arguments", {{"schedule_id", "30000000-0000-4000-8000-000000000001"}, {"offset", index}, {"revision", "1"}}}}}}.dump());
    }
    Domain::Result<std::optional<Domain::McpFrame>> receive(const Domain::OperationContext&) noexcept override {
        std::unique_lock lock{mutex_};
        if (next_ < inbound_.size()) return Domain::Result<std::optional<Domain::McpFrame>>::success(Domain::McpFrame{inbound_[next_++]});
        if (!changed_.wait_for(lock, std::chrono::seconds{20}, [&] { return stopped_ || outbound_.size() == expected_; }))
            return Domain::Result<std::optional<Domain::McpFrame>>::failure(Domain::makeError(Domain::ErrorCodes::DeadlineExceeded, "Receipt transport timed out."));
        return Domain::Result<std::optional<Domain::McpFrame>>::success(std::nullopt);
    }
    Domain::Result<void> send(const Domain::McpFrame& value, const Domain::OperationContext&) noexcept override {
        std::lock_guard lock{mutex_}; outbound_.push_back(value.utf8Json); changed_.notify_all(); return Domain::Result<void>::success();
    }
    void shutdown() noexcept override { std::lock_guard lock{mutex_}; stopped_ = true; changed_.notify_all(); }
    std::vector<std::string> outbound() const { std::lock_guard lock{mutex_}; return outbound_; }
private:
    mutable std::mutex mutex_; std::condition_variable changed_;
    std::vector<std::string> inbound_, outbound_; std::size_t expected_, next_{}; bool stopped_{};
};
void requireWireRoundtrip(Fixture& fixture, const std::vector<std::string>& payloads) {
    auto catalog = take(Mcp::McpToolCatalog::create()); PayloadRouter router{payloads}; CallerResolver resolver{fixture.authority};
    ReceiptTransport transport{payloads.size()}; Mcp::McpServer server{*catalog, router, resolver, fixture.uuid, fixture.clock, "Scheduler receipt wire test"};
    require(server.run(transport, Domain::McpRole::Primary, parse<Domain::DeploymentId>("scheduler-wire-test"), fixture.authority.client,
        Fixture::context()).hasValue(), "Actual MCP scheduler receipt session failed.");
    const auto frames = transport.outbound(); require(frames.size() == payloads.size() + 1U, "MCP scheduler receipts were lost.");
    for (const auto& frame : frames) {
        require(frame.size() <= Mcp::McpJsonCodec::MaximumDocumentBytes, "A schedule receipt exceeded the actual MCP wire bound.");
        const auto encoded = Json::parse(frame); const auto id = encoded.at("id").get<std::size_t>(); if (id == 0U) continue;
        const auto& result = encoded.at("result"); require(result.at("isError") == false, "MCP rejected a bounded scheduler receipt.");
        require(result.at("structuredContent") == Json::parse(payloads.at(id - 1U)), "MCP changed a schedule receipt.");
        std::string assembled;
        for (const auto& block : result.at("content")) {
            const auto text = block.at("text").get<std::string>(); require(text.size() <= 32768U, "A scheduler text block exceeded LM Studio's supported bound.");
            const auto fragment = Json::parse(text);
            assembled += fragment.contains("kind") ? fragment.at("part").get<std::string>() : text;
        }
        require(Json::parse(assembled) == result.at("structuredContent"), "MCP text fragments lost scheduler receipt bytes.");
    }
}
void dueNoOverlapScopeAndRealResults() {
    Fixture fixture; const auto created = fixture.create();
    require(created.at("state") == "scheduled" && fixture.runs.count() == 0U, "Creation falsely dispatched or completed a task.");
    require(fixture.store.lastPath == "C:\\owner-home\\forge\\schedules.json", "Scheduler used a project-selected storage path.");
    fixture.authority.grants.push_back(Domain::FileAccess::Execute); fixture.authority.roots.push_back(take(Domain::PathText::create("C:\\extra")));
    fixture.authority.shell = true; fixture.authority.generation = 2U;
    fixture.clock.advance(std::chrono::seconds{60}); require(fixture.service->tick(Fixture::context()).hasValue(), "Due schedule tick failed.");
    const auto request = fixture.runs.last();
    require(fixture.runs.count() == 1U && request.authorityGeneration == 2U && request.readOnlyTools && request.workerScope &&
        request.workerScope->trustedRoots.size() == 1U && request.workerScope->grants.size() == 3U && !request.workerScope->shellEnabled &&
        request.workerScope->allowedTools == std::vector<std::string>{"fs_read"}, "A scheduled fire broadened its frozen authority or tool scope.");
    fixture.clock.advance(std::chrono::seconds{180}); require(fixture.service->tick(Fixture::context()).hasValue(), "Active schedule tick failed.");
    require(fixture.runs.count() == 1U, "A recurring task overlapped itself.");
    const auto eventCount = fixture.events.size(); require(fixture.service->tick(Fixture::context()).hasValue(), "Repeated status tick failed.");
    require(fixture.events.size() == eventCount, "An unchanged active run emitted a notification.");
    fixture.runs.finish(); require(fixture.service->tick(Fixture::context()).hasValue(), "Terminal observation tick failed.");
    require(fixture.runs.count() == 2U, "A completed interval task did not admit the next due run.");
    require(fixture.runs.last().operationId != request.operationId && fixture.runs.last().runId != request.runId,
        "Distinct scheduled runs shared a provider run or cancellation operation identity.");
    const auto listed = fixture.list().front();
    require(listed.at("history").front().at("state") == "completed" && listed.at("history").front().at("output") == "actual completed output" &&
        listed.at("history").front().at("output_tokens") == 5U, "Schedule history invented or dropped the actual managed result.");
}
void restoreFutureCancelAndCrossOwner() {
    Fixture fixture; const auto created = fixture.create();
    fixture.service.reset(); fixture.service = fixture.make(); require(fixture.service->initialize(Fixture::context()).hasValue(), "Future schedule did not restore.");
    fixture.clock.advance(std::chrono::seconds{60}); require(fixture.service->tick(Fixture::context()).hasValue(), "Restored schedule did not fire.");
    require(fixture.runs.count() == 1U, "Restored schedule fired the wrong number of runs.");
    const auto canceled = fixture.execute("schedule_cancel", Json{{"schedule_id", created.at("schedule_id")}, {"authorization", "User cancelled this recurring check."}}).at("schedule");
    require(canceled.at("state") == "cancelled" && canceled.at("latest_run").at("state") == "cancelled", "Schedule cancellation did not report the actual run state.");
    fixture.clock.advance(std::chrono::hours{1}); require(fixture.service->tick(Fixture::context()).hasValue(), "Canceled tick failed.");
    require(fixture.runs.count() == 1U, "A cancelled schedule dispatched another run.");
    fixture.authority.client = parse<Domain::ClientId>("different-owner");
    require(fixture.list().empty(), "A different caller saw another owner's schedule.");
    requireError(fixture.service->execute("schedule_cancel", Json{{"schedule_id", created.at("schedule_id")}, {"authorization", "different owner"}}.dump(),
        fixture.caller(), Fixture::context()), Domain::ErrorCodes::RecordNotFound, "A different owner canceled the schedule.");
}
void interruptedRecoveryRequiresExplicitAuthorization() {
    Fixture fixture; const auto created = fixture.create(); fixture.clock.advance(std::chrono::seconds{60});
    require(fixture.service->tick(Fixture::context()).hasValue(), "Initial dispatch failed."); fixture.runs.finish(true);
    auto restored = fixture.make(); require(restored->initialize(Fixture::context()).hasValue(), "Interrupted schedule restore failed.");
    fixture.clock.advance(std::chrono::hours{1}); require(restored->tick(Fixture::context()).hasValue(), "Interrupted observation failed.");
    require(fixture.runs.count() == 1U && fixture.list(restored.get()).front().at("needs_attention") == true &&
        fixture.list(restored.get()).front().at("latest_run").at("interrupted") == true, "Interrupted effects were replayed or hidden.");
    const auto result = fixture.execute("schedule_run_now", Json{{"schedule_id", created.at("schedule_id")}, {"authorization", "User explicitly authorized another attempt after inspecting uncertain effects."}}, restored.get());
    require(fixture.runs.count() == 2U && result.at("schedule").at("needs_attention") == false, "Explicit retry authorization did not start a fresh run.");
}
void persistenceFailuresNeverDispatch() {
    Fixture fixture; fixture.store.failWrites = true;
    const Json input{{"name", "Check"}, {"task", "Inspect files"}, {"owner_reference", "owned task"}, {"authorization", "explicit user request"}, {"interval_sec", 60}};
    requireError(fixture.service->execute("schedule_create", input.dump(), fixture.caller(), Fixture::context()), Domain::ErrorCodes::Conflict, "Failed create persistence was accepted.");
    require(fixture.list().empty() && fixture.runs.count() == 0U, "Failed persistence admitted a schedule or run.");
    fixture.store.failWrites = false; const auto created = fixture.create(); fixture.clock.advance(std::chrono::seconds{60});
    fixture.store.failWrites = true; fixture.store.ambiguousWrites = true;
    require(!fixture.service->tick(Fixture::context()), "Ambiguous admission save was accepted.");
    require(fixture.runs.count() == 0U, "Inference began before durable admission succeeded.");
    fixture.store.failWrites = false; fixture.store.ambiguousWrites = false;
    auto restored = fixture.make(); require(restored->initialize(Fixture::context()).hasValue(), "Ambiguous store restore failed.");
    require(restored->tick(Fixture::context()).hasValue() && fixture.runs.count() == 0U && fixture.list(restored.get()).front().at("needs_attention") == true,
        "An ambiguous persisted run was resubmitted automatically.");
    static_cast<void>(fixture.execute("schedule_run_now", Json{{"schedule_id", created.at("schedule_id")}, {"authorization", "User authorized new dispatch after reviewing the missing run."}}, restored.get()));
    require(fixture.runs.count() == 1U, "Authorized recovery did not dispatch exactly once.");
}
void refusedWorkerAdmissionRequiresExplicitRecoveryWithoutRestart() {
    Fixture fixture; const auto created = fixture.create(); fixture.clock.advance(std::chrono::seconds{60});
    fixture.runs.startError = Domain::makeError(Domain::ErrorCodes::LimitExceeded, "Independent worker capacity is sixteen.", true);
    require(fixture.service->tick(Fixture::context()).hasValue(), "A refused admission was not recorded.");
    fixture.runs.statusError = Domain::makeError(Domain::ErrorCodes::TransportClosed, "The worker status is temporarily unavailable.");
    const auto refused = fixture.list().front();
    require(fixture.runs.attempts() == 1U && fixture.runs.count() == 0U && refused.at("needs_attention") == true &&
        refused.at("latest_run").at("state") == "admitted" && refused.at("last_error").at("code").get<std::string>() == Domain::ErrorCodes::LimitExceeded,
        "A capacity refusal was hidden or falsely recorded as a dispatched worker.");
    const Json retry{{"schedule_id", created.at("schedule_id")}, {"authorization", "User inspected the refused admission and explicitly authorized another attempt."}};
    requireError(fixture.service->execute("schedule_run_now", retry.dump(), fixture.caller(), Fixture::context()),
        Domain::ErrorCodes::Conflict, "An unavailable worker status authorized duplicate unconfirmed work.");
    require(fixture.runs.attempts() == 1U && fixture.list().front().at("latest_run").at("interrupted") == false,
        "A transient status failure was misclassified as a missing durable run.");
    fixture.runs.statusError.reset(); fixture.runs.startError.reset();
    require(fixture.service->tick(Fixture::context()).hasValue(), "Missing-run reconciliation failed.");
    const auto missing = fixture.list().front();
    require(missing.at("latest_run").at("state") == "missing" && missing.at("latest_run").at("interrupted") == true &&
        missing.at("latest_run").at("error").at("code").get<std::string>() == Domain::ErrorCodes::SessionNotFound &&
        missing.at("latest_run").at("previous_error").at("code").get<std::string>() == Domain::ErrorCodes::LimitExceeded,
        "Missing-run reconciliation lost the actual admission error or inferred completion.");
    const auto events = fixture.events.size(); fixture.clock.advance(std::chrono::hours{1});
    require(fixture.service->tick(Fixture::context()).hasValue() && fixture.runs.attempts() == 1U && fixture.events.size() == events,
        "A missing admitted run was automatically replayed or emitted unchanged notifications.");
    static_cast<void>(fixture.execute("schedule_run_now", retry));
    const auto restarted = fixture.record(created.at("schedule_id").get<std::string>());
    require(fixture.runs.count() == 1U && fixture.runs.attempts() == 2U && restarted.at("needs_attention") == false &&
        restarted.at("latest_run").at("state") == "running" && restarted.at("latest_run").at("run_id") != missing.at("latest_run").at("run_id") &&
        restarted.at("history").back().at("state") == "missing" && restarted.at("history").back().at("previous_error").at("code").get<std::string>() == Domain::ErrorCodes::LimitExceeded,
        "Explicit recovery did not retain the uncertain prior attempt and dispatch exactly one fresh run without a Manager restart.");
    requireError(fixture.service->execute("schedule_run_now", retry.dump(), fixture.caller(), Fixture::context()),
        Domain::ErrorCodes::Conflict, "A known active worker was duplicated by another manual invocation.");
    require(fixture.runs.attempts() == 2U, "An active worker admitted a second manual attempt.");
}
void invalidInputAndCurrentPolicyFailClosed() {
    Fixture fixture;
    Json input{{"name", "Check"}, {"task", "Inspect"}, {"owner_reference", "task"}, {"authorization", "explicit request"}, {"interval_sec", 59}};
    requireError(fixture.service->execute("schedule_create", input.dump(), fixture.caller(), Fixture::context()), Domain::ErrorCodes::InvalidRequest, "An interval below 60 seconds was accepted.");
    input.erase("interval_sec"); input["at_time"] = "2026-02-30T00:00:00Z";
    requireError(fixture.service->execute("schedule_create", input.dump(), fixture.caller(), Fixture::context()), Domain::ErrorCodes::InvalidRequest, "An invalid calendar time was accepted.");
    input.erase("at_time"); input["interval_sec"] = 60; input["allowed_tools"] = Json::array({"agent_spawn"});
    requireError(fixture.service->execute("schedule_create", input.dump(), fixture.caller(), Fixture::context()), Domain::ErrorCodes::Unauthorized, "Recursive worker spawning was allowed.");
    input["allowed_tools"] = Json::array({"fs_write"});
    requireError(fixture.service->execute("schedule_create", input.dump(), fixture.caller(), Fixture::context()), Domain::ErrorCodes::Unauthorized, "A read-only scheduled worker received a write tool.");
    input.erase("allowed_tools");
    for (const auto& timeout : Json::array({0, 3601, -1, -4294967295LL, 600.5, "600", nullptr})) {
        input["timeout_sec"] = timeout;
        requireError(fixture.service->execute("schedule_create", input.dump(), fixture.caller(), Fixture::context()), Domain::ErrorCodes::InvalidRequest,
            "A noninteger, wrapped, or out-of-range timeout was accepted.");
    }
    input.erase("timeout_sec"); input["authorization"] = "";
    requireError(fixture.service->execute("schedule_create", input.dump(), fixture.caller(), Fixture::context()), Domain::ErrorCodes::InvalidRequest, "An empty authorization reference was accepted.");
    input["authorization"] = "explicit request"; auto cancelled = Fixture::context(); std::stop_source source; source.request_stop(); cancelled.cancellation = source.get_token();
    requireError(fixture.service->execute("schedule_create", input.dump(), fixture.caller(), cancelled), Domain::ErrorCodes::Cancelled, "Cancelled schedule creation was accepted.");
    const auto created = fixture.create(); fixture.authority.roots = {take(Domain::PathText::create("C:\\different"))}; ++fixture.authority.generation;
    fixture.clock.advance(std::chrono::seconds{60}); require(fixture.service->tick(Fixture::context()).hasValue(), "Policy removal observation failed.");
    require(fixture.runs.count() == 0U && fixture.list().front().at("needs_attention") == true, "Removed frozen roots were replaced with new privileges.");
    static_cast<void>(created);
}
void managerOwnedLoopActuallyDispatchesAndStops() {
    Fixture fixture; static_cast<void>(fixture.create()); fixture.clock.advance(std::chrono::seconds{60});
    require(fixture.service->start().hasValue(), "The Manager-owned scheduler loop did not start.");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (fixture.runs.count() == 0U && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds{10});
    require(fixture.runs.count() == 1U, "The autonomous scheduler loop did not dispatch a due model run.");
    fixture.service->shutdown();
    const auto stored = Json::parse(reinterpret_cast<const char*>(fixture.store.bytes.data()),
        reinterpret_cast<const char*>(fixture.store.bytes.data()) + fixture.store.bytes.size());
    require(stored.at("schedules").front().at("latest_run").at("state") == "cancelled", "Scheduler shutdown did not persist actual run cancellation.");
    requireError(fixture.service->tick(Fixture::context()), Domain::ErrorCodes::HostCapabilityUnavailable, "A stopped scheduler accepted another tick.");
}
void managerNarrowedCallerRetainsScheduleAccess() {
    Fixture fixture; const auto baseline = fixture.caller();
    const auto narrowed = take(fixture.authority.narrow(baseline, baseline.trustedRoots(), baseline.grants(), false, baseline.generation() + 1U, Fixture::context()));
    const Json input{{"name", "Broker task"}, {"task", "Inspect project"}, {"owner_reference", "broker authorized task"},
        {"authorization", "User requested the brokered task."}, {"interval_sec", 60}};
    require(fixture.service->execute("schedule_create", input.dump(), narrowed, Fixture::context()).hasValue(),
        "A valid Manager-narrowed caller generation could not create a schedule.");
    const auto readOnly = take(fixture.authority.narrow(baseline, baseline.trustedRoots(), {Domain::FileAccess::Read}, false, baseline.generation() + 1U, Fixture::context()));
    require(fixture.service->execute("schedule_list", "{}", readOnly, Fixture::context()).hasValue(),
        "A valid read-only Manager-narrowed caller could not list schedules.");
}
void fullWorkerRootScopeSurvivesPersistence() {
    Fixture fixture;
    for (std::size_t index = 1U; index < 64U; ++index)
        fixture.authority.roots.push_back(take(Domain::PathText::create("D:\\workspace" + std::to_string(index))));
    static_cast<void>(fixture.create());
    fixture.service.reset(); fixture.service = fixture.make();
    require(fixture.service->initialize(Fixture::context()).hasValue(), "A valid 64-root worker scope did not restore.");
    fixture.clock.advance(std::chrono::seconds{60});
    require(fixture.service->tick(Fixture::context()).hasValue() && fixture.runs.count() == 1U &&
        fixture.runs.last().workerScope && fixture.runs.last().workerScope->trustedRoots.size() == 64U,
        "A restored schedule dropped or rejected a valid worker root scope.");
}
void actualNotificationReceiptsPersistWithoutChangingRuns() {
    Fixture fixture;
    const auto created = fixture.create();
    require(created.at("last_notification").at("receipt").at("submission_accepted") == true &&
        created.at("last_notification").at("receipt").at("display_confirmed") == false,
        "The schedule receipt hid or overstated the actual notification submission result.");
    fixture.service.reset(); fixture.service = fixture.make();
    require(fixture.service->initialize(Fixture::context()).hasValue() &&
        fixture.list().front().at("last_notification").at("receipt").at("display_confirmed") == false,
        "The actual notification receipt did not survive restart.");
    fixture.failNotifications = true; fixture.clock.advance(std::chrono::seconds{60});
    require(fixture.service->tick(Fixture::context()).hasValue() && fixture.runs.count() == 1U,
        "A blocked notification changed the scheduled provider dispatch.");
    const auto listed = fixture.list().front();
    require(listed.at("latest_run").at("state") == "running" && listed.at("needs_attention") == false &&
        listed.at("last_notification").at("receipt").is_null() &&
        listed.at("last_notification").at("error").at("code").get<std::string>() == Domain::ErrorCodes::HostCapabilityUnavailable,
        "Notification failure was hidden or mislabeled as a provider failure.");
}
void boundedSummariesAndRecordPagesPreserveLargeTasksOnActualWire() {
    Fixture fixture; fixture.authority.roots.clear();
    for (std::size_t index = 0U; index < 64U; ++index)
        fixture.authority.roots.push_back(take(Domain::PathText::create("D:\\" + std::to_string(index) + std::string(1024U, 'r'))));
    std::string task;
    const std::string pattern = "\\\"\n\xf0\x9f\x98\x80";
    while (task.size() + pattern.size() <= Service::MaximumTaskBytes) task += pattern;
    task.append(Service::MaximumTaskBytes - task.size(), '\\');
    std::vector<std::string> receipts; std::string selected;
    for (std::size_t index = 0U; index < Service::MaximumSchedules; ++index) {
        const auto result = fixture.execute("schedule_create", Json{{"name", std::string(128U, '\x01')}, {"task", task},
            {"owner_reference", "authorized task " + std::to_string(index)}, {"authorization", "User requested this bounded recurring task."}, {"interval_sec", 60}});
        receipts.push_back(result.dump()); const auto& item = result.at("schedule");
        require(item.at("summary") == true && !item.contains("task") && item.at("task_total_bytes") == task.size() &&
            item.at("scope").at("root_count") == 64U, "A mutation returned full task/scope payload or lost its size metadata.");
        if (selected.empty()) selected = item.at("schedule_id").get<std::string>();
    }
    const auto listed = fixture.execute("schedule_list", Json::object()); receipts.push_back(listed.dump());
    require(listed.at("count") == Service::MaximumSchedules && listed.at("summaries") == true &&
        listed.dump().size() <= Service::MaximumResponseBytes, "Many long tasks overflowed or were omitted from the summary list.");
    for (unsigned attempt = 0U; attempt < 5U; ++attempt) {
        receipts.push_back(fixture.execute("schedule_run_now", Json{{"schedule_id", selected}, {"authorization", "User explicitly requested another completed attempt."}}).dump());
        fixture.runs.finish(false, std::string(8192U, '\x01'));
        static_cast<void>(fixture.execute("schedule_list", Json{{"schedule_id", selected}}));
    }
    const auto full = fixture.record(selected, nullptr, &receipts);
    require(full.at("task").get<std::string>() == task && full.at("scope").at("roots").size() == 64U &&
        full.at("scope").at("roots").back().get<std::string>() == fixture.authority.roots.back().value() && full.at("history").size() == 5U &&
        full.at("history").front().at("output").get<std::string>() == std::string(8192U, '\x01') && full.at("logs").size() > 0U,
        "Paged reconstruction dropped full task, frozen roots, escaped history, or logs.");
    receipts.push_back(fixture.execute("schedule_cancel", Json{{"schedule_id", selected}, {"authorization", "User cancelled after inspecting the full record."}}).dump());
    for (const auto& receipt : receipts) require(receipt.size() <= Service::MaximumResponseBytes, "A schedule response exceeded its owner boundary.");
    requireWireRoundtrip(fixture, receipts);
    fixture.authority.client = parse<Domain::ClientId>("different-owner");
    require(fixture.execute("schedule_list", Json::object()).at("count") == 0U, "Summary list exposed another caller's records.");
    requireError(fixture.service->execute("schedule_list", Json{{"schedule_id", selected}}.dump(), fixture.caller(), Fixture::context()),
        Domain::ErrorCodes::RecordNotFound, "Full record paging exposed another caller's task or history.");
}
void recordPagingValidationRevisionAndLegacyStore() {
    Fixture fixture; const auto created = fixture.create(); const auto id = created.at("schedule_id").get<std::string>();
    const auto first = fixture.execute("schedule_list", Json{{"schedule_id", id}, {"max_bytes", 17}});
    const auto revision = first.at("revision");
    const auto unchanged = fixture.execute("schedule_list", Json{{"schedule_id", id}, {"offset", first.at("next_offset")}, {"revision", revision}, {"max_bytes", 17}});
    require(unchanged.at("revision") == revision && unchanged.at("offset") == first.at("next_offset"), "An unchanged read advanced the record revision.");
    static_cast<void>(fixture.create("independent task"));
    require(fixture.execute("schedule_list", Json{{"schedule_id", id}, {"offset", first.at("next_offset")}, {"revision", revision}}).at("revision") == revision,
        "Another schedule's mutation invalidated this record's revision.");
    for (const auto& invalid : std::vector<Json>{{{"offset", 0}}, {{"max_bytes", 1}}, {{"revision", "1"}}, {{"schedule_id", id}, {"unknown", true}},
        {{"schedule_id", id}, {"max_bytes", 0}}, {{"schedule_id", id}, {"max_bytes", 32769}}, {{"schedule_id", id}, {"max_bytes", 1.5}},
        {{"schedule_id", id}, {"offset", -1}}, {{"schedule_id", id}, {"offset", first.at("next_offset")}}, {{"schedule_id", id}, {"offset", 8388608}, {"revision", revision}},
        {{"schedule_id", id}, {"revision", "01"}}, {{"schedule_id", id}, {"revision", "1.0"}}})
        requireError(fixture.service->execute("schedule_list", invalid.dump(), fixture.caller(), Fixture::context()), Domain::ErrorCodes::InvalidRequest, "Invalid paging arguments were accepted.");
    const auto unicode = fixture.execute("schedule_create", Json{{"name", "Unicode"}, {"task", "Before\xf0\x9f\x98\x80 after"}, {"owner_reference", "unicode task"},
        {"authorization", "User requested Unicode byte boundary validation."}, {"interval_sec", 60}}).at("schedule");
    const auto unicodeId = unicode.at("schedule_id").get<std::string>(); const auto unicodeRecord = fixture.record(unicodeId);
    const auto emoji = unicodeRecord.dump().find("\xf0\x9f\x98\x80"); require(emoji != std::string::npos, "Unicode fixture did not retain its actual UTF-8.");
    for (const auto offset : {emoji, emoji + 1U})
        requireError(fixture.service->execute("schedule_list", Json{{"schedule_id", unicodeId}, {"offset", offset}, {"max_bytes", 1}, {"revision", unicode.at("revision")}}.dump(),
            fixture.caller(), Fixture::context()), Domain::ErrorCodes::InvalidRequest, "A page split a UTF-8 codepoint or accepted an interior byte offset.");
    static_cast<void>(fixture.execute("schedule_run_now", Json{{"schedule_id", id}, {"authorization", "User authorized this change while another reader paged."}}));
    requireError(fixture.service->execute("schedule_list", Json{{"schedule_id", id}, {"offset", first.at("next_offset")}, {"revision", revision}}.dump(),
        fixture.caller(), Fixture::context()), Domain::ErrorCodes::Conflict, "Record mutation let a reader combine mismatched revisions.");
    const auto latest = fixture.record(id); fixture.service.reset(); fixture.service = fixture.make();
    require(fixture.service->initialize(Fixture::context()).hasValue() && fixture.record(unicodeId).at("revision") == unicodeRecord.at("revision"),
        "An unchanged persisted record did not preserve its continuation revision across restart.");
    fixture.service.reset(); fixture.service = fixture.make();
    // A legacy schema1 store without revision fields remains readable and
    // advances from zero when its record next changes.
    auto stored = Json::parse(reinterpret_cast<const char*>(fixture.store.bytes.data()), reinterpret_cast<const char*>(fixture.store.bytes.data()) + fixture.store.bytes.size());
    for (auto& item : stored.at("schedules")) item.erase("revision");
    const auto bytes = stored.dump(); fixture.store.bytes.assign(reinterpret_cast<const std::byte*>(bytes.data()), reinterpret_cast<const std::byte*>(bytes.data()) + bytes.size());
    require(fixture.service->initialize(Fixture::context()).hasValue(), "A legacy schedule store without revisions failed to restore.");
    const auto legacy = fixture.record(stored.at("schedules").back().at("schedule_id").get<std::string>());
    require(legacy.at("revision") == "0" && legacy.at("task") == stored.at("schedules").back().at("task"), "Legacy record paging changed its task or initial revision.");
    static_cast<void>(latest);
    auto catalog = take(Mcp::McpToolCatalog::create());
    const auto descriptor = std::find_if(catalog->tools().begin(), catalog->tools().end(), [](const auto& item) { return item.tool.name == "schedule_list"; });
    require(descriptor != catalog->tools().end(), "The schedule_list descriptor is missing.");
    const auto schema = Json::parse(descriptor->inputSchema);
    require(schema.at("additionalProperties") == false && schema.at("properties").size() == 4U &&
        schema.at("properties").at("max_bytes").at("maximum") == 32768, "The additive schedule paging schema is not closed or bounded.");
}
void unsavedNotificationNeverIssuesARecordRevision() {
    Fixture fixture; const auto original = fixture.create(); const auto id = original.at("schedule_id").get<std::string>();
    fixture.failNotificationPersistence = true;
    const auto canceled = fixture.execute("schedule_cancel", Json{{"schedule_id", id}, {"authorization", "User cancelled and kept actual notification diagnostics."}}).at("schedule");
    require(canceled.at("cancelled") == true && canceled.at("revision").is_null() && canceled.at("record_paging_available") == false &&
        canceled.at("last_notification").at("submission_accepted") == true && canceled.at("last_notification").at("persistence_error").at("code").get<std::string>() == Domain::ErrorCodes::Conflict,
        "An unsaved notification hid its actual receipt or issued a reusable nondurable page token.");
    const auto listed = fixture.execute("schedule_list", Json::object()).at("schedules").front();
    require(listed.at("revision").is_null() && listed.at("record_paging_available") == false, "A summary issued a token for unsaved fields.");
    for (const auto offset : {0U, 1U})
        requireError(fixture.service->execute("schedule_list", Json{{"schedule_id", id}, {"offset", offset}, {"revision", original.at("revision")}}.dump(),
            fixture.caller(), Fixture::context()), Domain::ErrorCodes::Conflict, "Unsaved record fields could be paged under a durable revision.");
    fixture.failNotificationPersistence = false; fixture.store.failWrites = false;
    static_cast<void>(fixture.create("separate authorized task"));
    const auto persisted = fixture.record(id);
    require(persisted.at("revision") != original.at("revision") && persisted.at("last_notification").at("receipt").at("submission_accepted") == true &&
        persisted.at("last_notification").at("persistence_error").at("code").get<std::string>() == Domain::ErrorCodes::Conflict,
        "A successful later commit failed to retain actual submission/error evidence under a fresh record revision.");
    fixture.service.reset(); fixture.service = fixture.make(); require(fixture.service->initialize(Fixture::context()).hasValue(), "Notification recovery did not restart.");
    require(fixture.record(id).at("revision") == persisted.at("revision"), "Committed notification recovery reused or lost its durable guard.");
}
} // namespace
} // namespace ForgeConductor::Tests

int main() {
    using namespace ForgeConductor::Tests;
    try {
        dueNoOverlapScopeAndRealResults(); std::cout << "PASS schedules.due_no_overlap_scope_actual_results\n";
        restoreFutureCancelAndCrossOwner(); std::cout << "PASS schedules.restore_cancel_owner\n";
        interruptedRecoveryRequiresExplicitAuthorization(); std::cout << "PASS schedules.interruption_authorized_retry\n";
        persistenceFailuresNeverDispatch(); std::cout << "PASS schedules.persistence_fail_closed\n";
        refusedWorkerAdmissionRequiresExplicitRecoveryWithoutRestart(); std::cout << "PASS schedules.missing_admission_manual_recovery\n";
        invalidInputAndCurrentPolicyFailClosed(); std::cout << "PASS schedules.validation_policy\n";
        managerOwnedLoopActuallyDispatchesAndStops(); std::cout << "PASS schedules.manager_owned_dispatch_shutdown\n";
        managerNarrowedCallerRetainsScheduleAccess(); std::cout << "PASS schedules.manager_narrowed_caller\n";
        fullWorkerRootScopeSurvivesPersistence(); std::cout << "PASS schedules.full_worker_scope_persistence\n";
        actualNotificationReceiptsPersistWithoutChangingRuns(); std::cout << "PASS schedules.actual_notification_receipts\n";
        boundedSummariesAndRecordPagesPreserveLargeTasksOnActualWire(); std::cout << "PASS schedules.bounded_summaries_full_pages_actual_mcp_wire\n";
        recordPagingValidationRevisionAndLegacyStore(); std::cout << "PASS schedules.record_page_validation_revision_legacy\n";
        unsavedNotificationNeverIssuesARecordRevision(); std::cout << "PASS schedules.unsaved_notification_no_revision_aba\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
