#include "ForgeConductor/Application/ScheduledTaskService.h"
#include "ForgeConductor/Application/ManagedRunWorkerPolicy.h"
#include "ForgeConductor/Domain/Utf8.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {
using Json = nlohmann::json;
struct Failure final { Domain::Error error; };
[[noreturn]] void fail(std::string_view code, std::string message) { throw Failure{Domain::makeError(code, std::move(message))}; }
template<typename T> T take(Domain::Result<T> result) {
    if (!result) throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void take(Domain::Result<void> result) { if (!result) throw Failure{std::move(result).error()}; }
void bounded(std::size_t size, std::size_t maximum, std::string_view label) {
    if (size > maximum) fail(Domain::ErrorCodes::PayloadTooLarge, std::string{label} + " exceeds its supported bound.");
}
void keys(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) fail(Domain::ErrorCodes::InvalidRequest, "Schedule arguments must be an object.");
    for (const auto& [key, member] : value.items()) {
        static_cast<void>(member);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) fail(Domain::ErrorCodes::InvalidRequest, "Unknown schedule property: " + key);
    }
}
std::string text(const Json& object, std::string_view key, std::size_t maximum) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_string()) fail(Domain::ErrorCodes::InvalidRequest, "Missing schedule text: " + std::string{key});
    auto value = found->get<std::string>();
    bounded(value.size(), maximum, key);
    if (value.empty() || value.find('\0') != std::string::npos || !Domain::isValidUtf8(value))
        fail(Domain::ErrorCodes::InvalidRequest, std::string{key} + " must be nonempty UTF-8 without NUL bytes.");
    return value;
}
std::int64_t milliseconds(Domain::UtcTimePoint value) { return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count(); }
std::string utc(std::int64_t value) {
    const std::chrono::sys_time<std::chrono::milliseconds> point{std::chrono::milliseconds{value}};
    const auto day = std::chrono::floor<std::chrono::days>(point);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss clock{point - day};
    char result[32]{};
    std::snprintf(result, sizeof(result), "%04d-%02u-%02uT%02lld:%02lld:%02lldZ", int(date.year()), unsigned(date.month()), unsigned(date.day()),
        static_cast<long long>(clock.hours().count()), static_cast<long long>(clock.minutes().count()), static_cast<long long>(clock.seconds().count()));
    return result;
}
std::int64_t parseUtc(std::string_view value) {
    if (value.size() != 20U || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':' || value[19] != 'Z')
        fail(Domain::ErrorCodes::InvalidRequest, "at_time must use UTC YYYY-MM-DDTHH:MM:SSZ.");
    const auto digits = [&](std::size_t begin, std::size_t count) {
        unsigned number{};
        for (std::size_t index = begin; index < begin + count; ++index) {
            if (value[index] < '0' || value[index] > '9') fail(Domain::ErrorCodes::InvalidRequest, "at_time contains invalid digits.");
            number = number * 10U + static_cast<unsigned>(value[index] - '0');
        }
        return number;
    };
    const auto year = digits(0U, 4U), hour = digits(11U, 2U), minute = digits(14U, 2U), second = digits(17U, 2U);
    const std::chrono::year_month_day date{std::chrono::year{static_cast<int>(year)}, std::chrono::month{digits(5U, 2U)}, std::chrono::day{digits(8U, 2U)}};
    if (!date.ok() || year < 1970U || year > 9999U || hour > 23U || minute > 59U || second > 59U)
        fail(Domain::ErrorCodes::InvalidRequest, "at_time is not a supported calendar time.");
    return milliseconds(std::chrono::sys_days{date} + std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second});
}
bool nonterminal(const Json& run) {
    if (run.is_null()) return false;
    const auto state = run.at("state").get<std::string>();
    return state == "admitted" || state == "running" || state == "paused" || state == "cancelling" || state == "missing";
}
std::string state(Domain::ManagedRunState value) {
    switch (value) {
    case Domain::ManagedRunState::Running: return "running";
    case Domain::ManagedRunState::Paused: return "paused";
    case Domain::ManagedRunState::Cancelling: return "cancelling";
    case Domain::ManagedRunState::Completed: return "completed";
    case Domain::ManagedRunState::Failed: return "failed";
    case Domain::ManagedRunState::Cancelled: return "cancelled";
    }
    return "unknown";
}
std::string page(std::string_view value, std::size_t maximum = 8192U) {
    auto end = (std::min)(value.size(), maximum);
    while (end < value.size() && end > 0U && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) --end;
    return std::string{value.substr(0U, end)};
}
Json errorJson(const Domain::Error& error) { return Json{{"code", error.code}, {"message", page(error.message, 2048U)}, {"retryable", error.retryable}}; }
Json scopeJson(const Domain::ManagedRunWorkerScope& scope) {
    Json roots = Json::array(), grants = Json::array(), denials = Json::array();
    for (const auto& root : scope.trustedRoots) roots.push_back(root.value());
    for (const auto grant : scope.grants) grants.push_back(static_cast<int>(grant));
    for (const auto denial : scope.denials) denials.push_back(static_cast<int>(denial));
    return Json{{"roots", roots}, {"grants", grants}, {"denials", denials}, {"shell", scope.shellEnabled}, {"allowed_tools", scope.allowedTools}, {"timeout_sec", scope.timeoutSeconds}};
}
Domain::ManagedRunWorkerScope scopeFrom(const Json& value) {
    Domain::ManagedRunWorkerScope result;
    for (const auto& root : value.at("roots")) result.trustedRoots.push_back(take(Domain::PathText::create(root.get<std::string>())));
    const auto access = [](const Json& raw) {
        const auto integer = raw.get<int>();
        const auto mode = static_cast<Domain::FileAccess>(integer);
        if (mode != Domain::FileAccess::Read && mode != Domain::FileAccess::Write && mode != Domain::FileAccess::Create && mode != Domain::FileAccess::Delete && mode != Domain::FileAccess::Execute)
            fail(Domain::ErrorCodes::IntegrityFailure, "The stored schedule has an invalid filesystem grant.");
        return mode;
    };
    for (const auto& grant : value.at("grants")) result.grants.push_back(access(grant));
    for (const auto& denial : value.at("denials")) result.denials.push_back(access(denial));
    result.shellEnabled = value.at("shell").get<bool>();
    result.allowedTools = value.at("allowed_tools").get<std::vector<std::string>>();
    const auto& rawTimeout = value.at("timeout_sec");
    if (!rawTimeout.is_number_integer() || rawTimeout.get<std::uint64_t>() < 1U || rawTimeout.get<std::uint64_t>() > 3600U)
        fail(Domain::ErrorCodes::IntegrityFailure, "The stored schedule timeout is invalid.");
    result.timeoutSeconds = rawTimeout.get<std::uint32_t>();
    bounded(result.trustedRoots.size(), 64U, "Schedule roots"); bounded(result.grants.size(), 5U, "Schedule grants"); bounded(result.denials.size(), 5U, "Schedule denials"); bounded(result.allowedTools.size(), 128U, "Schedule allowed tools");
    if (result.trustedRoots.empty() || result.grants.empty())
        fail(Domain::ErrorCodes::IntegrityFailure, "The stored schedule scope is incomplete.");
    for (const auto& tool : result.allowedTools) if (tool.empty() || tool.size() > 128U || !isManagedWorkerToolPermitted(tool))
        fail(Domain::ErrorCodes::Unauthorized, "The schedule contains an unsupported worker tool.");
    return result;
}
Json runJson(const Domain::ManagedRunSnapshot& value) {
    const auto& record = value.record;
    return Json{{"run_id", record.runId.value()}, {"state", state(record.state)}, {"updated_at", milliseconds(record.updatedAt)},
        {"interrupted", record.workerInterrupted}, {"output", record.outputText ? Json(page(*record.outputText)) : Json(nullptr)},
        {"output_total_bytes", record.outputText ? record.outputText->size() : 0U},
        {"output_truncated", record.outputTruncated || (record.outputText && record.outputText->size() > 8192U)},
        {"input_tokens", record.inputTokens}, {"output_tokens", record.outputTokens},
        {"error", record.lastError ? errorJson(*record.lastError) : Json(nullptr)}};
}
Json publicRecord(Json record) {
    record["created_at"] = utc(record.at("created_at").get<std::int64_t>());
    record["updated_at"] = utc(record.at("updated_at").get<std::int64_t>());
    record["next_at"] = record.at("next_at").is_null() ? Json(nullptr) : Json(utc(record.at("next_at").get<std::int64_t>()));
    record["state"] = record.at("cancelled").get<bool>() ? "cancelled" : record.at("needs_attention").get<bool>() ? "needs_attention" :
        nonterminal(record.at("latest_run")) ? "running" : record.at("next_at").is_null() ? "finished" : "scheduled";
    record["authorization_reference_is_human_proof"] = false;
    record["manager_owned"] = true;
    record["revision"] = std::to_string(record.value("revision", std::uint64_t{}));
    return record;
}
Json errorSummary(const Json& error) {
    return error.is_null() ? Json(nullptr) : Json{{"code", page(error.at("code").get<std::string>(), 64U)},
        {"retryable", error.at("retryable")}};
}
Json runSummary(const Json& run) {
    if (run.is_null()) return nullptr;
    return Json{{"run_id", run.at("run_id")}, {"state", run.at("state")}, {"updated_at", run.value("updated_at", Json(nullptr))},
        {"interrupted", run.value("interrupted", false)}, {"output_total_bytes", run.value("output_total_bytes", Json(0U))},
        {"output_truncated", run.value("output_truncated", false)}, {"input_tokens", run.value("input_tokens", Json(0U))},
        {"output_tokens", run.value("output_tokens", Json(0U))}, {"error", errorSummary(run.value("error", Json(nullptr)))}};
}
Json notificationSummary(const Json& value) {
    if (value.is_null()) return nullptr;
    const auto& receipt = value.at("receipt");
    const auto flag = [&](std::string_view name) -> Json {
        if (!receipt.is_object()) return nullptr;
        const auto found = receipt.find(std::string{name});
        return found != receipt.end() && found->is_boolean() ? *found : Json(nullptr);
    };
    return Json{{"event", value.at("event")}, {"at", value.at("at")},
        {"submission_accepted", flag("submission_accepted")}, {"display_confirmed", flag("display_confirmed")},
        {"error", errorSummary(value.at("error"))}, {"persistence_error", errorSummary(value.value("persistence_error", Json(nullptr)))}};
}
Json summary(const Json& record, bool pagingAvailable) {
    const auto& scope = record.at("scope");
    return Json{{"schedule_id", record.at("schedule_id")}, {"name", record.at("name")},
        {"revision", pagingAvailable ? Json(std::to_string(record.value("revision", std::uint64_t{}))) : Json(nullptr)},
        {"record_paging_available", pagingAvailable}, {"summary", true},
        {"state", record.at("cancelled").get<bool>() ? "cancelled" : record.at("needs_attention").get<bool>() ? "needs_attention" :
            nonterminal(record.at("latest_run")) ? "running" : record.at("next_at").is_null() ? "finished" : "scheduled"},
        {"created_at", utc(record.at("created_at").get<std::int64_t>())}, {"updated_at", utc(record.at("updated_at").get<std::int64_t>())},
        {"next_at", record.at("next_at").is_null() ? Json(nullptr) : Json(utc(record.at("next_at").get<std::int64_t>()))},
        {"interval_sec", record.at("interval_sec")}, {"cancelled", record.at("cancelled")}, {"needs_attention", record.at("needs_attention")},
        {"allow_tools", record.at("allow_tools")}, {"read_only_tools", record.at("read_only_tools")},
        {"task_total_bytes", record.at("task").get_ref<const std::string&>().size()},
        {"owner_reference_total_bytes", record.at("owner_reference").get_ref<const std::string&>().size()},
        {"authorization_total_bytes", record.at("authorization").get_ref<const std::string&>().size()},
        {"scope", Json{{"root_count", scope.at("roots").size()}, {"grant_count", scope.at("grants").size()},
            {"denial_count", scope.at("denials").size()}, {"shell", scope.at("shell")}, {"allowed_tool_count", scope.at("allowed_tools").size()},
            {"timeout_sec", scope.at("timeout_sec")}}}, {"latest_run", runSummary(record.at("latest_run"))},
        {"last_error", errorSummary(record.at("last_error"))}, {"last_notification", notificationSummary(record.value("last_notification", Json(nullptr)))},
        {"history_count", record.at("history").size()}, {"log_count", record.at("logs").size()},
        {"record_retrieval", "schedule_list with schedule_id; concatenate record_json_page in offset order, then parse JSON"},
        {"authorization_reference_is_human_proof", false}, {"manager_owned", true}};
}
std::string response(Json value) {
    auto encoded = value.dump();
    bounded(encoded.size(), ScheduledTaskService::MaximumResponseBytes, "Schedule response");
    return encoded;
}
} // namespace

class ScheduledTaskService::Impl final {
public:
    Impl(Contracts::IManagedRunService& runs, Contracts::IWorkspaceAuthority& authority,
        Contracts::IAtomicFileStore& store, Contracts::IClock& clock, Contracts::IUuidGenerator& uuid,
        Contracts::IToolCatalog& catalog, Paths paths, Notification notification)
        : runs_{runs}, authority_{authority}, store_{store}, clock_{clock}, uuid_{uuid}, catalog_{catalog}, paths_{std::move(paths)}, notification_{std::move(notification)} {}
    ~Impl() noexcept { shutdown(); }
    void check(const Domain::OperationContext& context) const {
        if (context.isCancellationRequested()) fail(Domain::ErrorCodes::Cancelled, "The scheduled task operation was cancelled.");
        if (context.isExpired(clock_.monotonicNow())) fail(Domain::ErrorCodes::DeadlineExceeded, "The scheduled task operation expired.");
    }
    Domain::Result<void> initialize(const Domain::OperationContext& context) noexcept {
        return guarded([&] {
            std::lock_guard lock{mutex_}; check(context);
            if (initialized_) return;
            auto paths = storagePaths(context);
            auto loaded = store_.read(paths.read, MaximumStoreBytes, context);
            if (!loaded && loaded.error().code != Domain::ErrorCodes::RecordNotFound) throw Failure{loaded.error()};
            Json restored = Json::array();
            if (loaded) {
                auto envelope = parse({reinterpret_cast<const char*>(loaded.value().data()), loaded.value().size()}, context);
                if (envelope.at("schema") != 1 || !envelope.at("schedules").is_array()) fail(Domain::ErrorCodes::IntegrityFailure, "The schedule store schema is unsupported.");
                restored = envelope.at("schedules"); bounded(restored.size(), MaximumSchedules, "Stored schedules");
                std::set<std::string> identities;
                for (const auto& record : restored) {
                    static_cast<void>(take(Domain::Uuid::parse(text(record, "schedule_id", 36U))));
                    static_cast<void>(take(Domain::ProjectId::parse(text(record, "project_id", 36U))));
                    static_cast<void>(take(Domain::ClientId::parse(text(record, "client_id", 128U))));
                    static_cast<void>(text(record, "name", 128U)); static_cast<void>(text(record, "task", MaximumTaskBytes));
                    static_cast<void>(text(record, "owner_reference", 512U)); static_cast<void>(text(record, "authorization", 2048U));
                    static_cast<void>(scopeFrom(record.at("scope")));
                    if (!identities.insert(record.at("schedule_id").get<std::string>()).second) fail(Domain::ErrorCodes::IntegrityFailure, "The schedule store contains duplicate identities.");
                    static_cast<void>(record.at("cancelled").get<bool>()); static_cast<void>(record.at("needs_attention").get<bool>());
                    static_cast<void>(record.at("allow_tools").get<bool>()); static_cast<void>(record.at("read_only_tools").get<bool>());
                    static_cast<void>(record.at("created_at").get<std::int64_t>()); static_cast<void>(record.at("updated_at").get<std::int64_t>());
                    if (record.contains("revision") && (!record.at("revision").is_number_unsigned() &&
                        (!record.at("revision").is_number_integer() || record.at("revision").get<std::int64_t>() < 0)))
                        fail(Domain::ErrorCodes::IntegrityFailure, "A stored schedule revision is invalid.");
                    if (!record.at("next_at").is_null()) static_cast<void>(record.at("next_at").get<std::int64_t>());
                    if (!record.at("interval_sec").is_null()) {
                        const auto interval = record.at("interval_sec").get<std::int64_t>();
                        if (interval < 60 || interval > 31'536'000) fail(Domain::ErrorCodes::IntegrityFailure, "A stored schedule interval is invalid.");
                    }
                    bounded(record.at("logs").size(), 16U, "Schedule logs"); bounded(record.at("history").size(), 5U, "Schedule history");
                    if (!record.at("latest_run").is_null()) static_cast<void>(take(Domain::SessionId::parse(record.at("latest_run").at("run_id").get<std::string>())));
                }
            }
            records_ = std::move(restored); persisted_ = loaded.hasValue();
            for (std::size_t index = 0U; index < records_.size(); ++index) reconcile(index, context, true);
            initialized_ = true;
        });
    }
    Domain::Result<void> tick(const Domain::OperationContext& context) noexcept {
        return guarded([&] {
            std::lock_guard lock{mutex_}; ready(context);
            for (std::size_t index = 0U; index < records_.size(); ++index) {
                check(context); reconcile(index, context, false);
                const auto& record = records_[index];
                if (!record.at("cancelled").get<bool>() && !record.at("needs_attention").get<bool>() &&
                    !record.at("next_at").is_null() && record.at("next_at").get<std::int64_t>() <= milliseconds(clock_.utcNow()) && !nonterminal(record.at("latest_run")))
                    static_cast<void>(fire(index, context));
            }
        });
    }
    Domain::Result<std::string> execute(std::string_view name, std::string_view arguments,
        const Contracts::WorkspaceAuthority& caller, const Domain::OperationContext& context) noexcept {
        try {
            std::lock_guard lock{mutex_}; ready(context);
            bounded(arguments.size(), 256U * 1024U, "Schedule input"); const auto input = parse(arguments, context);
            auto current = take(authority_.authorityFor(caller.projectId(), context));
            if (current.callerId() != caller.callerId() || current.authorityId() != caller.authorityId() || current.generation() > caller.generation() ||
                caller.generation() == (std::numeric_limits<std::uint64_t>::max)())
                fail(Domain::ErrorCodes::Unauthorized, "The schedule caller authority is stale or mismatched.");
            // Manager routing supplies a narrowed capability with a newer
            // generation. Ask its issuer to validate that capability instead
            // of equating its generation with the full project's baseline.
            static_cast<void>(take(authority_.narrow(caller, caller.trustedRoots(), caller.grants(), caller.shellEnabled(), caller.generation() + 1U, context)));
            if (name != "schedule_list" && std::find(caller.grants().begin(), caller.grants().end(), Domain::FileAccess::Write) == caller.grants().end())
                fail(Domain::ErrorCodes::Unauthorized, "Schedule mutations require the caller's current write grant.");
            if (name == "schedule_list") {
                keys(input, {"schedule_id", "offset", "max_bytes", "revision"});
                if (input.contains("schedule_id")) {
                    const auto id = text(input, "schedule_id", 36U); static_cast<void>(take(Domain::Uuid::parse(id)));
                    const auto index = find(id, caller); reconcile(index, context, false);
                    if (unpersistedRecords_[index]) fail(Domain::ErrorCodes::Conflict,
                        "The live notification receipt has not been persisted. Inspect its summary persistence error; retry full record retrieval after a successful store commit.");
                    const auto revision = std::to_string(records_[index].value("revision", std::uint64_t{}));
                    const auto number = [&](std::string_view key, std::uint64_t fallback, std::uint64_t maximum) {
                        const auto found = input.find(std::string{key}); if (found == input.end()) return fallback;
                        if (!found->is_number_integer() || (!found->is_number_unsigned() && found->get<std::int64_t>() < 0) || found->get<std::uint64_t>() > maximum)
                            fail(Domain::ErrorCodes::InvalidRequest, std::string{key} + " is outside the supported page bound.");
                        return found->get<std::uint64_t>();
                    };
                    const auto offset = static_cast<std::size_t>(number("offset", 0U, MaximumStoreBytes));
                    const auto maximum = static_cast<std::size_t>(number("max_bytes", MaximumRecordPageBytes, MaximumRecordPageBytes));
                    if (maximum == 0U || (offset != 0U && !input.contains("revision")))
                        fail(Domain::ErrorCodes::InvalidRequest, "max_bytes must be positive; continuation pages require the returned revision.");
                    if (input.contains("revision")) {
                        const auto supplied = text(input, "revision", 20U);
                        if ((supplied.size() > 1U && supplied.front() == '0') || supplied.find_first_not_of("0123456789") != std::string::npos)
                            fail(Domain::ErrorCodes::InvalidRequest, "revision must be the canonical decimal string returned by the first page.");
                        if (supplied != revision) fail(Domain::ErrorCodes::Conflict, "The schedule record changed. Restart retrieval at offset 0 and use its returned revision.");
                    }
                    const auto encoded = publicRecord(records_[index]).dump();
                    if (offset > encoded.size() || (offset < encoded.size() && (static_cast<unsigned char>(encoded[offset]) & 0xc0U) == 0x80U))
                        fail(Domain::ErrorCodes::InvalidRequest, "offset must identify a UTF-8 boundary within the full record JSON.");
                    const auto part = page(std::string_view{encoded}.substr(offset), maximum);
                    if (part.empty() && offset < encoded.size()) fail(Domain::ErrorCodes::InvalidRequest, "max_bytes must fit the next complete UTF-8 character.");
                    const auto next = offset + part.size();
                    return Domain::Result<std::string>::success(response(Json{{"ok", true}, {"full_record", true}, {"schedule_id", id},
                        {"revision", revision}, {"encoding", "utf-8"}, {"record_json_page", part}, {"offset", offset}, {"bytes_read", part.size()},
                        {"total_bytes", encoded.size()}, {"next_offset", next < encoded.size() ? Json(next) : Json(nullptr)}, {"eof", next == encoded.size()}}));
                }
                if (input.size() != 0U) fail(Domain::ErrorCodes::InvalidRequest, "Schedule record paging properties require schedule_id.");
                Json found = Json::array();
                for (std::size_t index = 0U; index < records_.size(); ++index) if (owned(records_[index], caller)) {
                    reconcile(index, context, false); found.push_back(summary(records_[index], !unpersistedRecords_[index]));
                }
                return Domain::Result<std::string>::success(response(Json{{"ok", true}, {"count", found.size()}, {"summaries", true}, {"schedules", found}}));
            }
            if (name == "schedule_create") {
                keys(input, {"name", "task", "owner_reference", "authorization", "at_time", "interval_sec", "allow_tools", "read_only_tools", "allowed_tools", "timeout_sec"});
                bounded(records_.size() + 1U, MaximumSchedules, "Schedules");
                const auto owner = text(input, "owner_reference", 512U);
                for (const auto& record : records_) if (owned(record, caller) && record.at("owner_reference").get<std::string>() == owner &&
                    ((!record.at("cancelled").get<bool>() && (!record.at("next_at").is_null() || record.at("needs_attention").get<bool>())) ||
                     nonterminal(record.at("latest_run")))) fail(Domain::ErrorCodes::Conflict, "This owned task already has an active schedule.");
                if (input.contains("at_time") == input.contains("interval_sec")) fail(Domain::ErrorCodes::InvalidRequest, "Provide exactly one of at_time or interval_sec.");
                Json interval = nullptr; std::int64_t next{}; const auto now = milliseconds(clock_.utcNow());
                if (input.contains("at_time")) {
                    next = parseUtc(text(input, "at_time", 20U));
                    if (next <= now) fail(Domain::ErrorCodes::InvalidRequest, "at_time must be in the future.");
                } else {
                    if (!input.at("interval_sec").is_number_integer()) fail(Domain::ErrorCodes::InvalidRequest, "interval_sec must be an integer.");
                    const auto seconds = input.at("interval_sec").get<std::int64_t>();
                    if (seconds < 60 || seconds > 31'536'000) fail(Domain::ErrorCodes::InvalidRequest, "interval_sec must be between 60 and 31536000 seconds.");
                    interval = seconds; next = now + seconds * 1000;
                }
                const bool readOnly = input.value("read_only_tools", true), allow = input.value("allow_tools", true);
                std::uint32_t timeout = 600U;
                if (input.contains("timeout_sec")) {
                    const auto& rawTimeout = input.at("timeout_sec");
                    if (!rawTimeout.is_number_integer() || rawTimeout.get<std::uint64_t>() < 1U || rawTimeout.get<std::uint64_t>() > 3600U)
                        fail(Domain::ErrorCodes::InvalidRequest, "timeout_sec must be an integer between 1 and 3600.");
                    timeout = rawTimeout.get<std::uint32_t>();
                }
                Domain::ManagedRunWorkerScope scope{caller.trustedRoots(), caller.grants(), caller.denials(), caller.shellEnabled(), {}, timeout};
                bounded(scope.trustedRoots.size(), 64U, "Schedule roots");
                scope.allowedTools = input.contains("allowed_tools") ? input.at("allowed_tools").get<std::vector<std::string>>() : managedWorkerToolNames(catalog_, readOnly);
                bounded(scope.allowedTools.size(), 128U, "Schedule allowed tools");
                std::set<std::string> unique;
                for (const auto& tool : scope.allowedTools) {
                    const auto available = catalog_.tools();
                    const auto match = std::find_if(available.begin(), available.end(), [&](const auto& descriptor) { return descriptor.tool.name == tool; });
                    if (tool.empty() || tool.size() > 128U || !unique.insert(tool).second || !isManagedWorkerToolPermitted(tool) || match == available.end() ||
                        match->tool.availability != Domain::ToolAvailability::Available || (readOnly && match->tool.effect != Domain::ToolEffect::Read))
                        fail(Domain::ErrorCodes::Unauthorized, "The schedule tool allowlist is unavailable, duplicated, or outside the authorized worker mode.");
                }
                if (allow && scope.allowedTools.empty()) fail(Domain::ErrorCodes::InvalidRequest, "A tool-enabled schedule requires a nonempty allowlist.");
                Json record{{"schedule_id", take(uuid_.next()).value()}, {"project_id", caller.projectId().value()}, {"client_id", caller.callerId().value()},
                    {"name", text(input, "name", 128U)}, {"task", text(input, "task", MaximumTaskBytes)}, {"owner_reference", owner},
                    {"authorization", text(input, "authorization", 2048U)}, {"scope", scopeJson(scope)}, {"allow_tools", allow}, {"read_only_tools", readOnly},
                    {"interval_sec", interval}, {"next_at", next}, {"created_at", now}, {"updated_at", now}, {"cancelled", false}, {"needs_attention", false},
                    {"latest_run", nullptr}, {"last_error", nullptr}, {"last_notification", nullptr}, {"history", Json::array()}, {"logs", Json::array()}, {"revision", 1U}};
                auto proposed = records_; proposed.push_back(record); log(proposed.back(), "created"); commit(std::move(proposed), context);
                notify(records_.size() - 1U, "created", context); return receipt(records_.size() - 1U);
            }
            if (name != "schedule_cancel" && name != "schedule_run_now") fail(Domain::ErrorCodes::InvalidRequest, "Unknown scheduled task tool.");
            keys(input, {"schedule_id", "authorization"});
            const auto id = text(input, "schedule_id", 36U); static_cast<void>(take(Domain::Uuid::parse(id)));
            const auto authorization = text(input, "authorization", 2048U);
            const auto index = find(id, caller);
            if (name == "schedule_cancel") {
                auto proposed = records_; proposed[index]["cancelled"] = true; proposed[index]["next_at"] = nullptr;
                proposed[index]["authorization"] = authorization; log(proposed[index], "cancel_requested"); commit(std::move(proposed), context);
                reconcile(index, context, false); notify(index, "cancel_requested", context); return receipt(index);
            }
            reconcile(index, context, false);
            if (records_[index].at("cancelled").get<bool>()) fail(Domain::ErrorCodes::Conflict, "A cancelled schedule cannot be run again.");
            if (nonterminal(records_[index].at("latest_run")) && (!records_[index].at("needs_attention").get<bool>() ||
                !records_[index].at("latest_run").value("interrupted", false))) fail(Domain::ErrorCodes::Conflict, "The scheduled task already has an active or unconfirmed run.");
            auto proposed = records_; proposed[index]["needs_attention"] = false; proposed[index]["authorization"] = authorization;
            if (nonterminal(proposed[index].at("latest_run"))) {
                // The explicit invocation authorizes a new attempt; uncertain
                // prior effects remain visible in history, never called success.
                proposed[index]["history"].push_back(proposed[index]["latest_run"]); trim(proposed[index]["history"], 5U); proposed[index]["latest_run"] = nullptr;
            }
            log(proposed[index], "manual_run_authorized"); commit(std::move(proposed), context);
            if (auto error = fire(index, context)) throw Failure{std::move(*error)};
            return receipt(index);
        } catch (Failure& failure) { return Domain::Result<std::string>::failure(std::move(failure.error)); }
        catch (const Json::exception&) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Schedule input or stored fields have invalid JSON types.")); }
        catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "The scheduled task operation failed safely.")); }
    }
    Domain::Result<void> start() noexcept {
        return guarded([&] {
            std::lock_guard lock{mutex_};
            if (!initialized_ || shutdown_) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Initialize the scheduler before starting it.");
            if (worker_.joinable()) return;
            worker_ = std::jthread{[this](std::stop_token token) {
                std::string previousError;
                while (!token.stop_requested()) {
                    const auto generated = uuid_.next();
                    if (generated) {
                        const Domain::OperationContext operation{Domain::OperationId{generated.value()}, clock_.monotonicNow() + std::chrono::seconds{30}, token,
                            take(Domain::CorrelationId::parse("forge-schedule-worker"))};
                        const auto result = tick(operation);
                        if (!result && result.error().code != Domain::ErrorCodes::Cancelled) {
                            const auto currentError = result.error().code + ": " + result.error().message;
                            if (currentError != previousError) {
                                try { if (notification_) { const auto message = Json{{"event", "scheduler_error"}, {"error", errorJson(result.error())}}.dump(); static_cast<void>(notification_(message, operation)); } } catch (...) {}
                                previousError = currentError;
                            }
                        } else if (result) previousError.clear();
                    }
                    std::unique_lock wait{waitMutex_}; condition_.wait_for(wait, token, std::chrono::seconds{1}, [] { return false; });
                }
            }};
        });
    }
    void shutdown() noexcept {
        try {
            std::jthread worker;
            { std::lock_guard lock{mutex_}; if (shutdown_) return; shutdown_ = true; worker = std::move(worker_); }
            worker.request_stop(); condition_.notify_all(); if (worker.joinable()) worker.join();
            std::lock_guard lock{mutex_};
            const auto uuid = uuid_.next(); if (!uuid || !initialized_) return;
            const Domain::OperationContext operation{Domain::OperationId{uuid.value()}, clock_.monotonicNow() + std::chrono::seconds{30}, {}, take(Domain::CorrelationId::parse("forge-schedule-shutdown"))};
            for (std::size_t index = 0U; index < records_.size(); ++index) if (nonterminal(records_[index].at("latest_run"))) {
                auto cancelled = runs_.cancel(take(Domain::SessionId::parse(records_[index].at("latest_run").at("run_id").get<std::string>())), operation);
                if (cancelled) observe(index, cancelled.value(), operation, false);
            }
        } catch (...) {}
    }
private:
    template<typename F> Domain::Result<void> guarded(F action) noexcept {
        try { action(); return Domain::Result<void>::success(); }
        catch (Failure& failure) { return Domain::Result<void>::failure(std::move(failure.error)); }
        catch (const Json::exception&) { return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure, "The schedule store contains invalid JSON fields.")); }
        catch (...) { return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "The scheduled task service failed safely.")); }
    }
    void ready(const Domain::OperationContext& context) const { check(context); if (!initialized_ || shutdown_) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "The persistent Manager scheduler is unavailable."); }
    Json parse(std::string_view value, const Domain::OperationContext& context) const {
        std::vector<std::set<std::string>> objects;
        return Json::parse(value, [&](int depth, Json::parse_event_t event, Json& member) {
            check(context); if (depth > 16) fail(Domain::ErrorCodes::LimitExceeded, "Schedule JSON exceeds depth 16.");
            if (event == Json::parse_event_t::object_start) objects.emplace_back();
            else if (event == Json::parse_event_t::key && !objects.back().insert(member.get<std::string>()).second) fail(Domain::ErrorCodes::InvalidRequest, "Schedule JSON contains duplicate keys.");
            else if (event == Json::parse_event_t::object_end) objects.pop_back();
            return true;
        });
    }
    ScheduleStoragePaths storagePaths(const Domain::OperationContext& context) {
        auto paths = take(paths_(context));
        if (paths.read.access() != Domain::FileAccess::Read || paths.write.access() != Domain::FileAccess::Write || paths.create.access() != Domain::FileAccess::Create ||
            paths.read.canonicalPath() != paths.write.canonicalPath() || paths.read.canonicalPath() != paths.create.canonicalPath() ||
            paths.read.authorityRoot() != paths.write.authorityRoot() || paths.read.authorityRoot() != paths.create.authorityRoot() ||
            paths.read.authorityId() != paths.write.authorityId() || paths.read.authorityId() != paths.create.authorityId())
            fail(Domain::ErrorCodes::Unauthorized, "Schedule storage capabilities must identify one separate authorized host file.");
        return paths;
    }
    void commit(Json proposed, const Domain::OperationContext& context) {
        for (auto& record : proposed) {
            const auto previous = std::find_if(records_.begin(), records_.end(), [&](const auto& existing) { return existing.at("schedule_id") == record.at("schedule_id"); });
            if (previous != records_.end() && (*previous != record || unpersistedRecords_[static_cast<std::size_t>(std::distance(records_.begin(), previous))])) {
                const auto revision = previous->value("revision", std::uint64_t{});
                if (revision == (std::numeric_limits<std::uint64_t>::max)()) fail(Domain::ErrorCodes::IntegrityFailure, "The schedule revision cannot advance safely.");
                record["revision"] = revision + 1U;
            }
        }
        check(context); const auto bytes = Json{{"schema", 1}, {"schedules", proposed}}.dump(); bounded(bytes.size(), MaximumStoreBytes, "Schedule store");
        auto paths = storagePaths(context);
        take(store_.replace(persisted_ ? paths.write : paths.create, {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()}, true, context));
        records_ = std::move(proposed); persisted_ = true; unpersistedRecords_.fill(false);
    }
    static bool owned(const Json& record, const Contracts::WorkspaceAuthority& caller) { return record.at("project_id").get<std::string>() == caller.projectId().value() && record.at("client_id").get<std::string>() == caller.callerId().value(); }
    std::size_t find(std::string_view id, const Contracts::WorkspaceAuthority& caller) const {
        for (std::size_t index = 0U; index < records_.size(); ++index) if (records_[index].at("schedule_id").get<std::string>() == id && owned(records_[index], caller)) return index;
        fail(Domain::ErrorCodes::RecordNotFound, "No schedule with that identity is owned by this project and caller.");
    }
    static void trim(Json& entries, std::size_t count) { while (entries.size() > count) entries.erase(entries.begin()); }
    void log(Json& record, std::string_view event) { record["updated_at"] = milliseconds(clock_.utcNow()); record["logs"].push_back(Json{{"at", utc(milliseconds(clock_.utcNow()))}, {"event", event}}); trim(record["logs"], 16U); }
    void notify(std::size_t index, std::string_view event, const Domain::OperationContext& context) noexcept {
        Json result{{"event", event}, {"at", utc(milliseconds(clock_.utcNow()))}, {"receipt", nullptr}, {"error", nullptr}};
        try {
            if (notification_) {
                const auto& record = records_[index];
                const auto message = Json{{"schedule_id", record.at("schedule_id")}, {"project_id", record.at("project_id")}, {"name", record.at("name")},
                    {"event", event}, {"latest_run", record.at("latest_run")}, {"needs_attention", record.at("needs_attention")}}.dump();
                auto submitted = notification_(message, context);
                if (submitted) {
                    bounded(submitted.value().size(), 8192U, "Notification receipt");
                    auto receipt = parse(submitted.value(), context);
                    if (!receipt.is_object()) fail(Domain::ErrorCodes::InvalidRequest, "The notification callback returned a nonobject receipt.");
                    result["receipt"] = std::move(receipt);
                } else result["error"] = errorJson(submitted.error());
            } else result["receipt"] = Json{{"channel", "unconfigured"}, {"submission_accepted", false}, {"display_confirmed", false}};
        } catch (Failure& failure) { result["error"] = errorJson(failure.error); }
        catch (...) { result["error"] = errorJson(Domain::makeError(Domain::ErrorCodes::InternalFailure, "The notification callback failed safely; display is unconfirmed.")); }
        // Notification results never change provider state or cause automatic
        // resubmission. If their separate persistence fails, retain the actual
        // submission receipt and the persistence failure in the live response.
        try {
            auto proposed = records_; proposed[index]["last_notification"] = result;
            commit(std::move(proposed), context);
        } catch (Failure& failure) {
            result["persistence_error"] = errorJson(failure.error); retainLiveNotification(index, std::move(result));
        } catch (...) {
            result["persistence_error"] = errorJson(Domain::makeError(Domain::ErrorCodes::InternalFailure, "The notification receipt could not be persisted."));
            retainLiveNotification(index, std::move(result));
        }
    }
    void retainLiveNotification(std::size_t index, Json result) noexcept {
        try {
            // Do not issue a revision for fields absent from durable storage:
            // otherwise a restart could reuse the token for different data.
            unpersistedRecords_[index] = true;
            records_[index]["last_notification"] = std::move(result);
        } catch (...) {}
    }
    Domain::Result<std::string> receipt(std::size_t index) const { return Domain::Result<std::string>::success(response(Json{{"ok", true}, {"schedule", summary(records_[index], !unpersistedRecords_[index])}})); }
    void block(std::size_t index, const Domain::Error& error, const Domain::OperationContext& context) {
        auto proposed = records_; proposed[index]["needs_attention"] = true; proposed[index]["last_error"] = errorJson(error);
        log(proposed[index], "needs_attention"); commit(std::move(proposed), context); notify(index, "needs_attention", context);
    }
    void observe(std::size_t index, const Domain::ManagedRunSnapshot& run, const Domain::OperationContext& context, bool restored) {
        if (run.record.projectId.value() != records_[index].at("project_id").get<std::string>() || run.record.clientId.value() != records_[index].at("client_id").get<std::string>())
            fail(Domain::ErrorCodes::IntegrityFailure, "A scheduled run belongs to a different owner.");
        auto actual = runJson(run); const bool interrupted = run.record.workerInterrupted || (restored && nonterminal(actual));
        if (interrupted) actual["interrupted"] = true;
        const bool changed = records_[index].at("latest_run").at("state") != actual.at("state") || interrupted;
        if (records_[index].at("latest_run") == actual && (!interrupted || records_[index].at("needs_attention").get<bool>())) return;
        auto proposed = records_; proposed[index]["latest_run"] = actual;
        if (interrupted) {
            proposed[index]["needs_attention"] = true;
            proposed[index]["last_error"] = errorJson(Domain::makeError(Domain::ErrorCodes::Conflict, "A prior Manager run was interrupted; its effects are uncertain. Explicit run-now authorization is required."));
        }
        if (changed) log(proposed[index], interrupted ? "interrupted" : actual.at("state").get<std::string>());
        if (!nonterminal(actual) && (proposed[index]["history"].empty() || proposed[index]["history"].back().at("run_id") != actual.at("run_id"))) {
            proposed[index]["history"].push_back(actual); trim(proposed[index]["history"], 5U);
        }
        commit(std::move(proposed), context); if (changed) notify(index, interrupted ? "interrupted" : actual.at("state").get<std::string>(), context);
    }
    void reconcile(std::size_t index, const Domain::OperationContext& context, bool restored) {
        const auto latest = records_[index].at("latest_run"); if (latest.is_null()) return;
        const auto id = take(Domain::SessionId::parse(latest.at("run_id").get<std::string>()));
        auto actual = records_[index].at("cancelled").get<bool>() && nonterminal(latest) ? runs_.cancel(id, context) : runs_.status(id, context);
        if (!actual) {
            const bool missing = actual.error().code == Domain::ErrorCodes::SessionNotFound;
            if (restored || missing) {
                auto proposed = records_; proposed[index]["latest_run"]["interrupted"] = true;
                if (missing) {
                    proposed[index]["latest_run"]["state"] = "missing";
                    proposed[index]["latest_run"]["error"] = errorJson(actual.error());
                    if (!proposed[index]["latest_run"].contains("previous_error") && !proposed[index].at("last_error").is_null())
                        proposed[index]["latest_run"]["previous_error"] = proposed[index].at("last_error");
                }
                if (proposed[index].at("latest_run") != latest) {
                    // A missing durable run never establishes that admission
                    // had no effects. Keep the uncertainty and its diagnostics
                    // visible, but permit an explicitly authorized new attempt
                    // without requiring a Manager restart. Other status errors
                    // leave the unconfirmed live run protected from duplication.
                    commit(std::move(proposed), context); notify(index, "interrupted", context);
                }
            }
            if (!records_[index].at("needs_attention").get<bool>()) block(index, actual.error(), context);
            return;
        }
        observe(index, actual.value(), context, restored);
    }
    std::optional<Domain::Error> fire(std::size_t index, const Domain::OperationContext& context) {
        try {
            const auto record = records_[index];
            for (std::size_t other = 0U; other < records_.size(); ++other) if (other != index && records_[other].at("project_id") == record.at("project_id") &&
                records_[other].at("client_id") == record.at("client_id") && records_[other].at("owner_reference") == record.at("owner_reference") && nonterminal(records_[other].at("latest_run")))
                fail(Domain::ErrorCodes::Conflict, "The owned task already has another active run.");
            auto current = take(authority_.authorityFor(take(Domain::ProjectId::parse(record.at("project_id").get<std::string>())), context));
            if (current.callerId().value() != record.at("client_id").get<std::string>()) fail(Domain::ErrorCodes::Unauthorized, "The schedule owner no longer matches the current authority issuer.");
            auto frozen = scopeFrom(record.at("scope"));
            std::erase_if(frozen.trustedRoots, [&](const auto& root) { return std::find(current.trustedRoots().begin(), current.trustedRoots().end(), root) == current.trustedRoots().end(); });
            std::erase_if(frozen.grants, [&](auto grant) { return std::find(current.grants().begin(), current.grants().end(), grant) == current.grants().end() || std::find(current.denials().begin(), current.denials().end(), grant) != current.denials().end(); });
            for (const auto denial : current.denials()) if (std::find(frozen.denials.begin(), frozen.denials.end(), denial) == frozen.denials.end()) frozen.denials.push_back(denial);
            frozen.shellEnabled = frozen.shellEnabled && current.shellEnabled();
            if (frozen.trustedRoots.empty() || frozen.grants.empty()) fail(Domain::ErrorCodes::Unauthorized, "The schedule's frozen scope has no intersection with current owner policy.");
            const auto runId = Domain::SessionId{take(uuid_.next())}; const auto now = milliseconds(clock_.utcNow());
            const auto operationId = Domain::OperationId{take(uuid_.next())};
            const auto correlationId = take(Domain::CorrelationId::parse("schedule-" + record.at("schedule_id").get<std::string>()));
            Domain::ManagedRunStartRequest request{runId, current.projectId(), current.callerId(), operationId, correlationId, current.generation(),
                record.at("task").get<std::string>(), record.at("allow_tools").get<bool>(), false, record.at("read_only_tools").get<bool>(), frozen.timeoutSeconds};
            request.workerScope = frozen;
            auto proposed = records_; proposed[index]["latest_run"] = Json{{"run_id", runId.value()}, {"state", "admitted"}, {"updated_at", now}, {"interrupted", false}};
            proposed[index]["next_at"] = record.at("interval_sec").is_null() ? Json(nullptr) : Json(now + record.at("interval_sec").get<std::int64_t>() * 1000);
            proposed[index]["last_error"] = nullptr; log(proposed[index], "dispatch_admitted");
            // Durable admission precedes inference. An ambiguous save or start
            // is never followed by a second provider submission automatically.
            commit(std::move(proposed), context);
            auto started = runs_.start(request, context); if (!started) throw Failure{started.error()};
            observe(index, started.value(), context, false); return std::nullopt;
        } catch (Failure& failure) {
            block(index, failure.error, context); return std::move(failure.error);
        }
    }
    Contracts::IManagedRunService& runs_; Contracts::IWorkspaceAuthority& authority_; Contracts::IAtomicFileStore& store_;
    Contracts::IClock& clock_; Contracts::IUuidGenerator& uuid_; Contracts::IToolCatalog& catalog_; Paths paths_; Notification notification_;
    std::mutex mutex_, waitMutex_; std::condition_variable_any condition_; std::jthread worker_;
    Json records_ = Json::array(); bool persisted_{}, initialized_{}, shutdown_{};
    std::array<bool, MaximumSchedules> unpersistedRecords_{};
};

ScheduledTaskService::ScheduledTaskService(Contracts::IManagedRunService& runs, Contracts::IWorkspaceAuthority& authority,
    Contracts::IAtomicFileStore& store, Contracts::IClock& clock, Contracts::IUuidGenerator& uuid, Contracts::IToolCatalog& catalog,
    Paths paths, Notification notification) : implementation_{std::make_unique<Impl>(runs, authority, store, clock, uuid, catalog, std::move(paths), std::move(notification))} {}
ScheduledTaskService::~ScheduledTaskService() noexcept = default;
Domain::Result<void> ScheduledTaskService::initialize(const Domain::OperationContext& context) noexcept { return implementation_->initialize(context); }
Domain::Result<void> ScheduledTaskService::tick(const Domain::OperationContext& context) noexcept { return implementation_->tick(context); }
Domain::Result<void> ScheduledTaskService::start() noexcept { return implementation_->start(); }
void ScheduledTaskService::shutdown() noexcept { implementation_->shutdown(); }
Domain::Result<std::string> ScheduledTaskService::execute(std::string_view name, std::string_view arguments, const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept { return implementation_->execute(name, arguments, authority, context); }
} // namespace ForgeConductor::Application
