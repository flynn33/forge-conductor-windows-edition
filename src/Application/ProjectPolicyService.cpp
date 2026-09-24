#include "ForgeConductor/Application/ProjectPolicyService.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>

namespace ForgeConductor::Application {
namespace {
using Json = nlohmann::json;
template<class T> T take(Domain::Result<T> result)
{
    if (!result) throw std::runtime_error{result.error().message};
    return std::move(result).value();
}
void valid(const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) throw std::runtime_error{"Policy operation cancelled."};
    if (context.isExpired(std::chrono::steady_clock::now())) throw std::runtime_error{"Policy operation deadline expired."};
}
std::span<const std::byte> bytes(const std::string& value)
{ return {reinterpret_cast<const std::byte*>(value.data()), value.size()}; }
std::string normalized(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    while (path.ends_with('/')) path.pop_back();
    return path;
}
bool below(const std::string& path, const std::string& root)
{ return path == root || (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/'); }
bool relative(const std::string& value)
{
    if (value.empty() || value.starts_with('/') || value.find(':') != std::string::npos || value.find('\0') != std::string::npos) return false;
    std::size_t begin{};
    for (;;) {
        const auto end = value.find('/', begin);
        const auto part = value.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == ".." || part.ends_with('.') || part.ends_with(' ')) return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}
Json summary(const Json& snapshot, bool adopted)
{
    Json result{{"adopted", adopted}};
    if (snapshot.is_null()) return result;
    result["revision"] = snapshot.at("revision");
    result["source"] = snapshot.at("bundle").at("source");
    result["commit"] = snapshot.at("bundle").at("commit");
    result["excluded_files"] = snapshot.at("bundle").at("excluded_files");
    result["files"] = Json::array();
    for (const auto& file : snapshot.at("bundle").at("files"))
        result["files"].push_back({{"path", file.at("path")}, {"bytes", file.at("content").get_ref<const std::string&>().size()}});
    result["review_accepted"] = adopted && snapshot.contains("review");
    result["enforcement"] = "Required review, approved write paths, prohibited paths, and exact approved tool calls. Semantic policy conformance requires the recorded human review.";
    if (snapshot.contains("review")) result["review"] = snapshot.at("review");
    return result;
}
}

class ProjectPolicyService::Impl final {
public:
    Impl(Contracts::IPolicySourceReader& source, Contracts::IAtomicFileStore& files,
        Contracts::IHasher& hasher, Contracts::IProjectRegistryRepository& projects, Paths paths, std::string protectedRoot)
        : source_{source}, files_{files}, hasher_{hasher}, projects_{projects}, paths_{std::move(paths)}, protectedRoot_{normalized(std::move(protectedRoot))} {}

    void validateRoots(const std::vector<Domain::PathText>& roots)
    {
        if (protectedRoot_.empty()) return;
        for (const auto& root : roots) {
            const auto path = normalized(root.value());
            if (below(path, protectedRoot_) || below(protectedRoot_, path))
                throw std::runtime_error{"Choose a project folder that does not overlap Forge Conductor's private data folder."};
        }
    }

    Json load(const PolicyStoragePaths& paths, const Domain::OperationContext& context)
    {
        auto content = files_.read(paths.read, Contracts::IAtomicFileStore::MaximumBytes, context);
        if (!content && content.error().code == Domain::ErrorCodes::RecordNotFound) return nullptr;
        const auto value = take(std::move(content));
        const auto snapshot = Json::parse(reinterpret_cast<const char*>(value.data()), reinterpret_cast<const char*>(value.data()) + value.size());
        if (snapshot.at("schema") != 1 || snapshot.at("revision") != digest(snapshot.at("bundle")))
            throw std::runtime_error{"The adopted policy snapshot failed integrity validation. Reimport and review it before writing."};
        return snapshot;
    }
    std::string digest(const Json& bundle) { return take(hasher_.sha256(bytes(bundle.dump()))).value(); }
    void save(const PolicyStoragePaths& paths, const Json& snapshot, bool exists, const Domain::OperationContext& context)
    {
        valid(context);
        const auto encoded = snapshot.dump();
        if (encoded.size() > Contracts::IAtomicFileStore::MaximumBytes) throw std::runtime_error{"Policy snapshot is too large to persist completely."};
        auto written = files_.replace(exists ? paths.write : paths.create, bytes(encoded), true, context);
        if (!written) throw std::runtime_error{written.error().message};
    }
    std::string execute(const Contracts::ProjectPolicyRequest& request, const Domain::OperationContext& context)
    {
        valid(context);
        validateRoots(take(projects_.descriptor(request.projectId, context)).aliases);
        if (request.action == Contracts::ProjectPolicyAction::Preview) {
            const auto imported = take(source_.read(request.source, context));
            Json bundle{{"source", imported.source}, {"commit", imported.commit},
                {"files", Json::array()}, {"excluded_files", imported.excludedFiles}};
            for (const auto& file : imported.files) bundle["files"].push_back({{"path", file.path}, {"content", file.content}});
            Json snapshot{{"schema", 1}, {"project", request.projectId.value()},
                {"revision", digest(bundle)}, {"bundle", std::move(bundle)}};
            std::lock_guard lock{mutex_};
            if (previews_.size() >= 8 && !previews_.contains(request.projectId.value()))
                throw std::runtime_error{"Eight policy previews are pending. Adopt a preview or restart the Manager before importing another."};
            previews_.insert_or_assign(request.projectId.value(), snapshot);
            return summary(snapshot, false).dump();
        }
        std::lock_guard lock{mutex_};
        const auto paths = take(paths_(request.projectId, context));
        auto current = load(paths, context);
        if (!current.is_null() && current.at("project") != request.projectId.value()) throw std::runtime_error{"Policy belongs to another project."};
        if (request.action == Contracts::ProjectPolicyAction::Inspect) return summary(current, !current.is_null()).dump();
        if (request.action == Contracts::ProjectPolicyAction::Adopt) {
            const auto found = previews_.find(request.projectId.value());
            if (found == previews_.end() || request.expectedRevision.empty() || found->second.at("revision") != request.expectedRevision)
                throw std::runtime_error{"Preview this exact policy revision before adopting it."};
            save(paths, found->second, !current.is_null(), context);
            const auto result = summary(found->second, true).dump();
            previews_.erase(found);
            return result;
        }
        if (request.action == Contracts::ProjectPolicyAction::ReadDocument &&
            (current.is_null() || current.at("revision") != request.expectedRevision)) {
            const auto preview = previews_.find(request.projectId.value());
            if (preview != previews_.end() && preview->second.at("revision") == request.expectedRevision) current = preview->second;
        }
        if (current.is_null() || request.expectedRevision.empty() || current.at("revision") != request.expectedRevision)
            throw std::runtime_error{"The adopted policy revision changed. Inspect it again before proceeding."};
        if (request.action == Contracts::ProjectPolicyAction::ReadDocument) {
            for (const auto& file : current.at("bundle").at("files")) {
                if (file.at("path") != request.source) continue;
                const auto& content = file.at("content").get_ref<const std::string&>();
                const auto window = request.reviewJson.empty() ? Json::object() : Json::parse(request.reviewJson);
                const auto offset = window.value("offset", std::size_t{});
                if (offset > content.size()) throw std::runtime_error{"Policy document offset is outside the snapshot."};
                auto end = std::min(content.size(), offset + 32768U);
                while (end < content.size() && (static_cast<unsigned char>(content[end]) & 0xc0U) == 0x80U) --end;
                if (offset < content.size() && (static_cast<unsigned char>(content[offset]) & 0xc0U) == 0x80U)
                    throw std::runtime_error{"Policy document offset must start at a UTF-8 character boundary."};
                return Json{{"revision", request.expectedRevision}, {"path", request.source},
                    {"content", content.substr(offset, end - offset)}, {"next_offset", end}, {"complete", end == content.size()}}.dump();
            }
            throw std::runtime_error{"That document is not in the adopted policy snapshot."};
        }
        if (request.action != Contracts::ProjectPolicyAction::Review) throw std::runtime_error{"Unknown policy operation."};
        auto review = Json::parse(request.reviewJson);
        validateReview(review, current);
        current["review"] = std::move(review);
        save(paths, current, true, context);
        return summary(current, true).dump();
    }
    void validateReview(const Json& review, const Json& current)
    {
        if (!review.is_object() || review.value("schema", 0) != 1 || review.value("accepted", false) != true ||
            review.value("policy_revision", "") != current.at("revision").get<std::string>() ||
            review.value("reviewer", "").empty() || review.value("evidence", "").empty() ||
            review.value("reviewed_at", "").empty() || !review.at("unresolved_obligations").is_array() || !review.at("unresolved_obligations").empty())
            throw std::runtime_error{"Accepted review requires this policy revision, reviewer, time, evidence, and no unresolved obligations."};
        const auto& coverage = review.at("source_coverage");
        if (!coverage.is_array()) throw std::runtime_error{"Record source coverage before accepting the review."};
        for (const auto& file : current.at("bundle").at("files")) {
            bool covered{};
            for (const auto& record : coverage) {
                if (record.at("path") != file.at("path")) continue;
                const auto status = record.value("status", "");
                covered = (status == "read" || status == "excluded") && !record.value("evidence_or_reason", "").empty();
            }
            if (!covered) throw std::runtime_error{"Review must account for each imported source file: " + file.at("path").get<std::string>()};
        }
        if (!current.at("bundle").at("excluded_files").empty() && review.value("non_text_review", "").empty())
            throw std::runtime_error{"Record the review or justified exclusion of the listed non-text source files."};
        for (const char* key : {"write_paths", "prohibited_paths"}) {
            if (!review.at(key).is_array() || review.at(key).size() > 128) throw std::runtime_error{"Review paths must be bounded arrays."};
            for (const auto& item : review.at(key)) {
                const auto path = normalized(item.get<std::string>());
                if (!(path == "." && std::string_view{key} == "write_paths") && !relative(path))
                    throw std::runtime_error{"Review paths must be explicit project-relative paths without traversal."};
            }
        }
        if (!review.at("approved_calls").is_array() || review.at("approved_calls").size() > 128)
            throw std::runtime_error{"Approved calls must be a bounded array of exact tool names and argument objects."};
        for (const auto& call : review.at("approved_calls"))
            if (call.value("tool", "") != "shell_exec" || !call.at("arguments").is_object() ||
                call.at("arguments").value("command", "").empty())
                throw std::runtime_error{"Approved commands must be exact shell_exec argument objects with a nonempty command."};
    }
    void check(const Domain::ToolAuthorizationRequest& request, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context)
    {
        valid(context);
        std::lock_guard lock{mutex_};
        validateRoots(authority.trustedRoots());
        const auto paths = take(paths_(authority.projectId(), context));
        const auto current = load(paths, context);
        if (current.is_null()) return;
        if (current.at("project") != authority.projectId().value()) throw std::runtime_error{"Policy belongs to another project."};
        if (request.effect == Domain::ToolEffect::Read) return;
        if (!current.contains("review")) throw std::runtime_error{"This project has an adopted policy awaiting review. Read-only work is available; accept its review before changing files or running commands."};
        const auto& review = current.at("review");
        validateReview(review, current);
        const auto arguments = Json::parse(request.call.canonicalArguments);
        for (const auto& approved : review.at("approved_calls"))
            if (approved.at("tool") == request.call.toolName && approved.at("arguments") == arguments) return;
        const auto& name = request.call.toolName;
        // These operations maintain project history, not arbitrary product files
        // or the separately persisted policy/review. Export remains gated.
        constexpr std::string_view bookkeeping[]{"memory_set", "memory_delete", "project_memory.remember",
            "project_memory.remember_batch", "project_memory.update", "project_memory.forget", "project_memory.link",
            "continuity.checkpoint", "continuity.prepare_handoff", "continuity.acknowledge_handoff",
            "continuity.request_rollover", "continuity.resume", "clu_cancel", "clu_start_handoff",
            "agent_run_start", "agent_run_status", "agent_run_complete"};
        if (std::find(std::begin(bookkeeping), std::end(bookkeeping), name) != std::end(bookkeeping)) return;
        if (name != "fs_write" && name != "fs_edit" && name != "fs_mkdir" && name != "pdf_write")
            throw std::runtime_error{"Policy requires an exact reviewed call for " + name + ". No command or tool-name wildcard is permitted."};
        auto path = normalized(arguments.at("path").get<std::string>());
        bool absolute = path.find(':') != std::string::npos || path.starts_with('/');
        if (absolute) {
            bool matched{};
            for (const auto& root : authority.trustedRoots()) {
                const auto base = normalized(root.value());
                if (path != base && below(path, base)) { path.erase(0, base.size() + 1); matched = true; break; }
            }
            if (!matched) throw std::runtime_error{"Policy write is outside the project roots."};
        }
        if (!relative(path) || below(path, ".git")) throw std::runtime_error{"Policy write path is not an ordinary project path."};
        for (const auto& denied : review.at("prohibited_paths"))
            if (below(path, normalized(denied.get<std::string>()))) throw std::runtime_error{"The adopted review prohibits this path."};
        for (const auto& allowed : review.at("write_paths")) {
            const auto root = normalized(allowed.get<std::string>());
            if (root == "." || below(path, root)) return;
        }
        throw std::runtime_error{"The write is outside the paths accepted by this policy review."};
    }
    Contracts::IPolicySourceReader& source_;
    Contracts::IAtomicFileStore& files_;
    Contracts::IHasher& hasher_;
    Contracts::IProjectRegistryRepository& projects_;
    Paths paths_;
    std::string protectedRoot_;
    std::mutex mutex_;
    std::map<std::string, Json> previews_;
};

ProjectPolicyService::ProjectPolicyService(Contracts::IPolicySourceReader& source, Contracts::IAtomicFileStore& files,
    Contracts::IHasher& hasher, Contracts::IProjectRegistryRepository& projects, Paths paths, std::string protectedRoot)
    : implementation_{std::make_unique<Impl>(source, files, hasher, projects, std::move(paths), std::move(protectedRoot))} {}
ProjectPolicyService::~ProjectPolicyService() = default;
Domain::Result<std::string> ProjectPolicyService::execute(const Contracts::ProjectPolicyRequest& request,
    const Domain::OperationContext& context) noexcept
{
    try { return Domain::Result<std::string>::success(implementation_->execute(request, context)); }
    catch (const std::exception& error) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, error.what())); }
    catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Policy operation failed safely.")); }
}
Domain::Result<void> ProjectPolicyService::check(const Domain::ToolAuthorizationRequest& request,
    const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& context) noexcept
{
    try { implementation_->check(request, authority, context); return Domain::Result<void>::success(); }
    catch (const std::exception& error) { return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized, error.what())); }
    catch (...) { return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized, "The project policy could not be verified.")); }
}
} // namespace ForgeConductor::Application
