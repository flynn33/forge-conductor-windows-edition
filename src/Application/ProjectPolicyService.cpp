#include "ForgeConductor/Application/ProjectPolicyService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace ForgeConductor::Application {
namespace {

using Json = nlohmann::json;

template<class T>
T take(Domain::Result<T> result)
{
    if (!result) throw std::runtime_error{result.error().message};
    return std::move(result).value();
}

void valid(const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) {
        throw std::runtime_error{"CLU governance operation cancelled."};
    }
    if (context.isExpired(std::chrono::steady_clock::now())) {
        throw std::runtime_error{"CLU governance operation deadline expired."};
    }
}

std::span<const std::byte> bytes(const std::string& value)
{
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

std::string normalized(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z'
            ? character + 32 : character);
    });
    while (path.size() > 1U && path.ends_with('/')) path.pop_back();
    return path;
}

bool below(const std::string& path, const std::string& root)
{
    return path == root ||
        (path.size() > root.size() && path.starts_with(root) &&
            path[root.size()] == '/');
}

std::int64_t nowMilliseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string severityName(const Json& rule)
{
    const auto value = rule.value("severity", std::string{"warning"});
    if (value == "information" || value == "warning" || value == "error" ||
        value == "critical") return value;
    return "warning";
}

void redact(Json& value, std::string key = {})
{
    auto lowered = normalized(std::move(key));
    const bool sensitive = lowered.find("token") != std::string::npos ||
        lowered.find("secret") != std::string::npos ||
        lowered.find("password") != std::string::npos ||
        lowered.find("credential") != std::string::npos ||
        lowered.find("private_key") != std::string::npos ||
        lowered.find("authorization") != std::string::npos;
    if (sensitive && !value.is_null()) {
        value = "[REDACTED]";
        return;
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (!text.empty() && (text.front() == '{' || text.front() == '[')) {
            try {
                auto nested = Json::parse(text);
                redact(nested);
                value = nested.dump();
            } catch (...) {
                // Ordinary text remains unchanged. Export redaction must never
                // turn a non-JSON evidence string into a policy failure.
            }
        }
        return;
    }
    if (value.is_object()) {
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
            redact(iterator.value(), iterator.key());
        }
    } else if (value.is_array()) {
        for (auto& item : value) redact(item);
    }
}

Json compileRules(const Json& entries)
{
    Json rules = Json::array();
    for (const auto& entry : entries) {
        if (!entry.contains("content") || entry.at("content").is_null()) continue;
        const auto path = entry.value("path", std::string{});
        const auto content = entry.at("content").get<std::string>();
        try {
            const auto document = Json::parse(content);
            if (document.is_object() && document.contains("forge_clu_rules") &&
                document.at("forge_clu_rules").is_array()) {
                std::size_t index{};
                for (auto rule : document.at("forge_clu_rules")) {
                    if (!rule.is_object()) continue;
                    rule["id"] = rule.value("id", path + "#" + std::to_string(index));
                    rule["title"] = rule.value("title", rule.at("id").get<std::string>());
                    rule["source"] = path;
                    rule["severity"] = severityName(rule);
                    rule["correction"] = rule.value(
                        "correction", std::string{"Correct the operation to comply with the bound development policy."});
                    rules.push_back(std::move(rule));
                    ++index;
                }
            }
        } catch (...) {
        }
        std::istringstream lines{content};
        std::string line;
        std::size_t lineNumber{};
        while (std::getline(lines, line)) {
            ++lineNumber;
            const auto add = [&](const std::string_view prefix,
                                 const std::string_view field) {
                if (!line.starts_with(prefix)) return false;
                auto value = line.substr(prefix.size());
                std::string correction{
                    "Correct the operation to comply with this policy rule."};
                if (const auto separator = value.find('|');
                    separator != std::string::npos) {
                    correction = value.substr(separator + 1U);
                    value.resize(separator);
                }
                while (!value.empty() && value.front() == ' ') value.erase(0U, 1U);
                while (!value.empty() && value.back() == ' ') value.pop_back();
                while (!correction.empty() && correction.front() == ' ') {
                    correction.erase(0U, 1U);
                }
                if (value.empty()) return true;
                rules.push_back({
                    {"id", path + ":" + std::to_string(lineNumber)},
                    {"title", line.substr(0U, (std::min)(line.size(), std::size_t{120U}))},
                    {"source", path + ":" + std::to_string(lineNumber)},
                    {"severity", "error"},
                    {std::string{field}, value},
                    {"correction", correction}});
                return true;
            };
            if (add("FORBID_TOOL ", "tool")) continue;
            static_cast<void>(add("FORBID_PATH ", "path_contains"));
        }
    }
    return rules;
}

Json summary(const Json& snapshot)
{
    if (snapshot.is_null()) {
        return Json{{"active", false}, {"state", "not_configured"},
            {"open_findings", 0U}, {"coverage_gap_count", 0U}};
    }
    const auto& binding = snapshot.at("binding");
    std::size_t open{};
    std::string highest{"none"};
    const std::map<std::string, int> rank{{"none", 0}, {"information", 1},
        {"warning", 2}, {"error", 3}, {"critical", 4}};
    for (const auto& finding : snapshot.at("findings")) {
        if (finding.value("state", std::string{}) == "resolved") continue;
        ++open;
        const auto severity = finding.value("severity", std::string{"warning"});
        if (rank.at(severity) > rank.at(highest)) highest = severity;
    }
    Json guidance = Json::array();
    for (const auto& finding : snapshot.at("findings")) {
        if (finding.value("state", std::string{}) == "resolved") continue;
        guidance.push_back({
            {"finding_id", finding.at("finding_id")},
            {"rule_id", finding.at("rule_id")},
            {"evidence", finding.at("evidence")},
            {"required_correction", finding.at("requested_correction")}});
    }
    Json coverage = Json::array();
    for (const auto& entry : snapshot.at("entries")) {
        coverage.push_back({{"path", entry.at("path")},
            {"kind", entry.at("kind")},
            {"byte_length", entry.at("byte_length")},
            {"content_hash", entry.at("content_hash")},
            {"interpretation", entry.at("interpretation")},
            {"coverage_detail", entry.at("coverage_detail")}});
    }
    return Json{{"active", true}, {"state", "enforcing"},
        {"source", binding.at("source")},
        {"commit", binding.at("commit")},
        {"revision", binding.at("revision")},
        {"entry_count", binding.at("entry_count")},
        {"coverage_gap_count", binding.at("coverage_gap_count")},
        {"open_findings", open}, {"highest_severity", highest},
        {"coverage", std::move(coverage)},
        {"agent_guidance", std::move(guidance)}};
}

} // namespace

class ProjectPolicyService::Impl final {
public:
    Impl(Contracts::IPolicySourceReader& source,
         Contracts::IAtomicFileStore& files,
         Contracts::IHasher& hasher,
         Contracts::IProjectRegistryRepository& projects,
         Paths paths,
         std::string protectedRoot)
        : source_{source}, files_{files}, hasher_{hasher}, projects_{projects},
          paths_{std::move(paths)},
          protectedRoot_{normalized(std::move(protectedRoot))}
    {
    }

    void validateRoots(const std::vector<Domain::PathText>& roots) const
    {
        if (protectedRoot_.empty()) return;
        for (const auto& root : roots) {
            const auto path = normalized(root.value());
            if (below(path, protectedRoot_) || below(protectedRoot_, path)) {
                throw std::runtime_error{
                    "Choose a project folder that does not overlap Forge Conductor's private data folder."};
            }
        }
    }

    std::string digest(const Json& value)
    {
        return take(hasher_.sha256(bytes(value.dump()))).value();
    }

    Json migrateLegacy(Json snapshot)
    {
        if (snapshot.is_null() || snapshot.value("schema", 0) != 1) return snapshot;
        Json entries = Json::array();
        const auto& bundle = snapshot.at("bundle");
        for (const auto& file : bundle.at("files")) {
            const auto& content = file.at("content").get_ref<const std::string&>();
            entries.push_back({{"path", file.at("path")}, {"kind", "file"},
                {"byte_length", content.size()}, {"content_hash", nullptr},
                {"content", content},
                {"interpretation", "interpreted"}, {"coverage_detail", nullptr}});
        }
        for (const auto& path : bundle.value("excluded_files", Json::array())) {
            entries.push_back({{"path", path}, {"kind", "file"},
                {"byte_length", 0U}, {"content_hash", nullptr},
                {"content", nullptr},
                {"interpretation", "opaque"},
                {"coverage_detail", "Migrated legacy non-text coverage gap"}});
        }
        const auto gaps = static_cast<std::size_t>(std::count_if(
            entries.begin(), entries.end(), [](const Json& entry) {
                return entry.value("interpretation", std::string{}) != "interpreted";
            }));
        Json migrated{{"schema", 2}, {"project", snapshot.at("project")},
            {"binding", {{"source", bundle.at("source")},
                {"commit", bundle.at("commit")},
                {"revision", snapshot.at("revision")},
                {"entry_count", entries.size()}, {"coverage_gap_count", gaps},
                {"bound_at_utc_ms", nowMilliseconds()}}},
            {"entries", std::move(entries)}, {"findings", Json::array()},
            {"notifications", Json::array()}, {"history", Json::array()}};
        if (snapshot.contains("review")) {
            migrated["history"].push_back({
                {"kind", "legacy_policy_review"},
                {"evidence", snapshot.at("review")},
                {"correlation_id", "legacy-migration"},
                {"migrated_at_utc_ms", nowMilliseconds()}});
        }
        migrated["rules"] = compileRules(migrated.at("entries"));
        return migrated;
    }

    Json load(const PolicyStoragePaths& paths,
              const Domain::OperationContext& context)
    {
        auto content = files_.read(
            paths.read, Contracts::IAtomicFileStore::MaximumBytes, context);
        if (!content && content.error().code == Domain::ErrorCodes::RecordNotFound) {
            return nullptr;
        }
        const auto value = take(std::move(content));
        auto snapshot = Json::parse(
            reinterpret_cast<const char*>(value.data()),
            reinterpret_cast<const char*>(value.data()) + value.size());
        const auto legacy = snapshot.is_object() && snapshot.value("schema", 0) == 1;
        snapshot = migrateLegacy(std::move(snapshot));
        if (!snapshot.is_object() || snapshot.value("schema", 0) != 2 ||
            !snapshot.contains("binding") || !snapshot.contains("entries") ||
            !snapshot.contains("rules") || !snapshot.contains("findings") ||
            !snapshot.contains("notifications") || !snapshot.contains("history")) {
            throw std::runtime_error{
                "The CLU governance snapshot failed integrity validation."};
        }
        if (legacy) save(paths, snapshot, true, context);
        return snapshot;
    }

    void save(const PolicyStoragePaths& paths,
              const Json& snapshot,
              const bool exists,
              const Domain::OperationContext& context)
    {
        valid(context);
        const auto encoded = snapshot.dump();
        if (encoded.size() > Contracts::IAtomicFileStore::MaximumBytes) {
            throw std::runtime_error{
                "The derived governance catalog exceeded local storage capacity; the source remains unchanged."};
        }
        auto written = files_.replace(
            exists ? paths.write : paths.create, bytes(encoded), true, context);
        if (!written) throw std::runtime_error{written.error().message};
    }

    Json bind(const Contracts::ProjectPolicyRequest& request,
              const PolicyStoragePaths& paths,
              const Json& current,
              const Domain::OperationContext& context)
    {
        const auto selectedSource = request.action == Contracts::ProjectPolicyAction::Refresh
            ? current.at("binding").at("source").get<std::string>()
            : request.source;
        if (selectedSource.empty()) {
            throw std::runtime_error{"Select a local or remote development-policy source."};
        }
        const auto imported = take(source_.read(selectedSource, context));
        Json entries = Json::array();
        std::size_t gaps{};
        for (const auto& file : imported.files) {
            if (file.interpretation != "interpreted") ++gaps;
            entries.push_back({{"path", file.path}, {"kind", file.kind},
                {"byte_length", file.byteLength},
                {"content_hash", file.contentHash
                    ? Json(*file.contentHash) : Json(nullptr)},
                {"content", file.content ? Json(*file.content) : Json(nullptr)},
                {"interpretation", file.interpretation},
                {"coverage_detail", file.coverageDetail
                    ? Json(*file.coverageDetail) : Json(nullptr)}});
        }
        Json revisionMaterial{{"source", imported.source},
            {"commit", imported.commit}, {"entries", entries}};
        const auto revision = digest(revisionMaterial);
        Json history = current.is_null() ? Json::array() : current.at("history");
        if (!current.is_null()) {
            history.push_back({{"kind", "policy_revision_replaced"},
                {"revision", current.at("binding").at("revision")},
                {"replaced_at_utc_ms", nowMilliseconds()}});
        }
        Json snapshot{{"schema", 2}, {"project", request.projectId.value()},
            {"binding", {{"source", imported.source}, {"commit", imported.commit},
                {"revision", revision}, {"entry_count", entries.size()},
                {"coverage_gap_count", gaps},
                {"bound_at_utc_ms", nowMilliseconds()}}},
            {"entries", std::move(entries)}, {"findings", Json::array()},
            {"notifications", Json::array()}, {"history", std::move(history)}};
        snapshot["rules"] = compileRules(snapshot.at("entries"));
        snapshot["history"].push_back({{"kind", "policy_revision_bound"},
            {"revision", revision}, {"source", imported.source},
            {"correlation_id", context.correlationId.value()},
            {"coverage_gap_count", gaps}, {"bound_at_utc_ms", nowMilliseconds()}});
        save(paths, snapshot, !current.is_null(), context);
        return snapshot;
    }

    bool ruleMatches(const Json& rule, const Json& evidence) const
    {
        if (rule.contains("tool") &&
            rule.at("tool").get<std::string>() != evidence.value("tool_name", "")) {
            return false;
        }
        if (rule.contains("effect") &&
            rule.at("effect").get<std::string>() != evidence.value("effect", "")) {
            return false;
        }
        if (rule.contains("path_contains")) {
            const auto arguments = evidence.value("arguments", std::string{});
            if (arguments.find(rule.at("path_contains").get<std::string>()) ==
                std::string::npos) return false;
        }
        return rule.contains("tool") || rule.contains("effect") ||
            rule.contains("path_contains");
    }

    std::optional<std::string> evaluate(Json& snapshot, const Json& evidence)
    {
        for (const auto& rule : snapshot.at("rules")) {
            if (!ruleMatches(rule, evidence)) continue;
            auto stableEvidence = evidence;
            stableEvidence.erase("observed_at_utc_ms");
            stableEvidence.erase("duration_ms");
            const auto material = snapshot.at("project").get<std::string>() + "\n" +
                snapshot.at("binding").at("revision").get<std::string>() + "\n" +
                rule.at("id").get<std::string>() + "\n" + stableEvidence.dump();
            const auto findingId = "finding-" + digest(material).substr(0U, 32U);
            auto found = std::find_if(snapshot["findings"].begin(),
                snapshot["findings"].end(), [&](const Json& finding) {
                    return finding.value("finding_id", std::string{}) == findingId;
                });
            if (found == snapshot["findings"].end()) {
                Json finding{{"finding_id", findingId},
                    {"project_id", snapshot.at("project")},
                    {"policy_revision", snapshot.at("binding").at("revision")},
                    {"rule_id", rule.at("id")}, {"rule_title", rule.at("title")},
                    {"rule_source", rule.at("source")},
                    {"severity", severityName(rule)},
                    {"state", "correction_requested"},
                    {"summary", "Development activity conflicts with the bound policy."},
                    {"evidence", evidence},
                    {"requested_correction", rule.at("correction")},
                    {"correction_evidence", Json::array()},
                    {"first_observed_utc_ms", nowMilliseconds()},
                    {"last_observed_utc_ms", nowMilliseconds()}};
                snapshot["findings"].push_back(finding);
                snapshot["notifications"].push_back({
                    {"notification_id", "notification-" + digest(findingId).substr(0U, 32U)},
                    {"finding_id", findingId}, {"state", "pending"},
                    {"rule_id", rule.at("id")}, {"evidence", evidence},
                    {"required_correction", rule.at("correction")},
                    {"created_at_utc_ms", nowMilliseconds()}});
            } else {
                (*found)["last_observed_utc_ms"] = nowMilliseconds();
            }
            return findingId;
        }
        return std::nullopt;
    }

    std::string execute(const Contracts::ProjectPolicyRequest& request,
                        const Domain::OperationContext& context)
    {
        valid(context);
        validateRoots(take(projects_.descriptor(request.projectId, context)).aliases);
        std::lock_guard lock{mutex_};
        const auto paths = take(paths_(request.projectId, context));
        auto current = load(paths, context);
        if (!current.is_null() &&
            current.at("project") != request.projectId.value()) {
            throw std::runtime_error{"CLU governance data belongs to another project."};
        }
        if (request.action == Contracts::ProjectPolicyAction::Inspect) {
            return summary(current).dump();
        }
        if (request.action == Contracts::ProjectPolicyAction::Bind ||
            request.action == Contracts::ProjectPolicyAction::Refresh) {
            if (request.action == Contracts::ProjectPolicyAction::Refresh &&
                current.is_null()) {
                throw std::runtime_error{"No policy source is bound to refresh."};
            }
            return summary(bind(request, paths, current, context)).dump();
        }
        if (current.is_null()) {
            throw std::runtime_error{"No development policy is bound to this project."};
        }
        if (!request.expectedRevision.empty() &&
            current.at("binding").at("revision") != request.expectedRevision) {
            throw std::runtime_error{
                "The bound policy revision changed; refresh the governance view."};
        }
        if (request.action == Contracts::ProjectPolicyAction::ReadDocument) {
            for (const auto& entry : current.at("entries")) {
                if (entry.at("path") != request.source) continue;
                if (!entry.contains("content") || entry.at("content").is_null()) {
                    return Json{{"revision", current.at("binding").at("revision")},
                        {"path", request.source}, {"content", ""},
                        {"next_offset", 0U}, {"complete", true},
                        {"interpretation", entry.at("interpretation")},
                        {"coverage_detail", entry.at("coverage_detail")}}.dump();
                }
                const auto& content = entry.at("content").get_ref<const std::string&>();
                const auto window = request.detailsJson.empty()
                    ? Json::object() : Json::parse(request.detailsJson);
                const auto offset = window.value("offset", std::size_t{});
                if (offset > content.size()) {
                    throw std::runtime_error{"Policy document offset is outside the derived text."};
                }
                auto end = (std::min)(content.size(), offset + 32U * 1024U);
                while (end < content.size() &&
                    (static_cast<unsigned char>(content[end]) & 0xc0U) == 0x80U) {
                    --end;
                }
                return Json{{"revision", current.at("binding").at("revision")},
                    {"path", request.source},
                    {"content", content.substr(offset, end - offset)},
                    {"next_offset", end}, {"complete", end == content.size()},
                    {"interpretation", entry.at("interpretation")},
                    {"coverage_detail", entry.at("coverage_detail")}}.dump();
            }
            throw std::runtime_error{"That entry is not in the bound policy revision."};
        }
        if (request.action == Contracts::ProjectPolicyAction::ListFindings) {
            auto result = Json{{"revision", current.at("binding").at("revision")},
                {"findings", current.at("findings")},
                {"notifications", current.at("notifications")}};
            Json activity = Json::array();
            const auto& history = current.at("history");
            const auto first = history.size() > 100U ? history.size() - 100U : 0U;
            for (auto index = first; index < history.size(); ++index) {
                const auto& event = history.at(index);
                const auto findingId = event.contains("finding_id") &&
                        event.at("finding_id").is_string()
                    ? event.at("finding_id").get<std::string>() : std::string{};
                const auto correlationId = event.contains("correlation_id") &&
                        event.at("correlation_id").is_string()
                    ? event.at("correlation_id").get<std::string>() : std::string{};
                activity.push_back({
                    {"kind", event.value("kind", std::string{"policy_event"})},
                    {"finding_id", findingId},
                    {"correlation_id", correlationId},
                    {"timestamp_utc_ms", event.value("resolved_at_utc_ms",
                        event.value("evaluated_at_utc_ms",
                            event.value("bound_at_utc_ms",
                                event.value("migrated_at_utc_ms", std::int64_t{}))))}});
            }
            result["activity"] = std::move(activity);
            if (!request.detailsJson.empty()) {
                const auto details = Json::parse(request.detailsJson);
                if (details.value("acknowledge_notifications", false)) {
                    Json delivered = Json::array();
                    for (auto& notification : current["notifications"]) {
                        if (notification.value("state", std::string{}) != "pending") continue;
                        delivered.push_back(notification);
                        notification["state"] = "delivered";
                        notification["delivered_at_utc_ms"] = nowMilliseconds();
                    }
                    if (!delivered.empty()) save(paths, current, true, context);
                    result["notifications"] = std::move(delivered);
                }
            }
            return result.dump();
        }
        if (request.action == Contracts::ProjectPolicyAction::Evaluate) {
            if (request.detailsJson.empty()) {
                throw std::runtime_error{"CLU evaluation requires development evidence."};
            }
            auto evidence = Json::parse(request.detailsJson);
            const auto finding = evaluate(current, evidence);
            current["history"].push_back({{"kind", "evaluation"},
                {"evidence", evidence},
                {"finding_id", finding ? Json(*finding) : Json(nullptr)},
                {"correlation_id", context.correlationId.value()},
                {"evaluated_at_utc_ms", nowMilliseconds()}});
            save(paths, current, true, context);
            auto result = summary(current);
            result["finding_id"] = finding ? Json(*finding) : Json(nullptr);
            return result.dump();
        }
        if (request.action == Contracts::ProjectPolicyAction::Resolve) {
            const auto details = Json::parse(request.detailsJson);
            const auto findingId = details.at("finding_id").get<std::string>();
            auto found = std::find_if(current["findings"].begin(),
                current["findings"].end(), [&](const Json& finding) {
                    return finding.value("finding_id", std::string{}) == findingId;
                });
            if (found == current["findings"].end()) {
                throw std::runtime_error{"The selected CLU finding was not found."};
            }
            (*found)["state"] = "resolved";
            (*found)["correction_evidence"].push_back(details.at("correction_evidence"));
            (*found)["resolved_at_utc_ms"] = nowMilliseconds();
            current["history"].push_back({{"kind", "finding_resolved"},
                {"finding_id", findingId},
                {"correction_evidence", details.at("correction_evidence")},
                {"correlation_id", context.correlationId.value()},
                {"resolved_at_utc_ms", nowMilliseconds()}});
            save(paths, current, true, context);
            return Json{{"finding", *found}, {"summary", summary(current)}}.dump();
        }
        if (request.action == Contracts::ProjectPolicyAction::ExportLog) {
            Json exported{{"schema", "forge-clu-governance-log-v1"},
                {"project_id", current.at("project")},
                {"binding", current.at("binding")},
                {"coverage", current.at("entries")},
                {"findings", current.at("findings")},
                {"notifications", current.at("notifications")},
                {"history", current.at("history")}};
            for (auto& entry : exported["coverage"]) entry.erase("content");
            redact(exported);
            return exported.dump();
        }
        throw std::runtime_error{"Unknown CLU governance operation."};
    }

    void check(const Domain::ToolAuthorizationRequest& request,
               const Contracts::WorkspaceAuthority& authority,
               const Domain::OperationContext& context)
    {
        valid(context);
        std::lock_guard lock{mutex_};
        validateRoots(authority.trustedRoots());
        const auto paths = take(paths_(authority.projectId(), context));
        auto current = load(paths, context);
        if (current.is_null()) return;
        if (current.at("project") != authority.projectId().value()) return;
        const auto effect = request.effect == Domain::ToolEffect::Read ? "read" :
            request.effect == Domain::ToolEffect::Write ? "write" :
            request.effect == Domain::ToolEffect::Execute ? "execute" : "destructive";
        Json evidence{{"phase", "pre_operation"},
            {"tool_name", request.call.toolName},
            {"arguments", request.call.canonicalArguments},
            {"effect", effect}, {"observed_at_utc_ms", nowMilliseconds()}};
        if (evaluate(current, evidence)) save(paths, current, true, context);
    }

    Contracts::IPolicySourceReader& source_;
    Contracts::IAtomicFileStore& files_;
    Contracts::IHasher& hasher_;
    Contracts::IProjectRegistryRepository& projects_;
    Paths paths_;
    std::string protectedRoot_;
    std::mutex mutex_;
};

ProjectPolicyService::ProjectPolicyService(
    Contracts::IPolicySourceReader& source,
    Contracts::IAtomicFileStore& files,
    Contracts::IHasher& hasher,
    Contracts::IProjectRegistryRepository& projects,
    Paths paths,
    std::string protectedRoot)
    : implementation_{std::make_unique<Impl>(source, files, hasher, projects,
        std::move(paths), std::move(protectedRoot))}
{
}

ProjectPolicyService::~ProjectPolicyService() = default;

Domain::Result<std::string> ProjectPolicyService::execute(
    const Contracts::ProjectPolicyRequest& request,
    const Domain::OperationContext& context) noexcept
{
    try {
        return Domain::Result<std::string>::success(
            implementation_->execute(request, context));
    } catch (const std::exception& failure) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, failure.what()));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "CLU governance operation failed safely."));
    }
}

Domain::Result<void> ProjectPolicyService::check(
    const Domain::ToolAuthorizationRequest& request,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    try {
        implementation_->check(request, authority, context);
    } catch (...) {
        // Governance findings guide and report. They never replace the
        // independent workspace/security authorizer or veto development.
    }
    return Domain::Result<void>::success();
}

} // namespace ForgeConductor::Application
