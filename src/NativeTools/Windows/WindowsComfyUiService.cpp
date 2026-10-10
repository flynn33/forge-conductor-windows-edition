#include "ForgeConductor/NativeTools/Windows/WindowsComfyUiService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsComfyUiBackend.h"
#include "ForgeConductor/Domain/ComfyJobModels.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "NativeFileOperations.h"
#include "ProviderOperationLease.h"
#include "ComfyUiNativeSupport.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_set>

namespace ForgeConductor::NativeTools::Windows {
namespace {
using Json = nlohmann::json;
constexpr std::size_t MaximumJson = 8U * 1024U * 1024U;
constexpr std::size_t MaximumReceipt = 16U * 1024U * 1024U;
// MCP repeats structured content and escapes fragmented text twice. Keep the
// native projection below the transport's 1 MiB envelope limit in that case.
constexpr std::size_t MaximumPublicJobBytes = 128U * 1024U;
constexpr std::size_t MaximumInlinePreviewBytes = 32U * 1024U;
constexpr std::size_t PublicPageHeaderReserve = 1024U;
struct Failure { Domain::Error error; };
[[noreturn]] void reject(std::string_view code, std::string message) {
    throw Failure{Domain::makeError(code, std::move(message))};
}
template<class T> T take(Domain::Result<T> value) {
    if (!value) throw Failure{value.error()}; return std::move(value).value();
}
void take(Domain::Result<void> value) { if (!value) throw Failure{value.error()}; }
void check(const Domain::OperationContext& c) {
    take(Infrastructure::Windows::Detail::validateOperationContext(c,
        std::chrono::steady_clock::now(), "ComfyUI operation"));
}
std::span<const std::byte> bytes(std::string_view value) {
    return std::as_bytes(std::span{value.data(), value.size()});
}
std::string routeToken(std::string_view value) {
    constexpr char hex[]="0123456789ABCDEF"; std::string result;
    for(unsigned char c:value) {
        if((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_' || c=='-') result+=static_cast<char>(c);
        else { result+='%'; result+=hex[c>>4U]; result+=hex[c&15U]; }
    }
    return result;
}
Json parse(std::string_view value, std::size_t maximum = MaximumJson) {
    if (value.size() > maximum || !Domain::isValidUtf8(value) || value.find('\0') != std::string_view::npos)
        reject(Domain::ErrorCodes::PayloadTooLarge, "ComfyUI JSON exceeds its UTF-8 byte bound.");
    std::vector<std::unordered_set<std::string>> keys;
    return Json::parse(value, [&](int depth, Json::parse_event_t event, Json& item) {
        if (depth > 64) reject(Domain::ErrorCodes::LimitExceeded, "ComfyUI JSON exceeds depth 64.");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(item.get<std::string>()).second)
            reject(Domain::ErrorCodes::InvalidRequest, "ComfyUI JSON contains duplicate keys.");
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
std::string text(const Json& value, const char* key, bool required = true, std::size_t maximum = 32768U) {
    if (!value.contains(key)) { if (required) reject(Domain::ErrorCodes::InvalidRequest, std::string{key} + " is required."); return {}; }
    if (!value.at(key).is_string()) reject(Domain::ErrorCodes::InvalidRequest, std::string{key} + " must be text.");
    auto result = value.at(key).get<std::string>();
    if ((required && result.empty()) || result.size() > maximum || result.find('\0') != std::string::npos)
        reject(Domain::ErrorCodes::InvalidRequest, std::string{key} + " exceeds its text bound.");
    return result;
}
std::uint64_t integer(const Json& value, const char* key, std::uint64_t fallback, std::uint64_t maximum) {
    if (!value.contains(key)) return fallback;
    if (!value.at(key).is_number_integer() || (value.at(key).is_number_integer() &&
        !value.at(key).is_number_unsigned() && value.at(key).get<std::int64_t>() < 0))
        reject(Domain::ErrorCodes::InvalidRequest, std::string{key} + " must be a nonnegative integer.");
    auto result = value.at(key).get<std::uint64_t>();
    if (result > maximum) reject(Domain::ErrorCodes::InvalidRequest, std::string{key} + " exceeds its bound.");
    return result;
}
void fields(const Json& value, const std::set<std::string>& names) {
    if (!value.is_object()) reject(Domain::ErrorCodes::InvalidRequest, "ComfyUI arguments must be an object.");
    for (const auto& [key, unused] : value.items()) if (!names.contains(key))
        reject(Domain::ErrorCodes::InvalidRequest, "Unknown ComfyUI argument: " + key);
}
std::filesystem::path native(std::string_view value) {
    return std::filesystem::path{std::u8string_view{reinterpret_cast<const char8_t*>(value.data()), value.size()}};
}
Domain::PathText path(std::string_view value) {
    auto p = native(value);
    if (!p.is_absolute()) reject(Domain::ErrorCodes::InvalidRequest, "ComfyUI paths must be absolute.");
    auto u = p.make_preferred().u8string();
    return take(Domain::PathText::create({reinterpret_cast<const char*>(u.data()), u.size()}));
}
Json configJson(const Domain::ComfyUiConfig& c) {
    return {{"enabled",c.enabled},{"automatic_setup",c.automaticSetup},{"installation_path",c.installationPath},
        {"model_storage_path",c.modelStoragePath},{"endpoint",c.endpoint},{"download_budget_bytes",c.downloadBudgetBytes},
        {"free_space_reserve_bytes",c.freeSpaceReserveBytes},{"generation_timeout_seconds",c.generationTimeoutSeconds},
        {"quality_preference",c.qualityPreference}};
}
bool terminal(Domain::ComfyJobState state) {
    using State=Domain::ComfyJobState;
    return state==State::Completed || state==State::Failed || state==State::Cancelled || state==State::Unknown || state==State::AwaitingPreviewApproval;
}
std::uint64_t processCreation(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if(!::GetProcessTimes(process,&created,&exited,&kernel,&user)) reject(Domain::ErrorCodes::InternalFailure,"Cannot measure Manager process identity.");
    return (static_cast<std::uint64_t>(created.dwHighDateTime)<<32U)|created.dwLowDateTime;
}
bool liveOwner(const Json& r) {
    if(r.value("owner_released",false)) return false;
    auto process=::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,r.at("owner_pid").get<DWORD>());
    if(!process) {
        if(::GetLastError()==ERROR_INVALID_PARAMETER) return false;
        reject(Domain::ErrorCodes::Conflict,"The previous Manager owner cannot be verified; recovery is fenced.");
    }
    bool alive{};
    try { alive=::WaitForSingleObject(process,0U)==WAIT_TIMEOUT && processCreation(process)==r.at("owner_creation_time").get<std::uint64_t>(); }
    catch(...) { ::CloseHandle(process); throw; }
    ::CloseHandle(process); return alive;
}
Json outputIds(const Json& args, const char* key, const Json& graph) {
    auto ids = args.value(key, args.value("expected_outputs", Json::array()));
    if (!ids.is_array() || ids.size() > 256U) reject(Domain::ErrorCodes::InvalidRequest, "Expected output IDs must be a bounded array.");
    for (const auto& id : ids) if (!id.is_string() || !graph.contains(id.get<std::string>()))
        reject(Domain::ErrorCodes::InvalidRequest, "Expected output ID is absent from the graph.");
    return ids;
}
bool decodedMotion(const Json& artifact) {
    const auto type=artifact.value("media_type",std::string{});
    const auto metadata=artifact.value("metadata",Json::object());
    return metadata.is_object() && metadata.value("decoded",false) &&
        (type.starts_with("video/") || type=="image/gif" || type=="image/apng" || type=="image/webp") &&
        metadata.value("duration",0.0)>0.0 && metadata.value("frame_count",0ULL)>=2U;
}
void graphShape(const Json& graph) {
    if (!graph.is_object() || graph.empty() || graph.size() > 4096U || graph.dump().size() > MaximumJson)
        reject(Domain::ErrorCodes::InvalidRequest, "Workflow must be a bounded executable API graph.");
    for (const auto& [id, node] : graph.items()) {
        if (id.empty() || id.size() > 128U || id.find_first_of("/\\") != std::string::npos || !node.is_object())
            reject(Domain::ErrorCodes::InvalidRequest, "Workflow node ID or object is invalid.");
        (void)text(node, "class_type", true, 256U);
        if (!node.contains("inputs") || !node.at("inputs").is_object())
            reject(Domain::ErrorCodes::InvalidRequest, "Workflow node inputs must be an object.");
    }
}
std::string approvalText(std::string value) {
    auto normalized=take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(value));
    const auto punctuation=[](const wchar_t c) {
        WORD classification{};
        if(!::GetStringTypeW(CT_CTYPE1,&c,1,&classification)) reject(Domain::ErrorCodes::HostCapabilityUnavailable,"Windows approval character classification failed.");
        return (classification&(C1_SPACE|C1_PUNCT))!=0U;
    };
    std::size_t begin{},end=normalized.size();
    while(begin<end && punctuation(normalized[begin])) ++begin;
    while(end>begin && punctuation(normalized[end-1U])) --end;
    for(auto& c:normalized) if(c>=L'A' && c<=L'Z') c=static_cast<wchar_t>(c+32);
    return take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(std::wstring_view{normalized}.substr(begin,end-begin)));
}
Json boundedEvidence(const Json& value) {
    auto encoded=value.dump();
    if(encoded.size()<=16U*1024U) return value;
    encoded.resize(8U*1024U);
    while(!Domain::isValidUtf8(encoded)) encoded.pop_back();
    Json result{{"truncated",true},{"json_prefix",encoded},{"full_evidence","saved_in_private_receipt"}};
    if(value.is_object()) for(const auto* key:{"ok","code","message","downloaded_bytes","total","partial","previous_attempt","rollback_complete","published_count","publication_uncertain_count","verified_staging_count","publication_manifest"}) {
        if(!value.contains(key)) continue;
        const auto& field=value.at(key);
        if(field.dump().size()<=4U*1024U) result[key]=field;
    }
    return result;
}
Json publicArtifactMetadata(const Json& metadata) {
    Json result=Json::object();
    if(!metadata.is_object()) return result;
    for(const auto* key:{"decoded","width","height","frame_count","duration","codec","sample_rate","channels","frame_rate","fps","container"}) {
        if(!metadata.contains(key)) continue;
        const auto& value=metadata.at(key);
        if((value.is_boolean() || value.is_number() || value.is_string()) && value.dump().size()<=256U) result[key]=value;
    }
    return result;
}
Json publicArtifact(const Json& artifact,std::size_t& previewBytes) {
    if(!artifact.is_object()) reject(Domain::ErrorCodes::IntegrityFailure,"Durable artifact evidence must be an object.");
    Json result=Json::object();
    for(const auto* key:{"node_id","path","media_type","bytes","sha256","metadata_method","history_key","provider_media_type","provider_view_url","role","source_sha256","sampled_frames"})
        if(artifact.contains(key)) result[key]=artifact.at(key);
    if(artifact.contains("descriptor")) {
        Json descriptor=Json::object();const auto& source=artifact.at("descriptor");
        if(source.is_object()) for(const auto* key:{"filename","subfolder","type","format","frame_rate","animated"})
            if(source.contains(key) && source.at(key).is_primitive()) descriptor[key]=source.at(key);
        result["descriptor"]=std::move(descriptor);
    }
    if(artifact.contains("metadata")) {
        result["metadata"]=publicArtifactMetadata(artifact.at("metadata"));
        if(result.at("metadata")!=artifact.at("metadata")) result["metadata_details"]="saved_in_private_receipt";
    }
    if(artifact.contains("preview")) {
        const auto size=artifact.at("preview").dump().size();
        if(size<=MaximumInlinePreviewBytes-previewBytes) { result["preview"]=artifact.at("preview");previewBytes+=size; }
        else result["inline_preview_omitted"]=true;
    }
    return result;
}
}

class WindowsComfyUiService::Impl final {
    struct Entry {
        Json record;
        std::mutex mutex;
        std::condition_variable changed;
        std::thread worker;
        std::stop_source stop;
        std::atomic<bool> finished{true};
        std::atomic<bool> activeReserved{};
        std::mutex workerMutex;
        HANDLE lease{};
        ~Entry() { if (worker.joinable()) worker.join(); if(lease) ::CloseHandle(lease); }
    };
public:
    Impl(Contracts::IWorkspaceAuthority& authority, Contracts::IAtomicFileStore& files,
        Contracts::IConfigurationStore& configuration, Contracts::IWorkspaceAuthority& storageAuthority,
        const Contracts::WorkspaceAuthority& storageScope, Domain::PathText jobsRoot,
        Contracts::IUuidGenerator& uuids, Contracts::IClock& clock, Contracts::IHasher& hasher,
        std::shared_ptr<Contracts::IComfyUiBackend> backend)
        : authority_(authority), files_(files), configuration_(configuration), storageAuthority_(storageAuthority),
          storageScope_(storageScope), jobsRoot_(std::move(jobsRoot)), uuids_(uuids), clock_(clock), hasher_(hasher), backend_(std::move(backend)) {
        if (!backend_) backend_ = std::make_shared<WindowsComfyUiBackend>(authority_, jobsRoot_);
    }
    void observer(std::function<Domain::Result<std::string>(const Domain::ProjectId&, std::string_view, const Domain::OperationContext&)> value) {
        std::lock_guard lock{observerMutex_}; observer_ = std::move(value);
    }
    Domain::Result<std::string> execute(std::string_view name, std::string_view arguments,
        const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& c) noexcept {
        try {
            check(c); if (stopped_) reject(Domain::ErrorCodes::TransportClosed, "ComfyUI service is stopping.");
            auto args = parse(arguments); if (!args.is_object()) reject(Domain::ErrorCodes::InvalidRequest, "Arguments must be an object.");
            auto configuration = settings(c);
            Json result;
            if (name == "comfy_status") {
                fields(args, {});
                result = {{"ok",true},{"configured",configuration.enabled},{"available",false},{"configuration",configJson(configuration)}};
                if (configuration.enabled) {
                    auto status = backend_->perform("status", "{}", configuration, authority, c);
                    if (status) result.update(parse(status.value()));
                    else result["error"] = error(status.error());
                }
            } else if (!configuration.enabled && name!="comfy_job_status" && name!="comfy_job_list" && name!="comfy_job_cancel") reject(Domain::ErrorCodes::HostCapabilityUnavailable, "ComfyUI automation is disabled in Forge Settings.");
            else if (name == "comfy_catalog") result = catalog(args, configuration, authority, c);
            else if (name == "comfy_validate") {
                fields(args,{"workflow","expected_outputs"});
                result = preflight(args.at("workflow"), outputIds(args,"expected_outputs",args.at("workflow")), configuration, authority,c);
            } else if (name == "comfy_workflow") {
                fields(args,{"action","workflow","path","patches"});
                result = call("workflow", args, configuration, authority,c);
            } else if (name == "comfy_prepare" || name == "comfy_control") {
                fields(args,name == "comfy_prepare" ? std::set<std::string>{"workflow","dependencies","timeout_sec"} : std::set<std::string>{"action","timeout_sec"});
                if (args.contains("workflow")) graphShape(args.at("workflow"));
                if (name == "comfy_control" && text(args,"action") != "start" && text(args,"action") != "stop" && text(args,"action") != "restart")
                    reject(Domain::ErrorCodes::InvalidRequest,"Control action must be start, stop or restart.");
                requireExecute(authority);
                result = startOperation(name == "comfy_prepare" ? "prepare" : "control",args,configuration,authority,c);
            } else if (name == "comfy_run") result = render(args,configuration,authority,c);
            else if (name == "comfy_job_list") result = list(args,authority,c);
            else if (name == "comfy_job_status" || name == "comfy_job_cancel" || name == "comfy_job_resume") {
                fields(args,name == "comfy_job_status" ? std::set<std::string>{"job_id","wait_sec"} : std::set<std::string>{"job_id"});
                auto entry = lookup(text(args,"job_id"),authority,c);
                if (name == "comfy_job_cancel") cancel(entry,authority,c);
                else if (name == "comfy_job_resume") resume(entry,configuration,authority,c);
                else {
                    auto wait = integer(args,"wait_sec",0U,60U);
                    std::unique_lock lock{entry->mutex};
                    auto end = (std::min)(c.deadline,std::chrono::steady_clock::now()+std::chrono::seconds{wait});
                    while (!entry->finished && std::chrono::steady_clock::now()<end) { check(c); entry->changed.wait_for(lock,std::chrono::milliseconds{50}); }
                }
                result = snapshot(entry,authority,c);
            } else reject(Domain::ErrorCodes::InvalidRequest,"Unknown ComfyUI tool.");
            return Domain::Result<std::string>::success(result.dump());
        } catch(const Failure& e) { return Domain::Result<std::string>::failure(e.error); }
        catch(const Json::exception& e) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest,"Invalid ComfyUI JSON shape: " + std::string{e.what()})); }
        catch(const std::exception& e) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,e.what())); }
        catch(...) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,"ComfyUI operation failed.")); }
    }
    void shutdown() noexcept {
        if(stopped_.exchange(true)) return;
        std::vector<std::shared_ptr<Entry>> entries;
        { std::lock_guard lock{entriesMutex_}; for(auto& [key,e]:entries_) entries.push_back(e); }
        for(auto& e:entries) e->stop.request_stop();
        for(auto& e:entries) join(e);
        backend_->shutdown();
    }
private:
    static Json error(const Domain::Error& e) { return {{"code",e.code},{"message",e.message},{"retryable",e.retryable}}; }
    std::string hash(std::string_view s) { return take(hasher_.sha256(bytes(s))).value(); }
    Domain::ComfyUiConfig settings(const Domain::OperationContext& c) {
        auto config = take(configuration_.reload(c)).comfyUi; take(Domain::validateComfyUiConfig(config)); return config;
    }
    Json call(std::string_view operation,const Json& args,const Domain::ComfyUiConfig& config,
        const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& c) {
        check(c); auto result = parse(take(backend_->perform(operation,args.dump(),config,authority,c))); check(c); return result;
    }
    Json request(const char* method,const std::string& route,const Json& body,const Domain::ComfyUiConfig& config,
        const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& c) {
        return call("request",{{"method",method},{"route",route},{"body",body}},config,authority,c);
    }
    void requireExecute(const Contracts::WorkspaceAuthority& a) {
        if(!a.shellEnabled() || std::find(a.grants().begin(),a.grants().end(),Domain::FileAccess::Execute)==a.grants().end() ||
            std::find(a.denials().begin(),a.denials().end(),Domain::FileAccess::Execute)!=a.denials().end())
            reject(Domain::ErrorCodes::Unauthorized,"ComfyUI operations require current Execute permission and enabled external-process policy.");
    }
    void validateNativeUserEvidence(const Json& observation) {
        (void)text(observation,"conversation_id");
        if(!observation.contains("user_messages") || !observation.at("user_messages").is_array())
            reject(Domain::ErrorCodes::IntegrityFailure,"Native conversation user-message evidence is incomplete.");
        std::optional<std::uint64_t> last;
        for(const auto& message:observation.at("user_messages")) {
            (void)text(message,"text",false,MaximumJson);
            const auto index=integer(message,"message_index",UINT64_MAX,UINT64_MAX);
            if(index==UINT64_MAX || (last && index<=*last) || !message.contains("selected_version"))
                reject(Domain::ErrorCodes::IntegrityFailure,"Native user message positions or selected versions are invalid.");
            (void)integer(message,"selected_version",0U,UINT64_MAX);last=index;
            if(message.contains("forge_generated") && !message.at("forge_generated").is_boolean())
                reject(Domain::ErrorCodes::IntegrityFailure,"Native user-message attribution is invalid.");
        }
    }
    Json observe(const Domain::ProjectId& project, const Domain::OperationContext& c, std::string_view boundConversation={}) {
        std::function<Domain::Result<std::string>(const Domain::ProjectId&, std::string_view, const Domain::OperationContext&)> callback;
        { std::lock_guard lock{observerMutex_}; callback=observer_; }
        if(!callback) reject(Domain::ErrorCodes::HostCapabilityUnavailable,"Preview approval requires the bound native LM Studio conversation.");
        auto result = parse(take(callback(project,boundConversation,c)));
        validateNativeUserEvidence(result);
        if(result.contains("predecessor_conversation")) validateNativeUserEvidence(result.at("predecessor_conversation"));
        if(result.contains("bound_conversation")) validateNativeUserEvidence(result.at("bound_conversation"));
        return result;
    }
    Json catalog(const Json& args,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        fields(args,{"kind","name","category","filter","offset","limit"});
        auto kind=text(args,"kind");
        const auto offset=integer(args,"offset",0U,100000U),limit=integer(args,"limit",32U,128U);
        if(!limit) reject(Domain::ErrorCodes::InvalidRequest,"Catalog limit must be positive.");
        if(kind=="workflows" || kind=="templates") {
            auto requestArgs=args;requestArgs["offset"]=offset;requestArgs["limit"]=limit;
            auto result=call("catalog",requestArgs,config,a,c);const auto entries=result.at("items");
            if(!entries.is_array()) reject(Domain::ErrorCodes::IntegrityFailure,"Workflow catalog items must be an array.");
            for(auto& [key,value]:result.items()) if(key!="items") value=boundedEvidence(value);
            result["items"]=Json::array();const auto total=integer(result,"total",offset+entries.size(),1000000U);
            if(result.dump().size()>MaximumPublicJobBytes-PublicPageHeaderReserve) reject(Domain::ErrorCodes::PayloadTooLarge,"Catalog metadata exceeds its delivery bound.");
            for(const auto& entry:entries) {
                if(result.dump().size()+entry.dump().size()+2U>MaximumPublicJobBytes-PublicPageHeaderReserve) {
                    if(result.at("items").empty()) reject(Domain::ErrorCodes::PayloadTooLarge,"One workflow catalog entry exceeds its delivery bound.");break;
                }
                result["items"].push_back(entry);
            }
            const auto next=offset+result.at("items").size();result["offset"]=offset;result["has_more"]=next<total;result["next_offset"]=next<total?Json(next):Json(nullptr);
            if(result.dump().size()>MaximumPublicJobBytes) reject(Domain::ErrorCodes::PayloadTooLarge,"Workflow catalog page exceeds its delivery bound.");
            return result;
        }
        const auto name=text(args,"name",false,256U),category=text(args,"category",false,256U);
        Json raw;
        if(kind=="nodes") raw=request("GET",name.empty()?"/object_info":"/object_info/"+routeToken(name),Json::object(),config,a,c);
        else if(kind=="models") {
            if(!name.empty() && !category.empty() && name!=category)
                reject(Domain::ErrorCodes::InvalidRequest,"Model catalog name and category select different provider folders.");
            const auto& folder=category.empty()?name:category;
            raw=request("GET",folder.empty()?"/models":"/models/"+routeToken(folder),Json::object(),config,a,c);
        }
        else reject(Domain::ErrorCodes::InvalidRequest,"Catalog kind must be nodes, models, workflows or templates.");
        auto filter=text(args,"filter",false,1024U);
        Json entries=Json::array();
        if(raw.is_object()) for(const auto& [key,value]:raw.items()) {
            if(!filter.empty() && key.find(filter)==std::string::npos) continue;
            if(kind=="nodes" && !category.empty() && value.value("category",std::string{}).find(category)==std::string::npos) continue;
            entries.push_back({{"name",key},{"schema",value}});
        } else if(raw.is_array()) for(const auto& item:raw) if(filter.empty() || item.dump().find(filter)!=std::string::npos) entries.push_back(item);
        Json page=Json::array(); auto end=(std::min)(entries.size(),static_cast<std::size_t>(offset+limit));
        for(auto i=static_cast<std::size_t>(offset);i<end;++i) {
            if(page.dump().size()+entries[i].dump().size()+2U>MaximumPublicJobBytes-PublicPageHeaderReserve) { if(page.empty()) reject(Domain::ErrorCodes::PayloadTooLarge,"One node schema exceeds the catalog page bound."); break; }
            page.push_back(entries[i]);
        }
        auto next=offset+page.size();
        return {{"ok",true},{"kind",kind},{"entries",page},{"offset",offset},{"next_offset",next<entries.size()?Json(next):Json(nullptr)},{"has_more",next<entries.size()},{"total",entries.size()}};
    }
    Json preflight(const Json& graph,Json expected,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        graphShape(graph); Json issues=Json::array(),warnings=Json::array(),schemas=Json::object(),outputs=Json::array(),providerInputs=Json::array();
        for(const auto& [id,node]:graph.items()) {
            auto type=text(node,"class_type",true,256U);
            if(!schemas.contains(type)) schemas[type]=request("GET","/object_info/"+routeToken(type),Json::object(),config,a,c);
            const auto& info=schemas.at(type);
            if(!info.is_object() || !info.contains(type)) { issues.push_back({{"node_id",id},{"code","missing_node"},{"class_type",type}}); continue; }
            const auto& schema=info.at(type);
            if(schema.value("output_node",false)) outputs.push_back(id);
            const auto& inputs=node.at("inputs");
            const auto definitions=schema.value("input",Json::object());
            const auto required=definitions.value("required",Json::object());
            for(const auto& [key,definition]:required.items())
                if(!inputs.contains(key)) issues.push_back({{"node_id",id},{"input",key},{"code","missing_input"}});
            for(const auto& [key,value]:inputs.items()) {
                if(value.is_array()) {
                    if(value.size()!=2U || !value[0].is_string() || (!value[1].is_number() && !value[1].is_boolean())) {
                        issues.push_back({{"node_id",id},{"input",key},{"code","literal_array_requires_wrapper"}});continue;
                    }
                    if(value.size()!=2U || !value[0].is_string() || !value[1].is_number_integer() || !graph.contains(value[0].get<std::string>()))
                        issues.push_back({{"node_id",id},{"input",key},{"code","invalid_link"}});
                    else if(value[1].get<std::int64_t>()<0) issues.push_back({{"node_id",id},{"input",key},{"code","invalid_output_index"}});
                    else {
                        const auto sourceType=text(graph.at(value[0].get<std::string>()),"class_type",true,256U);
                        if(!schemas.contains(sourceType)) schemas[sourceType]=request("GET","/object_info/"+routeToken(sourceType),Json::object(),config,a,c);
                        if(schemas[sourceType].contains(sourceType)) {
                            const auto returned=schemas[sourceType][sourceType].value("output",Json::array());
                            if(!returned.is_array() || value[1].get<std::uint64_t>()>=returned.size())
                                issues.push_back({{"node_id",id},{"input",key},{"code","invalid_output_index"}});
                        }
                    }
                    continue;
                }
                const auto& literal=value.is_object() && value.contains("__value__")?value.at("__value__"):value;
                if(value.is_object() && value.contains("__value__")) {
                    if(value.size()!=1U) {issues.push_back({{"node_id",id},{"input",key},{"code","unsupported_literal_wrapper"}});continue;}
                    if(literal.is_array() && literal.size()==2U && literal[0].is_string() && (literal[1].is_number() || literal[1].is_boolean())) {
                        issues.push_back({{"node_id",id},{"input",key},{"code","unsupported_literal_link_shape"}});continue;
                    }
                }
                Json definition;
                for(const auto* group:{"required","optional"}) if(definitions.contains(group) && definitions[group].contains(key)) definition=definitions[group][key];
                if(!definition.is_array() || definition.empty()) continue;
                const bool providerFile=definition.size()>1U && definition[1].is_object() &&
                    (definition[1].value("image_upload",false) || definition[1].value("audio_upload",false) || definition[1].value("video_upload",false));
                if(literal.is_string() && providerFile)
                    providerInputs.push_back({{"node_id",id},{"input",key},{"value",literal}});
                if(definition[0].is_array() && std::find(definition[0].begin(),definition[0].end(),literal)==definition[0].end()) {
                    if(literal.is_string() && providerFile)
                        warnings.push_back({{"node_id",id},{"input",key},{"code","provider_file_validation_required"},{"value",literal}});
                    else issues.push_back({{"node_id",id},{"input",key},{"code","missing_choice"},{"value",literal}});
                }
                if(definition[0].is_string()) {
                    const auto t=definition[0].get<std::string>();
                    if((t=="INT" && !literal.is_number_integer()) || (t=="FLOAT" && !literal.is_number()) ||
                        (t=="BOOLEAN" && !literal.is_boolean()) || (t=="STRING" && !literal.is_string()))
                        issues.push_back({{"node_id",id},{"input",key},{"code","invalid_type"}});
                    if(literal.is_number() && definition.size()>1U && definition[1].is_object()) {
                        const auto& bounds=definition[1]; auto v=literal.get<double>();
                        if((bounds.contains("min") && bounds["min"].is_number() && v<bounds["min"].get<double>()) || (bounds.contains("max") && bounds["max"].is_number() && v>bounds["max"].get<double>()))
                            issues.push_back({{"node_id",id},{"input",key},{"code","out_of_range"}});
                    }
                }
            }
        }
        if(expected.empty()) expected=outputs;
        if(expected.empty()) issues.push_back({{"code","missing_output_node"}});
        for(const auto& id:expected) if(std::find(outputs.begin(),outputs.end(),id)==outputs.end()) issues.push_back({{"node_id",id},{"code","not_an_output_node"}});
        Json motionOutputs=Json::array();bool mediaInspectionRequired=false;
        for(const auto& output:expected) {
            const auto id=output.get<std::string>();if(!graph.contains(id)) continue;
            const auto& node=graph.at(id);const auto type=node.at("class_type").get<std::string>();
            if(!schemas.contains(type) || !schemas.at(type).contains(type)) continue;
            const auto& schema=schemas.at(type).at(type);bool motion=false;
            if(type=="SaveAnimatedPNG" || type=="SaveAnimatedWEBP") mediaInspectionRequired=true;
            for(const auto& returned:schema.value("output",Json::array())) if(returned=="VIDEO") motion=true;
            const auto definitions=schema.value("input",Json::object());
            for(const auto& [input,value]:node.at("inputs").items()) {
                const auto& literal=value.is_object() && value.size()==1U && value.contains("__value__")?value.at("__value__"):value;
                for(const auto* group:{"required","optional"}) if(definitions.contains(group) && definitions.at(group).contains(input)) {
                    const auto& contract=definitions.at(group).at(input);
                    if(!contract.is_array() || contract.empty()) continue;
                    if(contract[0]=="VIDEO") motion=true;
                    if(contract[0].is_array() && literal.is_string() && literal.get_ref<const std::string&>().starts_with("video/") &&
                        std::find(contract[0].begin(),contract[0].end(),literal)!=contract[0].end()) motion=true;
                }
                if(value.is_array() && value.size()==2U && value[0].is_string() && value[1].is_number_integer() && value[1].get<std::int64_t>()>=0 && graph.contains(value[0].get<std::string>())) {
                    const auto source=graph.at(value[0].get<std::string>()).at("class_type").get<std::string>();
                    if(schemas.contains(source) && schemas.at(source).contains(source)) {
                        const auto returned=schemas.at(source).at(source).value("output",Json::array());const auto index=value[1].get<std::uint64_t>();
                        if(returned.is_array() && index<returned.size() && returned[index]=="VIDEO") motion=true;
                    }
                }
            }
            if(motion) {motionOutputs.push_back(id);mediaInspectionRequired=true;}
        }
        return {{"ok",true},{"validation_kind","preflight"},{"server_validation_performed",false},{"ready",issues.empty()},
            {"issues",issues},{"warnings",warnings},{"expected_outputs",expected},{"motion_output_nodes",motionOutputs},
            {"media_inspection_required",mediaInspectionRequired},
            {"provider_file_inputs",providerInputs},{"workflow_sha256",hash(graph.dump())}};
    }
    Domain::PathText storedPath(const Json& r) {
        auto p=native(jobsRoot_.value())/r.at("project_id").get<std::string>()/r.at("job_id").get<std::string>()/"receipt.json";
        auto u=p.u8string(); return path({reinterpret_cast<const char*>(u.data()),u.size()});
    }
    void persist(const Json& r,const Domain::OperationContext& c) {
        auto encoded=r.dump(); if(encoded.size()>MaximumReceipt) reject(Domain::ErrorCodes::PayloadTooLarge,"ComfyUI receipt exceeds 16 MiB.");
        auto receipt=Json{{"payload",r},{"sha256",hash(encoded)}}.dump();
        auto readAccess=take(storageAuthority_.authorize(storageScope_,{storedPath(r),std::nullopt,Domain::FileAccess::Read,false},c));
        auto old=files_.read(readAccess,MaximumReceipt+1024U,c);
        if(!old && old.error().code!=Domain::ErrorCodes::RecordNotFound) throw Failure{old.error()};
        auto target=take(storageAuthority_.authorize(storageScope_,{storedPath(r),std::nullopt,old?Domain::FileAccess::Write:Domain::FileAccess::Create,!old},c));
        take(Detail::ensureAuthorizedParentDirectories(target,c)); take(files_.replace(target,bytes(receipt),false,c));
    }
    void scope(const Json& r,const Contracts::WorkspaceAuthority& a,bool effects) {
        if(r.at("project_id")!=a.projectId().value() || r.at("principal")!=a.callerId().value())
            reject(Domain::ErrorCodes::ProjectScopeMismatch,"ComfyUI job belongs to another project or authenticated principal.");
        for(const auto& root:r.at("scope").at("roots")) if(std::none_of(a.trustedRoots().begin(),a.trustedRoots().end(),[&](const auto& p){return p.value()==root.get<std::string>();}))
            reject(Domain::ErrorCodes::PathOutsideAuthority,"ComfyUI job root is outside the current scope.");
        for(const auto& value:r.at("scope").at("grants")) {
            auto grant=static_cast<Domain::FileAccess>(value.get<int>());
            if(!effects && grant!=Domain::FileAccess::Read) continue;
            if(std::find(a.grants().begin(),a.grants().end(),grant)==a.grants().end() || std::find(a.denials().begin(),a.denials().end(),grant)!=a.denials().end())
                reject(Domain::ErrorCodes::Unauthorized,"ComfyUI job permission was revoked.");
        }
        if(effects && r.at("scope").value("shell_enabled",false) && !a.shellEnabled()) reject(Domain::ErrorCodes::ShellDisabled,"External-process permission was revoked.");
    }
    Contracts::WorkspaceAuthority fresh(const Json& r,const Domain::OperationContext& c) {
        auto a=take(authority_.authorityFor(take(Domain::ProjectId::parse(r.at("project_id").get<std::string>())),c)); scope(r,a,true);
        std::vector<Domain::PathText> roots; for(const auto& root:r.at("scope").at("roots")) roots.push_back(path(root.get<std::string>()));
        std::vector<Domain::FileAccess> grants; for(const auto& g:r.at("scope").at("grants")) grants.push_back(static_cast<Domain::FileAccess>(g.get<int>()));
        return take(authority_.narrow(a,roots,grants,r.at("scope").value("shell_enabled",false),a.generation()+1U,c));
    }
    Domain::OperationContext workerContext(const Json& r,std::stop_token stop={},std::uint32_t timeout=30U) {
        return {take(Domain::OperationId::parse(r.at("job_id").get<std::string>())),clock_.monotonicNow()+std::chrono::seconds{timeout},stop,
            take(Domain::CorrelationId::parse("comfy-job-"+r.at("job_id").get<std::string>()))};
    }
    void lease(const std::shared_ptr<Entry>& e,const std::string& project,const std::string& id) {
        const auto name=std::string{"Local\\ForgeConductor.ComfyJob."}+project+"."+id;
        const std::wstring wide{name.begin(),name.end()};
        auto handle=::CreateSemaphoreW(nullptr,1,1,wide.c_str());
        if(!handle) reject(Domain::ErrorCodes::Conflict,"Cannot acquire the durable ComfyUI job lease.");
        if(::WaitForSingleObject(handle,0U)!=WAIT_OBJECT_0) {
            ::CloseHandle(handle); reject(Domain::ErrorCodes::Conflict,"Another Manager holds the durable ComfyUI job lease.");
        }
        e->lease=handle;
    }
    void reserveActive(const std::shared_ptr<Entry>& e) {
        auto active=activeJobs_.load();
        do { if(active>=16U) reject(Domain::ErrorCodes::LimitExceeded,"ComfyUI has 16 admitted active jobs."); }
        while(!activeJobs_.compare_exchange_weak(active,active+1U));
        e->activeReserved=true;e->finished=false;
    }
    void releaseActive(const std::shared_ptr<Entry>& e) {
        if(e->activeReserved.exchange(false)) --activeJobs_;
        e->finished=true;e->changed.notify_all();
    }
    std::shared_ptr<Entry> create(const std::string& kind,const Domain::ComfyUiConfig& config,
        const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c,const std::function<void(Json&)>& initialize) {
        auto e=std::make_shared<Entry>(); Json roots=Json::array(),grants=Json::array();
        for(const auto& p:a.trustedRoots()) roots.push_back(p.value()); for(auto g:a.grants()) grants.push_back(static_cast<int>(g));
        auto id=take(uuids_.next()).value();
        lease(e,a.projectId().value(),id);
        e->record={{"schema_version",1U},{"kind","forge_comfy_job"},{"job_id",id},{"plan_id",id},{"project_id",a.projectId().value()},
            {"principal",a.callerId().value()},{"operation",kind},{"stage",kind=="render"?"preview":kind},{"state","before_dispatch"},{"phase","waiting_provider_lease"},
            {"configuration",configJson(config)},{"scope",{{"roots",roots},{"grants",grants},{"shell_enabled",a.shellEnabled()}}},
            {"created_utc_ms",std::chrono::duration_cast<std::chrono::milliseconds>(clock_.utcNow().time_since_epoch()).count()},
            {"revision",1U},{"prompt_id",nullptr},{"submission_acknowledged",false},{"publication_suppressed",false},
            {"cancellation_requested",false},{"remote_state","not_submitted"},{"error",nullptr},{"artifacts",Json::array()},
            {"preview_artifacts",Json::array()},{"owner_pid",::GetCurrentProcessId()},
            {"owner_creation_time",processCreation(::GetCurrentProcess())},{"owner_released",false},{"downloaded_bytes",0U}};
        initialize(e->record);
        std::lock_guard lock{entriesMutex_};
        if(entries_.size()>=256U) {
            for(auto it=entries_.begin();it!=entries_.end() && entries_.size()>=256U;) {
                if(it->second->finished) { join(it->second); it=entries_.erase(it); } else ++it;
            }
            if(entries_.size()>=256U) reject(Domain::ErrorCodes::LimitExceeded,"ComfyUI job cache is full.");
        }
        reserveActive(e);
        try { persist(e->record,c);entries_.emplace(a.projectId().value()+"/"+id,e); }
        catch(...) { releaseActive(e);throw; }
        return e;
    }
    void join(const std::shared_ptr<Entry>& e) { std::lock_guard lock{e->workerMutex}; if(e->worker.joinable()) e->worker.join(); }
    Domain::ComfyJobRecord decodedCore(const Json& r) {
        try {
            const auto requiredInteger=[&](const Json& value,const char* key,std::uint64_t maximum=UINT64_MAX) {
                if(!value.contains(key)) reject(Domain::ErrorCodes::IntegrityFailure,std::string{"Receipt is missing "}+key+'.');
                return integer(value,key,0U,maximum);
            };
            const auto requiredBoolean=[&](const Json& value,const char* key) {
                if(!value.contains(key) || !value.at(key).is_boolean()) reject(Domain::ErrorCodes::IntegrityFailure,std::string{"Receipt boolean is invalid: "}+key);
            };
            const auto canonicalId=[&](const Json& value,const char* key) {
                const auto encoded=text(value,key,true,36U);const auto id=take(Domain::Uuid::parse(encoded)).value();
                if(id!=encoded) reject(Domain::ErrorCodes::IntegrityFailure,std::string{"Receipt UUID is not canonical: "}+key);return id;
            };
            const auto seal=[&](const Json& value,const char* key) { return take(Domain::Sha256Digest::parse(text(value,key,true,64U))).value(); };
            if(!r.is_object() || requiredInteger(r,"schema_version")!=1U || text(r,"kind")!="forge_comfy_job") reject(Domain::ErrorCodes::IntegrityFailure,"Unsupported ComfyUI receipt version or kind.");
            Domain::ComfyJobRecord core;core.kind="forge_comfy_job";core.jobId=canonicalId(r,"job_id");core.projectId=canonicalId(r,"project_id");
            if(canonicalId(r,"plan_id")!=core.jobId) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt plan ID differs from its job ID.");
            (void)take(Domain::ClientId::parse(text(r,"principal",true,256U)));
            core.operation=text(r,"operation");const auto stage=text(r,"stage");
            if((core.operation!="render" && core.operation!="prepare" && core.operation!="control") ||
                (core.operation=="render"?(stage!="preview" && stage!="final"):stage!=core.operation)) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt operation or stage is invalid.");
            const std::map<std::string,Domain::ComfyJobState> states{{"preparing",Domain::ComfyJobState::Preparing},{"before_dispatch",Domain::ComfyJobState::BeforeDispatch},
                {"submitting",Domain::ComfyJobState::Submitting},{"queued",Domain::ComfyJobState::Queued},{"running",Domain::ComfyJobState::Running},{"fetching",Domain::ComfyJobState::Fetching},
                {"awaiting_preview_approval",Domain::ComfyJobState::AwaitingPreviewApproval},{"completed",Domain::ComfyJobState::Completed},{"failed",Domain::ComfyJobState::Failed},
                {"cancelled",Domain::ComfyJobState::Cancelled},{"unknown",Domain::ComfyJobState::Unknown}};
            const auto state=states.find(text(r,"state"));if(state==states.end()) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt state is unknown to version 1.");core.state=state->second;
            if(r.contains("phase")) {
                const auto phase=text(r,"phase",true,64U);const std::set<std::string> phases{"waiting_provider_lease","starting_provider","uploading_inputs","preflight","preparing_dependencies","controlling_provider",
                    "sealing_dependencies","waiting_provider_queue","verifying_submission","reconciling_provider","collecting_artifacts"};
                if(!states.contains(phase)&&!phases.contains(phase)) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt phase is unknown to version 1.");
            }
            core.downloadedBytes=requiredInteger(r,"downloaded_bytes");if(!requiredInteger(r,"revision")) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt revision must be positive.");
            if(!requiredInteger(r,"owner_pid",MAXDWORD) || !requiredInteger(r,"owner_creation_time")) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt process identity is invalid.");
            for(const auto* key:{"owner_released","submission_acknowledged","publication_suppressed","cancellation_requested"}) requiredBoolean(r,key);
            if(r.contains("preview_revision_requested")) requiredBoolean(r,"preview_revision_requested");
            if(r.value("preview_revision_requested",false)) {
                if(core.operation!="render" || stage!="preview" || !r.contains("preview_revision_evidence") || !r.at("preview_revision_evidence").is_object() || r.at("preview_revision_evidence").size()!=2U)
                    reject(Domain::ErrorCodes::IntegrityFailure,"Revision-invalidated receipt has no bounded native message evidence.");
                (void)requiredInteger(r.at("preview_revision_evidence"),"message_index");(void)requiredInteger(r.at("preview_revision_evidence"),"selected_version");
            } else if(r.contains("preview_revision_evidence")) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt revision evidence has no invalidation flag.");
            if(r.contains("preview_recovery_requires_new_preview")) requiredBoolean(r,"preview_recovery_requires_new_preview");
            if(r.value("preview_recovery_requires_new_preview",false)) {
                if(core.operation!="render" || stage!="preview" || !r.contains("preview_recovery_error") || !r.at("preview_recovery_error").is_object() || r.at("preview_recovery_error").size()!=2U)
                    reject(Domain::ErrorCodes::IntegrityFailure,"Invalidated preview recovery lacks bounded failure evidence.");
                (void)text(r.at("preview_recovery_error"),"code",true,128U);(void)text(r.at("preview_recovery_error"),"message",true,4096U);
            } else if(r.contains("preview_recovery_error")) reject(Domain::ErrorCodes::IntegrityFailure,"Preview recovery evidence has no invalidation flag.");
            if(!r.contains("configuration") || !r.at("configuration").is_object() || !r.contains("scope") || !r.at("scope").is_object()) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt configuration or scope is invalid.");
            const auto& scope=r.at("scope");requiredBoolean(scope,"shell_enabled");
            if(!scope.contains("roots") || !scope.at("roots").is_array() || scope.at("roots").empty() || scope.at("roots").size()>256U ||
                !scope.contains("grants") || !scope.at("grants").is_array() || scope.at("grants").size()>5U) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt scope collections are invalid.");
            for(const auto& root:scope.at("roots")) {if(!root.is_string()) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt root must be text.");(void)path(root.get_ref<const std::string&>());}
            std::set<std::uint64_t> grants;for(const auto& grant:scope.at("grants")) {const Json holder{{"grant",grant}};const auto value=requiredInteger(holder,"grant",static_cast<std::uint64_t>(Domain::FileAccess::Execute));if(!grants.insert(value).second) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt grants contain duplicates.");}
            if(!r.contains("prompt_id") || (!r.at("prompt_id").is_null() && !r.at("prompt_id").is_string())) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt prompt ID must be null or an exact UUID.");
            if(r.at("prompt_id").is_string()) core.promptId=canonicalId(r,"prompt_id");
            if(r.contains("graph_sha256")) core.graphSha256=seal(r,"graph_sha256");
            if(!core.promptId.empty() && (core.operation!="render" || core.graphSha256.empty() || !r.contains("graph"))) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt submission has no exact sealed render graph.");
            if(r.contains("graph")) {graphShape(r.at("graph"));if(!core.graphSha256.empty() && hash(r.at("graph").dump())!=core.graphSha256) reject(Domain::ErrorCodes::IntegrityFailure,"Submitted graph differs from its receipt seal.");}
            const auto artifacts=[&](const char* key) {
                if(!r.contains(key) || !r.at(key).is_array() || r.at(key).size()>512U) reject(Domain::ErrorCodes::IntegrityFailure,std::string{"Receipt artifact collection is invalid: "}+key);
                std::vector<Domain::ComfyArtifact> decoded;
                for(const auto& item:r.at(key)) {
                    Domain::ComfyArtifact artifact;artifact.nodeId=text(item,"node_id",true,128U);artifact.path=text(item,"path");(void)path(artifact.path);
                    artifact.mediaType=text(item,"media_type",true,256U);if(artifact.mediaType.find('/')==std::string::npos || artifact.mediaType.find_first_of(" \t\r\n")!=std::string::npos) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt artifact media type is invalid.");
                    artifact.sha256=seal(item,"sha256");artifact.bytes=requiredInteger(item,"bytes");
                    if(item.contains("metadata")) {if(!item.at("metadata").is_object()) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt artifact metadata must be an object.");artifact.metadataJson=item.at("metadata").dump();}
                    decoded.push_back(std::move(artifact));
                }
                return decoded;
            };
            core.artifacts=artifacts("artifacts");const auto previews=artifacts("preview_artifacts");
            if(core.operation=="render") {
                Domain::ComfyRenderPlan plan;plan.planId=core.jobId;plan.revision=requiredInteger(r,"revision");
                if(r.contains("input_seals")) {
                    if(!r.at("input_seals").is_array() || r.at("input_seals").size()>64U) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt input seals are invalid.");
                    for(const auto& input:r.at("input_seals")) {
                        (void)path(text(input,"path"));(void)seal(input,"sha256");(void)requiredInteger(input,"bytes");
                        if(input.contains("provider_sha256")) { (void)seal(input,"provider_sha256");(void)requiredInteger(input,"provider_bytes");
                            (void)text(input,"node_id",true,128U);(void)text(input,"input",true,128U);(void)text(input,"value");
                            if(!input.contains("descriptor") || !input.at("descriptor").is_object()) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt provider input descriptor is invalid.");
                        }
                    }
                }
                const bool submitted=!core.promptId.empty();
                for(const auto* key:{"preview_graph","final_graph"}) {if(r.contains(key)) graphShape(r.at(key));else if(submitted) reject(Domain::ErrorCodes::IntegrityFailure,"Submitted render plan is missing a saved graph.");}
                if(r.contains("preview_graph")) plan.previewGraphJson=r.at("preview_graph").dump();if(r.contains("final_graph")) plan.finalGraphJson=r.at("final_graph").dump();
                if(r.contains("preview_motion_output_nodes") || r.contains("final_motion_output_nodes")) {
                    for(const auto* prefix:{"preview","final"}) {
                        const auto key=std::string{prefix}+"_motion_output_nodes",graphKey=std::string{prefix}+"_graph",expectedKey=std::string{prefix}+"_expected_outputs";
                        if(!r.contains(key) || !r.at(key).is_array() || r.at(key).size()>256U || !r.contains(graphKey) || !r.contains(expectedKey))
                            reject(Domain::ErrorCodes::IntegrityFailure,"Receipt stage motion-output contract is invalid.");
                        const auto expected=outputIds(r,expectedKey.c_str(),r.at(graphKey));std::set<std::string> ids;
                        for(const auto& node:r.at(key)) {
                            if(!node.is_string() || node.get_ref<const std::string&>().empty() || node.get_ref<const std::string&>().size()>128U ||
                                !r.at(graphKey).contains(node.get_ref<const std::string&>()) || std::find(expected.begin(),expected.end(),node)==expected.end() ||
                                !ids.insert(node.get<std::string>()).second) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt motion output is not a unique intended stage node.");
                        }
                    }
                }
                if(r.contains("final_graph_sha256")) {plan.finalGraphSha256=seal(r,"final_graph_sha256");if(plan.finalGraphJson.empty() || hash(plan.finalGraphJson)!=plan.finalGraphSha256) reject(Domain::ErrorCodes::IntegrityFailure,"Final graph differs from its receipt seal.");}
                else if(submitted) reject(Domain::ErrorCodes::IntegrityFailure,"Submitted render plan has no final graph seal.");
                if(r.contains("conversation_id")) plan.conversationId=text(r,"conversation_id");
                if(r.contains("conversation_prefix")) {
                    if(!r.at("conversation_prefix").is_array()) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt conversation prefix must be an array.");
                    plan.conversationPrefixSha256=hash(r.at("conversation_prefix").dump());plan.approvalBoundary=requiredInteger(r,"approval_boundary");
                    if(plan.approvalBoundary!=r.at("conversation_prefix").size()) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt approval boundary differs from its saved prefix.");
                    std::optional<std::uint64_t> previous;
                    for(const auto& message:r.at("conversation_prefix")) {(void)text(message,"text",false,MaximumJson);const auto index=requiredInteger(message,"message_index");(void)requiredInteger(message,"selected_version");if(previous && index<=*previous) reject(Domain::ErrorCodes::IntegrityFailure,"Receipt conversation positions are not chronological.");previous=index;if(message.contains("forge_generated")) requiredBoolean(message,"forge_generated");}
                }
                if(core.state==Domain::ComfyJobState::AwaitingPreviewApproval && (stage!="preview" || previews.empty() || plan.conversationId.empty() || plan.finalGraphSha256.empty() || !r.contains("dependency_identity"))) reject(Domain::ErrorCodes::IntegrityFailure,"Awaiting-preview receipt lacks its verified plan, artifacts or conversation binding.");
                if(stage=="final") {
                    plan.approved=requiredInteger(r,"approved_revision")==plan.revision;
                    if(!plan.approved || !r.contains("approval_evidence") || !r.at("approval_evidence").is_object()) reject(Domain::ErrorCodes::IntegrityFailure,"Final receipt lacks approval of its saved revision.");
                    const auto& evidence=r.at("approval_evidence");const auto reply=approvalText(text(evidence,"text",true,MaximumJson));
                    if(reply!="approved" && reply!="yes" && reply!="render final") reject(Domain::ErrorCodes::IntegrityFailure,"Final receipt approval text is not an admitted operator reply.");
                    (void)requiredInteger(evidence,"message_index");(void)requiredInteger(evidence,"selected_version");
                    if(evidence.contains("forge_generated")) {requiredBoolean(evidence,"forge_generated");if(evidence.at("forge_generated").get<bool>()) reject(Domain::ErrorCodes::IntegrityFailure,"Final receipt approval is a Forge-generated message.");}
                }
                core.renderPlan=std::move(plan);
            }
            return core;
        } catch(const Failure& failure) {if(failure.error.code==Domain::ErrorCodes::IntegrityFailure) throw;reject(Domain::ErrorCodes::IntegrityFailure,"Invalid ComfyUI receipt core: "+failure.error.message);}
        catch(const Json::exception& failure) {reject(Domain::ErrorCodes::IntegrityFailure,"Invalid ComfyUI receipt shape: "+std::string{failure.what()});}
    }
    std::shared_ptr<Entry> lookup(std::string id,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        id=take(Domain::Uuid::parse(id)).value(); auto key=a.projectId().value()+"/"+id;
        std::lock_guard lock{entriesMutex_};
        if(auto it=entries_.find(key);it!=entries_.end()) { std::lock_guard row{it->second->mutex}; scope(it->second->record,a,false); return it->second; }
        Json stub{{"project_id",a.projectId().value()},{"job_id",id}};
        auto access=take(storageAuthority_.authorize(storageScope_,{storedPath(stub),std::nullopt,Domain::FileAccess::Read,false},c));
        auto data=take(files_.read(access,MaximumReceipt+1024U,c));
        auto envelope=parse({reinterpret_cast<const char*>(data.data()),data.size()},MaximumReceipt+1024U);
        if(!envelope.is_object() || envelope.size()!=2U || !envelope.contains("payload") || !envelope.contains("sha256") || hash(envelope.at("payload").dump())!=envelope.at("sha256").get<std::string>())
            reject(Domain::ErrorCodes::IntegrityFailure,"ComfyUI receipt integrity check failed.");
        auto e=std::make_shared<Entry>(); lease(e,a.projectId().value(),id); e->record=envelope.at("payload");
        const auto core=decodedCore(e->record);
        if(core.jobId!=id)
            reject(Domain::ErrorCodes::IntegrityFailure,"ComfyUI receipt identity is invalid.");
        scope(e->record,a,false);
        if(liveOwner(e->record)) reject(Domain::ErrorCodes::Conflict,"This receipt is still owned by a live Manager process.");
        if(!terminal(core.state)) {
            e->record["state"]="unknown"; e->record["publication_suppressed"]=true;
            e->record["error"]=error(Domain::makeError(Domain::ErrorCodes::TransportClosed,"Previous Manager ended; resume reconciles existing work without another submission."));
        }
        e->record["recovered"]=true; entries_[key]=e; return e;
    }
    Json snapshot(const std::shared_ptr<Entry>& e,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        std::lock_guard lock{e->mutex}; scope(e->record,a,false); check(c);
        const auto core=decodedCore(e->record);
        Json result=e->record; result["ok"]=true; result["done"]=e->finished.load() && terminal(core.state);
        if(!result.value("publication_suppressed",false)) for(const auto& artifact:core.artifacts) {
            const auto actual=backend_->perform("seal",Json{{"path",artifact.path}}.dump(),settings(c),a,c);
            if(!actual || parse(actual.value()).at("sha256")!=artifact.sha256 || parse(actual.value()).at("bytes")!=artifact.bytes) {
                result["publication_suppressed"]=true;result["state"]="failed";
                result["error"]=error(Domain::makeError(Domain::ErrorCodes::IntegrityFailure,"A published artifact is missing, changed or no longer authorized."));break;
            }
        }
        if(result.value("publication_suppressed",false)) { result["artifacts"]=Json::array();result["preview_artifacts"]=Json::array(); }
        if((terminal(core.state)&&(core.state!=Domain::ComfyJobState::Unknown||e->finished.load())) || result.at("state")!=e->record.at("state")) result["phase"]=result.at("state");
        else if(!result.contains("phase")) result["phase"]=result.at("state");
        const auto revisionRequested=result.value("preview_revision_requested",false) || result.value("preview_recovery_requires_new_preview",false);
        result["requires_new_preview"]=revisionRequested;
        result["requires_operator_approval"]=result.at("state")=="awaiting_preview_approval" && !revisionRequested;
        result["approval_reply_choices"]=revisionRequested?Json::array():Json::array({"approved","yes","render final"});
        result["receipt_path"]=storedPath(e->record).value();
        // Graphs and frozen capabilities remain in the private receipt, not every poll response.
        for(const auto* key:{"scope","preview_graph","final_graph","graph","arguments","inputs","input_seals","conversation_prefix","approval_evidence"}) result.erase(key);
        std::size_t previewBytes{};
        for(const auto* group:{"artifacts","preview_artifacts"}) for(auto& artifact:result.at(group)) artifact=publicArtifact(artifact,previewBytes);
        for(const auto* key:{"preparation","outcome"}) if(result.contains(key) && result[key].is_object()) result[key].erase("manifest");
        if(result.contains("collection_outcome") && result["collection_outcome"].is_object()) result["collection_outcome"].erase("artifacts");
        for(const auto* key:{"preparation","outcome","collection_outcome","dependency_identity","error","node_errors","submission_error","provider_error","provider_events","progress","progress_error","cancel_outcome"})
            if(result.contains(key)) result[key]=boundedEvidence(result.at(key));
        if(result.dump().size()>MaximumPublicJobBytes) reject(Domain::ErrorCodes::PayloadTooLarge,"The artifact collection exceeds the bounded delivery size; inspect its private receipt.");
        return result;
    }
    void requireProviderInputBindings(const Json& preflight,const Json& r) {
        for(const auto& required:preflight.at("provider_file_inputs")) {
            const auto node=text(required,"node_id"),input=text(required,"input");
            const auto explicitInput=std::count_if(r.at("inputs").begin(),r.at("inputs").end(),[&](const auto& binding) {
                return text(binding,"node_id")==node && (binding.contains("input")?text(binding,"input"):"image")==input;
            });
            const auto sealed=std::count_if(r.at("input_seals").begin(),r.at("input_seals").end(),[&](const auto& binding) {
                return binding.value("node_id",std::string{})==node && binding.value("input",std::string{})==input &&
                    binding.value("value",std::string{})==required.at("value").get<std::string>() && binding.contains("provider_sha256") && binding.contains("provider_bytes");
            });
            if(explicitInput!=1 || sealed!=1) reject(r.at("stage")=="final"?Domain::ErrorCodes::Conflict:Domain::ErrorCodes::InvalidRequest,
                "Provider file input "+node+"."+input+" requires an explicit local inputs binding and a sealed private upload; create another preview.");
        }
    }
    void verifyInputSeals(const Json& r,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        const auto owned="ForgeConductor/"+r.at("project_id").get<std::string>()+"/"+r.at("job_id").get<std::string>();
        for(const auto& sealed:r.at("input_seals")) {
            if(!sealed.contains("provider_sha256") || !sealed.contains("provider_bytes") || !sealed.contains("descriptor"))
                reject(Domain::ErrorCodes::Conflict,"Saved input has no verified private provider copy; create another preview.");
            auto actual=call("inspect",{{"path",sealed.at("path")}},config,a,c);
            auto provider=call("seal_input",{{"descriptor",sealed.at("descriptor")},{"namespace",owned}},config,a,c);
            if(actual.at("sha256")!=sealed.at("sha256") || actual.at("bytes")!=sealed.at("bytes") ||
                provider.at("sha256")!=sealed.at("provider_sha256") || provider.at("bytes")!=sealed.at("provider_bytes") ||
                sealed.at("provider_sha256")!=sealed.at("sha256") || sealed.at("provider_bytes")!=sealed.at("bytes"))
                reject(Domain::ErrorCodes::Conflict,"An original or private provider input changed; create another preview.");
        }
    }
    void verifyPlanSeals(const Json& r,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        if(hash(r.at("final_graph").dump())!=r.at("final_graph_sha256").get<std::string>())
            reject(Domain::ErrorCodes::IntegrityFailure,"Final graph differs from its approved seal.");
        verifyInputSeals(r,config,a,c);
        for(const auto* key:{"preview_graph","final_graph"}) requireProviderInputBindings(preflight(r.at(key),Json::array(),config,a,c),r);
        for(const auto& sealed:r.at("preview_artifacts")) {
            auto actual=call("inspect",{{"path",sealed.at("path")}},config,a,c);
            if(actual.at("sha256")!=sealed.at("sha256") || actual.at("bytes")!=sealed.at("bytes"))
                reject(Domain::ErrorCodes::Conflict,"An input or preview artifact changed; create another preview.");
        }
        auto identity=call("identity",{{"workflows",Json::array({r.at("preview_graph"),r.at("final_graph")})}},config,a,c);
        if(identity!=r.at("dependency_identity")) reject(Domain::ErrorCodes::Conflict,"Runtime or model dependencies changed; preview must be renewed.");
    }
    bool previewDeliveredBefore(const Json& observation,const Json& r,const Json& approval,
        bool requireSealedPlan=false,std::uint64_t* deliveredIndex=nullptr,std::uint64_t* deliveredVersion=nullptr) {
        const auto approvalIndex=integer(approval,"message_index",UINT64_MAX,UINT64_MAX);
        Json latest;std::uint64_t latestIndex{},latestVersion{};
        for(const auto& result:observation.value("native_tool_results",Json::array())) {
            const auto name=result.value("name",std::string{}),plugin=result.value("plugin_identifier",std::string{});
            if((name!="comfy_run" && name!="comfy_job_status" && name!="comfy_job_resume") || (plugin!="mcp/forge-conductor" && plugin!="mcp/forge-conductor-fallback")) continue;
            const auto resultIndex=integer(result,"message_index",UINT64_MAX,UINT64_MAX);
            if(resultIndex>=approvalIndex) continue;
            for(const auto& body:result.value("text_bodies",Json::array())) {
                if(!body.is_string()) continue;const auto receipt=Json::parse(body.get_ref<const std::string&>(),nullptr,false);
                if(!receipt.is_object() || receipt.value("state",std::string{})!="awaiting_preview_approval" ||
                    receipt.value("plan_id",std::string{}).empty()) continue;
                // Native tool evidence is chronological within each selected
                // message. One short approval applies to its latest preview.
                if(latest.is_null() || resultIndex>=latestIndex) { latest=receipt;latestIndex=resultIndex;latestVersion=integer(result,"selected_version",UINT64_MAX,UINT64_MAX); }
            }
        }
        if(!latest.is_object() || latest.value("plan_id",std::string{})!=r.at("plan_id").get<std::string>()) return false;
        if(requireSealedPlan) {
            if(!latest.value("ok",false) || latest.value("stage",std::string{})!="preview" || latestVersion==UINT64_MAX ||
                latest.value("publication_suppressed",false) || latest.value("requires_new_preview",false)) return false;
            for(const auto* key:{"job_id","project_id","revision","graph_sha256","final_graph_sha256"})
                if(!latest.contains(key) || latest.at(key)!=r.at(key)) return false;
            if(!latest.contains("dependency_identity") || latest.at("dependency_identity")!=boundedEvidence(r.at("dependency_identity"))) return false;
        }
        const auto published=latest.value("artifacts",Json::array());
        if(!published.is_array() || published.size()!=r.at("preview_artifacts").size()) return false;
        for(const auto& sealed:r.at("preview_artifacts")) if(std::none_of(published.begin(),published.end(),[&](const auto& file){
            return file.is_object() && file.value("sha256",std::string{})==sealed.at("sha256").get<std::string>() &&
                file.value("path",std::string{})==sealed.at("path").get<std::string>() && file.value("bytes",0ULL)==sealed.at("bytes").get<std::uint64_t>() &&
                (!requireSealedPlan || (file.contains("node_id") && file.at("node_id")==sealed.at("node_id") &&
                    file.contains("media_type") && file.at("media_type")==sealed.at("media_type")));
        })) return false;
        if(deliveredIndex) *deliveredIndex=latestIndex;
        if(deliveredVersion) *deliveredVersion=latestVersion;
        return true;
    }
    Json list(const Json& args,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        fields(args,{"offset","limit"}); auto offset=integer(args,"offset",0U,1000000U),limit=integer(args,"limit",32U,128U);
        if(!limit) reject(Domain::ErrorCodes::InvalidRequest,"Job list limit must be positive.");
        auto projectRoot=native(jobsRoot_.value())/a.projectId().value(); auto u=projectRoot.u8string();
        auto access=take(storageAuthority_.authorize(storageScope_,{path({reinterpret_cast<const char*>(u.data()),u.size()}),std::nullopt,Domain::FileAccess::Read,false},c));
        auto opened=Detail::openAuthorizedDirectory(access,c); Json ids=Json::array();
        if(opened) {
            for(const auto& row:take(Detail::enumerateDirectory(opened.value().handle.get(),10000U,c))) {
                if(!row.isDirectory() || row.isReparsePoint()) continue;
                auto id=take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(row.name)); if(Domain::Uuid::parse(id)) ids.push_back(id);
            }
        } else if(opened.error().code!=Domain::ErrorCodes::RecordNotFound) throw Failure{opened.error()};
        std::sort(ids.begin(),ids.end()); Json jobs=Json::array(),unreadable=Json::array(); auto end=(std::min)(ids.size(),static_cast<std::size_t>(offset+limit));
        std::size_t consumed{},deliveryBytes{};
        for(auto i=static_cast<std::size_t>(offset);i<end;++i) {
            check(c);const auto id=ids[i].get<std::string>();Json value;bool readable=true;
            const auto diagnostic=[&](const Domain::Error& cause) {
                return Json{{"job_id",id},{"receipt_path",storedPath(Json{{"project_id",a.projectId().value()},{"job_id",id}}).value()},
                    {"error",boundedEvidence(error(cause))}};
            };
            try { value=snapshot(lookup(id,a,c),a,c); }
            catch(const Failure& failure) {
                if(failure.error.code==Domain::ErrorCodes::Cancelled || failure.error.code==Domain::ErrorCodes::DeadlineExceeded) throw;
                readable=false;value=diagnostic(failure.error);
            } catch(const Json::exception& failure) {
                readable=false;value=diagnostic(Domain::makeError(Domain::ErrorCodes::IntegrityFailure,"Unreadable ComfyUI receipt JSON: "+std::string{failure.what()}));
            }
            const auto encodedBytes=value.dump().size()+2U;
            if(deliveryBytes+encodedBytes>MaximumPublicJobBytes-PublicPageHeaderReserve) {
                if(!consumed) reject(Domain::ErrorCodes::PayloadTooLarge,"One job snapshot or recovery diagnostic exceeds the list page's delivery bound.");break;
            }
            deliveryBytes+=encodedBytes;++consumed;
            if(readable) jobs.push_back(std::move(value));else unreadable.push_back(std::move(value));
        }
        const auto next=offset+consumed;
        Json result{{"ok",true},{"jobs",jobs},{"unreadable_jobs",unreadable},{"partial",!unreadable.empty()},{"total",ids.size()},
            {"has_more",next<ids.size()},{"next_offset",next<ids.size()?Json(next):Json(nullptr)}};
        if(result.dump().size()>MaximumPublicJobBytes) reject(Domain::ErrorCodes::PayloadTooLarge,"Job list page exceeds its delivery bound.");
        return result;
    }
    void mutate(const std::shared_ptr<Entry>& e,const Domain::OperationContext& c,const std::function<void(Json&)>& action) {
        std::lock_guard lock{e->mutex}; auto next=e->record;action(next);persist(next,c);e->record=std::move(next);e->changed.notify_all();
    }
    Json current(const std::shared_ptr<Entry>& e) { std::lock_guard lock{e->mutex}; return e->record; }
    void launch(const std::shared_ptr<Entry>& e,const Domain::ComfyUiConfig& config,bool attach=false) {
        try {
        std::lock_guard workerLock{e->workerMutex};
        if(stopped_) reject(Domain::ErrorCodes::TransportClosed,"ComfyUI service stopped before worker launch.");
        if(e->worker.joinable()) e->worker.join();
        e->stop=std::stop_source{}; e->finished=false;
        { auto r=current(e); auto c=workerContext(r,{},10U);
            if(r.value("cancellation_requested",false)) e->stop.request_stop();
            mutate(e,c,[](Json& value){value["owner_pid"]=::GetCurrentProcessId();value["owner_creation_time"]=processCreation(::GetCurrentProcess());value["owner_released"]=false;value["phase"]="waiting_provider_lease";}); }
        if(stopped_) reject(Domain::ErrorCodes::TransportClosed,"ComfyUI service stopped during worker admission.");
        e->worker=std::thread([this,e,config,attach] {
            std::optional<Contracts::WorkspaceAuthority> cleanupAuthority;
            try {
                auto r=current(e); auto timeout=static_cast<std::uint32_t>(integer(r.value("arguments",Json::object()),"timeout_sec",config.generationTimeoutSeconds,7200U));
                auto c=workerContext(r,e->stop.get_token(),(std::max)(timeout,1U));
                std::unique_lock<std::timed_mutex> gate{providerMutex_,std::defer_lock};
                while(!gate.try_lock_for(std::chrono::milliseconds{50})) check(c);
                const auto providerLease=take(Detail::ProviderOperationLease::acquire(config.endpoint,c));
                check(c);
                auto a=fresh(r,c);
                cleanupAuthority.emplace(a);
                if(configJson(settings(c))!=r.at("configuration")) reject(Domain::ErrorCodes::Conflict,"ComfyUI configuration changed after admission.");
                if(r.at("operation")!="render") {
                    mutate(e,c,[&](Json& value){value["state"]="preparing";value["phase"]=r.at("operation")=="prepare"?"preparing_dependencies":"controlling_provider";});
                    auto outcome=call(r.at("operation").get<std::string>(),r.at("arguments"),config,a,c);
                    mutate(e,c,[&](Json& value){value["outcome"]=outcome;value["downloaded_bytes"]=outcome.value("downloaded_bytes",0ULL);
                        value["state"]=outcome.value("ok",true)?"completed":"failed";
                        value["phase"]=value.at("state");
                        if(!outcome.value("ok",true)) value["error"]=outcome.value("error",Json{{"code",Domain::ErrorCodes::HostCapabilityUnavailable},{"message","Preparation could not resolve the workflow dependencies."}});
                    });
                } else run(e,config,a,c,attach);
            } catch(const Failure& failure) { fail(e,failure.error); }
            catch(const std::exception& failure) { fail(e,Domain::makeError(Domain::ErrorCodes::InternalFailure,failure.what())); }
            catch(...) { fail(e,Domain::makeError(Domain::ErrorCodes::InternalFailure,"ComfyUI worker failed.")); }
            try { auto latest=current(e); auto cleanup=workerContext(latest,{},10U);
                if(latest.at("operation")=="render" && cleanupAuthority) {
                    try {
                        (void)backend_->perform("progress",Json{{"action","close"},{"client_id","forge-"+latest.at("job_id").get<std::string>()}}.dump(),config,*cleanupAuthority,cleanup);
                    } catch(...) {}
                }
                mutate(e,cleanup,[](Json& value){value["owner_released"]=true;}); } catch(...) {}
            releaseActive(e);
        });
        } catch(...) {
            Domain::Error cause=Domain::makeError(Domain::ErrorCodes::InternalFailure,"ComfyUI worker could not start.");
            try { throw; } catch(const Failure& failure) { cause=failure.error; }
            catch(const std::exception& failure) { cause.message=failure.what(); } catch(...) {}
            fail(e,cause);
            try {const auto saved=current(e);const auto cleanup=workerContext(saved,{},10U);mutate(e,cleanup,[](Json& value){value["owner_released"]=true;});} catch(...) {}
            releaseActive(e);throw;
        }
    }
    void fail(const std::shared_ptr<Entry>& e,const Domain::Error& cause) noexcept {
        try {
            auto r=current(e); auto c=workerContext(r,{},10U);
            mutate(e,c,[&](Json& value) {
                auto state=value.at("state").get<std::string>();
                value["state"]=value.value("cancellation_requested",false) || cause.code==Domain::ErrorCodes::Cancelled?"cancelled":
                    (state=="submitting" || (value.at("prompt_id").is_string() && state!="fetching" &&
                    value.value("remote_state",std::string{})!="rejected" && value.value("remote_state",std::string{})!="failed" &&
                    value.value("remote_state",std::string{})!="completed"))?"unknown":"failed";
                value["error"]=error(cause); value["publication_suppressed"]=true;
                value["phase"]=value.at("state");
                value["remote_execution_may_continue"]=value.at("prompt_id").is_string() &&
                    value.value("remote_state",std::string{})!="rejected" && value.value("remote_state",std::string{})!="failed" && value.value("remote_state",std::string{})!="completed";
            });
        } catch(...) {}
    }
    Json startOperation(const std::string& kind,const Json& args,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        if(args.contains("timeout_sec") && !integer(args,"timeout_sec",1800U,7200U)) reject(Domain::ErrorCodes::InvalidRequest,"Timeout must be positive.");
        auto e=create(kind,config,a,c,[&](Json& r){r["arguments"]=args;});launch(e,config);return snapshot(e,a,c);
    }
    const Json* inspectPreviewReplies(Json& r,const Json& messages,std::size_t boundary,const std::shared_ptr<Entry>& e,const Domain::OperationContext& c) {
        const Json* latest{};
        for(auto index=boundary;index<messages.size();++index) {
            const auto& reply=messages[index];if(reply.value("forge_generated",false)) continue;
            const auto approved=approvalText(text(reply,"text",false,MaximumJson));
            if(approved=="verified") continue;
            if(approved!="approved" && approved!="yes" && approved!="render final") {
                r["preview_revision_requested"]=true;r["preview_revision_evidence"]={{"message_index",reply.at("message_index")},{"selected_version",reply.at("selected_version")}};
                persist(r,c);e->record=r;
                reject(Domain::ErrorCodes::Unauthorized,"The operator requested a revision; create and present a new preview before final approval.");
            }
            latest=&reply;
        }
        return latest;
    }
    void invalidatePreviewRecovery(Json& r,const std::shared_ptr<Entry>& e,const Domain::OperationContext& c,const char* message) {
        r["preview_recovery_requires_new_preview"]=true;r["preview_recovery_error"]={{"code",Domain::ErrorCodes::Conflict},{"message",message}};
        persist(r,c);e->record=r;reject(Domain::ErrorCodes::Conflict,message);
    }
    bool packetNamesPlan(const Json& value,const std::string& plan,std::size_t depth=0U) {
        if(depth>16U) return false;
        if(value.is_string()) {
            const auto& encoded=value.get_ref<const std::string&>();
            const auto token=[](char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_';};
            for(auto at=encoded.find(plan);at!=std::string::npos;at=encoded.find(plan,at+plan.size()))
                if((!at || !token(encoded[at-1U])) && (at+plan.size()==encoded.size() || !token(encoded[at+plan.size()]))) return true;
        } else if(value.is_structured()) for(const auto& child:value) if(packetNamesPlan(child,plan,depth+1U)) return true;
        return false;
    }
    bool successorRecoveredPlan(const Json& observation,const Json& r) {
        const auto handoff=observation.value("verified_handoff_id",std::string{});if(handoff.empty()) return false;
        for(const auto& result:observation.value("native_tool_results",Json::array())) {
            const auto plugin=result.value("plugin_identifier",std::string{});
            if(result.value("name",std::string{})!="context_get" || (plugin!="mcp/forge-conductor" && plugin!="mcp/forge-conductor-fallback")) continue;
            for(const auto& body:result.value("text_bodies",Json::array())) {
                if(!body.is_string()) continue;const auto recovered=Json::parse(body.get_ref<const std::string&>(),nullptr,false);
                if(recovered.is_object() && recovered.value("ok",false) && recovered.value("found",false) &&
                    recovered.value("action",std::string{})=="get" && recovered.value("handoff_id",std::string{})==handoff &&
                    recovered.value("workspace_project_id",std::string{})==r.at("project_id").get<std::string>() &&
                    recovered.contains("packet") && recovered.at("packet").is_object() && packetNamesPlan(recovered.at("packet"),r.at("plan_id").get<std::string>())) return true;
            }
        }
        return false;
    }
    Json render(const Json& args,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        auto stage=text(args,"stage"); requireExecute(a);
        if(stage=="final") {
            fields(args,{"stage","plan_id"}); auto e=lookup(text(args,"plan_id"),a,c);
            std::unique_lock lock{e->mutex}; auto r=e->record; scope(r,a,true);
            if(r.at("operation")!="render") reject(Domain::ErrorCodes::InvalidRequest,"Plan ID is not a render plan.");
            if(r.at("stage")=="final") { lock.unlock(); return snapshot(e,a,c); }
            if(!e->finished) reject(Domain::ErrorCodes::Conflict,"Preview worker is still completing its durable cleanup.");
            if(r.at("state")!="awaiting_preview_approval") reject(Domain::ErrorCodes::Conflict,"Preview is not ready for approval.");
            if(r.value("preview_revision_requested",false) || r.value("preview_recovery_requires_new_preview",false)) reject(Domain::ErrorCodes::Unauthorized,"The saved preview requires renewal; create and present a new preview before final approval.");
            if(configJson(config)!=r.at("configuration")) reject(Domain::ErrorCodes::Conflict,"Configuration changed; create a revised preview.");
            auto observation=observe(a.projectId(),c,r.at("conversation_id").get_ref<const std::string&>());
            auto messages=observation.at("user_messages"); auto boundary=r.at("approval_boundary").get<std::size_t>();
            if(observation.at("conversation_id")!=r.at("conversation_id")) {
                const Json* recoveredReply{};
                for(const auto& reply:messages) if(!reply.value("forge_generated",false)) recoveredReply=&reply;
                std::uint64_t deliveryIndex{},deliveryVersion{};
                if(recoveredReply && previewDeliveredBefore(observation,r,*recoveredReply,true,&deliveryIndex,&deliveryVersion)) {
                    if(!observation.contains("bound_conversation") ||
                        observation.at("bound_conversation").at("conversation_id")!=r.at("conversation_id"))
                        reject(Domain::ErrorCodes::Conflict,"The original saved preview conversation cannot be read for recovery.");
                    const auto& originalMessages=observation.at("bound_conversation").at("user_messages");
                    if(originalMessages.size()<boundary || Json(originalMessages.begin(),originalMessages.begin()+boundary)!=r.at("conversation_prefix"))
                        invalidatePreviewRecovery(r,e,c,"The original preview conversation prefix or selected message version changed; create and present a new preview.");
                    (void)inspectPreviewReplies(r,originalMessages,boundary,e,c);
                    const auto recoveredBoundary=static_cast<std::size_t>(std::count_if(messages.begin(),messages.end(),[&](const auto& reply){
                        return reply.at("message_index").template get<std::uint64_t>()<=deliveryIndex;
                    }));
                    const auto* approvedReply=inspectPreviewReplies(r,messages,recoveredBoundary,e,c);
                    if(!approvedReply) reject(Domain::ErrorCodes::Unauthorized,"Reply approved, yes or render final after the recovered preview.");
                    r["preview_recovery_evidence"]={{"predecessor_conversation_id",r.at("conversation_id")},
                        {"predecessor_prefix_sha256",hash(r.at("conversation_prefix").dump())},
                        {"preview_message_index",deliveryIndex},{"preview_selected_version",deliveryVersion},
                        {"final_graph_sha256",r.at("final_graph_sha256")}};
                    r["conversation_id"]=observation.at("conversation_id");
                    r["conversation_prefix"]=Json(messages.begin(),messages.begin()+recoveredBoundary);
                    r["approval_boundary"]=recoveredBoundary;boundary=recoveredBoundary;
                } else {
                    if(observation.value("verified_predecessor",std::string{})!=r.at("conversation_id").get<std::string>())
                        reject(Domain::ErrorCodes::Conflict,"Select the preview conversation or its verified recovered successor.");
                    if(!observation.contains("predecessor_conversation") || observation.at("predecessor_conversation").at("conversation_id")!=r.at("conversation_id"))
                        invalidatePreviewRecovery(r,e,c,"The saved predecessor conversation cannot be verified; create and present a new preview.");
                    const auto& predecessorMessages=observation.at("predecessor_conversation").at("user_messages");
                    if(predecessorMessages.size()<boundary || Json(predecessorMessages.begin(),predecessorMessages.begin()+boundary)!=r.at("conversation_prefix"))
                        invalidatePreviewRecovery(r,e,c,"The predecessor conversation prefix or selected message version changed; create and present a new preview.");
                    (void)inspectPreviewReplies(r,predecessorMessages,boundary,e,c);
                    if(!successorRecoveredPlan(observation,r)) reject(Domain::ErrorCodes::Conflict,"Successor has not retrieved this pending render plan through the verified Forge handoff.");
                    // The delivered continuity packet is the successor's baseline, not approval.
                    r["conversation_id"]=observation.at("conversation_id"); r["conversation_prefix"]=messages;
                    r["approval_boundary"]=messages.size(); persist(r,c);e->record=r;
                    reject(Domain::ErrorCodes::Conflict,"Preview approval is still required in the recovered successor chat.");
                }
            }
            if(messages.size()<boundary || Json(messages.begin(),messages.begin()+boundary)!=r.at("conversation_prefix"))
                reject(Domain::ErrorCodes::Conflict,"Preview conversation prefix or selected message version changed.");
            if(messages.size()<=boundary) reject(Domain::ErrorCodes::Unauthorized,"Reply approved, yes or render final in LM Studio after reviewing the preview.");
            const auto* latestReply=inspectPreviewReplies(r,messages,boundary,e,c);
            if(!latestReply) reject(Domain::ErrorCodes::Unauthorized,"Reply approved, yes or render final in LM Studio after reviewing the preview.");
            const auto& message=*latestReply;
            if(!previewDeliveredBefore(observation,r,message))
                reject(Domain::ErrorCodes::Unauthorized,"Approval must follow the saved Forge preview result in LM Studio.");
            verifyPlanSeals(r,config,a,c);
            r["approval_evidence"]=message; r["approved_revision"]=r.at("revision"); r["stage"]="final";
            r["state"]="before_dispatch";r["phase"]="waiting_provider_lease";r["prompt_id"]=nullptr;r["artifacts"]=Json::array();r["error"]=nullptr;
            r["submission_acknowledged"]=false;r["publication_suppressed"]=false;
            reserveActive(e);
            try {persist(r,c);e->record=r;} catch(...) {releaseActive(e);throw;}
            lock.unlock();launch(e,config);return snapshot(e,a,c);
        }
        if(stage!="preview") reject(Domain::ErrorCodes::InvalidRequest,"Render stage must be preview or final.");
        fields(args,{"stage","media_kind","preview_workflow","final_workflow","output_directory","inputs","expected_outputs","preview_expected_outputs","final_expected_outputs","timeout_sec"});
        const auto mediaKind=text(args,"media_kind",true,16U);
        if(mediaKind!="image" && mediaKind!="video" && mediaKind!="mixed") reject(Domain::ErrorCodes::InvalidRequest,"Render media_kind must be image, video or mixed.");
        graphShape(args.at("preview_workflow")); graphShape(args.at("final_workflow"));
        if(args.dump().size()+args.at("preview_workflow").dump().size()+args.at("final_workflow").dump().size()+
            (std::max)(args.at("preview_workflow").dump().size(),args.at("final_workflow").dump().size())>MaximumReceipt-2U*1024U*1024U)
            reject(Domain::ErrorCodes::PayloadTooLarge,"The saved render plan exceeds the durable graph budget.");
        if(args.contains("timeout_sec") && !integer(args,"timeout_sec",1800U,7200U)) reject(Domain::ErrorCodes::InvalidRequest,"Timeout must be positive.");
        const auto inputs=args.value("inputs",Json::array());
        if(!inputs.is_array() || inputs.size()>64U) reject(Domain::ErrorCodes::InvalidRequest,"Inputs must be a bounded array.");
        std::set<std::pair<std::string,std::string>> boundInputs;
        for(const auto& input:inputs) {
            fields(input,{"node_id","input","path"}); auto id=text(input,"node_id");
            if(!args.at("preview_workflow").contains(id) || !args.at("final_workflow").contains(id))
                reject(Domain::ErrorCodes::InvalidRequest,"Input node must be present in both saved workflows.");
            (void)path(text(input,"path")); const auto name=input.contains("input")?text(input,"input"):"image";
            if(!boundInputs.emplace(id,name).second) reject(Domain::ErrorCodes::InvalidRequest,"Input bindings must name distinct node inputs.");
            for(const auto* key:{"preview_workflow","final_workflow"}) if(!args.at(key).at(id).at("inputs").contains(name))
                reject(Domain::ErrorCodes::InvalidRequest,"Input binding must target an existing input in both saved workflows.");
        }
        const auto previewExpected=outputIds(args,"preview_expected_outputs",args.at("preview_workflow"));
        const auto finalExpected=outputIds(args,"final_expected_outputs",args.at("final_workflow"));
        auto destination=path(text(args,"output_directory"));
        auto previewDestination=native(destination.value())/"preview";auto finalDestination=native(destination.value())/"final";
        const auto previewPath=previewDestination.u8string(),finalPath=finalDestination.u8string();
        auto output=take(authority_.authorize(a,{path({reinterpret_cast<const char*>(previewPath.data()),previewPath.size()}),std::nullopt,Domain::FileAccess::Create,true},c));
        (void)take(authority_.authorize(a,{path({reinterpret_cast<const char*>(finalPath.data()),finalPath.size()}),std::nullopt,Domain::FileAccess::Create,true},c));
        const auto outputParent=native(output.canonicalPath().value()).parent_path().u8string();
        auto observation=observe(a.projectId(),c);
        auto e=create("render",config,a,c,[&](Json& r) {
            r["arguments"]=args;r["media_kind"]=mediaKind;r["preview_graph"]=args.at("preview_workflow");r["final_graph"]=args.at("final_workflow");
            r["final_graph_sha256"]=hash(r.at("final_graph").dump());r["output_directory"]=std::string{reinterpret_cast<const char*>(outputParent.data()),outputParent.size()};
            r["inputs"]=inputs;r["input_seals"]=Json::array();
            r["preview_expected_outputs"]=previewExpected;r["final_expected_outputs"]=finalExpected;
            r["conversation_id"]=observation.at("conversation_id");r["conversation_prefix"]=observation.at("user_messages");
            r["approval_boundary"]=observation.at("user_messages").size();
        });
        launch(e,config); return snapshot(e,a,c);
    }
    void run(const std::shared_ptr<Entry>& e,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c,bool attach) {
        auto r=current(e); auto final=r.at("stage")=="final";
        const auto phase=[&](const char* name){mutate(e,c,[&](Json& value){value["phase"]=name;});r["phase"]=name;};
        if(!attach) {
            if(config.automaticSetup && !final){phase("starting_provider");call("control",{{"action","start"}},config,a,c);}
            if(final){phase("verifying_submission");verifyPlanSeals(r,config,a,c);}
            if(!final) {
                phase("uploading_inputs");
                auto uploads=call("upload",{{"inputs",r.at("inputs")},{"namespace","ForgeConductor/"+r.at("project_id").get<std::string>()+"/"+r.at("job_id").get<std::string>()}},config,a,c);
                for(const auto& binding:uploads.value("bindings",Json::array())) for(const auto* key:{"preview_graph","final_graph"}) {
                    auto node=text(binding,"node_id"); auto input=text(binding,"input");
                    if(!r[key].contains(node)) reject(Domain::ErrorCodes::InvalidRequest,"Input binding node is absent from a workflow.");
                    r[key][node]["inputs"][input]=binding.at("value");
                }
                r["input_seals"]=uploads.value("inputs",Json::array());
            }
            for(const auto* key:{"preview_graph","final_graph"}) {
                auto suffix=std::string{key}=="preview_graph"?"preview":"final";
                auto name="ForgeConductor/"+r.at("project_id").get<std::string>()+"/"+r.at("job_id").get<std::string>()+"/"+suffix+"/output";
                for(auto& [id,node]:r[key].items()) if(node.at("inputs").contains("filename_prefix")) node["inputs"]["filename_prefix"]=name+"-"+id;
            }
            phase("preflight");auto preview=preflight(r.at("preview_graph"),r.at("preview_expected_outputs"),config,a,c);
            auto finalCheck=preflight(r.at("final_graph"),r.at("final_expected_outputs"),config,a,c);
            requireProviderInputBindings(preview,r);requireProviderInputBindings(finalCheck,r);
            const auto initiallyReady=preview.at("ready").get<bool>() && finalCheck.at("ready").get<bool>();
            const auto mediaInspectionRequired=preview.at("media_inspection_required").get<bool>() || finalCheck.at("media_inspection_required").get<bool>();
            if(final && !initiallyReady) reject(Domain::ErrorCodes::Conflict,"Approved workflow is no longer ready; create a revised preview.");
            if(!final && config.automaticSetup && (!initiallyReady || r.at("media_kind")!="image" || mediaInspectionRequired)) {
                    Json combined=Json::object();
                    for(const auto* key:{"preview_graph","final_graph"}) {
                        const auto prefix=std::string{key}=="preview_graph"?"preview-":"final-";
                        const auto& original=r.at(key);
                        for(const auto& [id,node]:original.items()) {
                            auto copied=node;
                            for(auto& [input,value]:copied["inputs"].items()) {
                                (void)input;
                                if(value.is_array() && value.size()==2U && value[0].is_string() && value[1].is_number_integer() && original.contains(value[0].get<std::string>()))
                                    value[0]=prefix+value[0].get<std::string>();
                            }
                            combined[prefix+id]=std::move(copied);
                        }
                    }
                    phase("preparing_dependencies");const auto prepared=call("prepare",{{"workflow",combined},{"media_kind",r.at("media_kind")},{"media_inspection_required",mediaInspectionRequired}},config,a,c);
                    r["preparation"]=prepared;r["downloaded_bytes"]=prepared.value("downloaded_bytes",0ULL);
                    mutate(e,c,[&](Json& value){value["preparation"]=prepared;value["downloaded_bytes"]=prepared.value("downloaded_bytes",0ULL);});
                    if(!prepared.value("ok",false)) reject(Domain::ErrorCodes::HostCapabilityUnavailable,"Automatic preparation did not resolve workflow requirements: "+prepared.dump());
                phase("preflight");preview=preflight(r.at("preview_graph"),r.at("preview_expected_outputs"),config,a,c);
                finalCheck=preflight(r.at("final_graph"),r.at("final_expected_outputs"),config,a,c);
                requireProviderInputBindings(preview,r);requireProviderInputBindings(finalCheck,r);
            }
            if(!preview.at("ready").get<bool>() || !finalCheck.at("ready").get<bool>()) reject(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Workflow preflight requires resolution: "+Json{{"preview",preview.at("issues")},{"final",finalCheck.at("issues")}}.dump());
            r["preview_expected_outputs"]=preview.at("expected_outputs"); r["final_expected_outputs"]=finalCheck.at("expected_outputs");
            r["preview_motion_output_nodes"]=preview.at("motion_output_nodes");r["final_motion_output_nodes"]=finalCheck.at("motion_output_nodes");
            r["motion_preview_required"]=r.at("media_kind")=="video" || !preview.at("motion_output_nodes").empty() || !finalCheck.at("motion_output_nodes").empty();
            r["final_graph_sha256"]=hash(r.at("final_graph").dump());
            if(!final){phase("sealing_dependencies");r["dependency_identity"]=call("identity",{{"workflows",Json::array({r.at("preview_graph"),r.at("final_graph")})}},config,a,c);}
            r["graph"]=r.at(final?"final_graph":"preview_graph");r["graph_sha256"]=hash(r.at("graph").dump());
            auto submissionAuthority=fresh(r,c);
            if(configJson(settings(c))!=r.at("configuration")) reject(Domain::ErrorCodes::Conflict,"Configuration changed before submission.");
            if(final) verifyPlanSeals(r,config,submissionAuthority,c);
            phase("waiting_provider_queue");
            for(;;) {
                const auto queue=request("GET","/queue",Json::object(),config,submissionAuthority,c);
                if(queue.value("queue_running",Json::array()).empty() && queue.value("queue_pending",Json::array()).empty()) break;
                check(c);std::unique_lock lock{e->mutex};e->changed.wait_for(lock,std::chrono::milliseconds{250});
            }
            const auto currentSubmissionAuthority=fresh(r,c);
            phase("verifying_submission");
            if(configJson(settings(c))!=r.at("configuration")) reject(Domain::ErrorCodes::Conflict,"Configuration changed while waiting for the provider queue.");
            if(final) verifyPlanSeals(r,config,currentSubmissionAuthority,c);
            else {
                verifyInputSeals(r,config,currentSubmissionAuthority,c);
                const auto identity=call("identity",{{"workflows",Json::array({r.at("preview_graph"),r.at("final_graph")})}},config,currentSubmissionAuthority,c);
                if(identity!=r.at("dependency_identity")) reject(Domain::ErrorCodes::Conflict,"Runtime or model dependencies changed while waiting for the provider queue; create another preview.");
            }
            r["prompt_id"]=take(uuids_.next()).value();r["state"]="submitting";r["phase"]="submitting";
            { std::lock_guard lock{e->mutex}; if(e->record.value("cancellation_requested",false)) reject(Domain::ErrorCodes::Cancelled,"Render was cancelled before submission.");e->record=r;persist(r,c); }
            const auto channel="forge-"+r.at("job_id").get<std::string>();
            const auto connected=backend_->perform("progress",Json{{"action","open"},{"client_id",channel}}.dump(),config,currentSubmissionAuthority,c);
            mutate(e,c,[&](Json& value){value["progress_connected"]=static_cast<bool>(connected);if(!connected)value["progress_error"]=error(connected.error());});
            auto submitted=request("POST","/prompt",{{"prompt",r.at("graph")},{"prompt_id",r.at("prompt_id")},
                {"client_id","forge-"+r.at("job_id").get<std::string>()}},config,currentSubmissionAuthority,c);
            if(submitted.contains("error") && submitted.value("http_status",0U)==400U) {
                mutate(e,c,[&](Json& value){value["state"]="failed";value["remote_state"]="rejected";value["node_errors"]=submitted.value("node_errors",Json::object());value["submission_error"]=submitted;});
                reject(Domain::ErrorCodes::InvalidRequest,"Provider rejected workflow: "+submitted.dump());
            }
            if(submitted.value("prompt_id",std::string{})!=r.at("prompt_id").get<std::string>()) reject(Domain::ErrorCodes::IntegrityFailure,"Provider prompt acknowledgement does not match the sealed ID.");
            mutate(e,c,[&](Json& value){value["submission_acknowledged"]=true;value["node_errors"]=submitted.value("node_errors",Json::object());value["state"]="queued";value["phase"]="queued";});
        }else phase("reconciling_provider");
        r=current(e);
        if(!r.at("prompt_id").is_string() || !r.contains("graph")) reject(Domain::ErrorCodes::Conflict,"No sealed existing provider submission can be resumed.");
        if(!r.contains("preview_motion_output_nodes") || !r.contains("final_motion_output_nodes")) {
            const auto preview=preflight(r.at("preview_graph"),r.at("preview_expected_outputs"),config,a,c);
            const auto finalCheck=preflight(r.at("final_graph"),r.at("final_expected_outputs"),config,a,c);
            if(!preview.at("ready").get<bool>() || !finalCheck.at("ready").get<bool>()) reject(Domain::ErrorCodes::Conflict,"Saved workflow output contracts cannot be verified during exact-job recovery.");
            mutate(e,c,[&](Json& value){value["preview_motion_output_nodes"]=preview.at("motion_output_nodes");value["final_motion_output_nodes"]=finalCheck.at("motion_output_nodes");
                value["motion_preview_required"]=value.at("media_kind")=="video" || !preview.at("motion_output_nodes").empty() || !finalCheck.at("motion_output_nodes").empty();});
            r=current(e);
        }
        auto expected=r.at(final?"final_expected_outputs":"preview_expected_outputs"); Json history;
        while(true) {
            check(c);auto freshAuthority=fresh(r,c);
            auto id=r.at("prompt_id").get<std::string>();auto response=request("GET","/history/"+id,Json::object(),config,freshAuthority,c);
            if(response.contains(id)) {
                history=response.at(id);
                if(!history.contains("prompt") || !history.at("prompt").is_array() || history.at("prompt").size()<3U ||
                    history.at("prompt")[1]!=r.at("prompt_id") || !ComfyDetail::promptGraphMatches(r.at("graph"),history.at("prompt")[2]))
                    reject(Domain::ErrorCodes::IntegrityFailure,"Provider history graph or prompt ID does not match the sealed request.");
                auto status=history.value("status",Json::object());
                if(status.value("status_str",std::string{})=="error") {
                    mutate(e,c,[&](Json& value){value["remote_state"]="failed";value["state"]="failed";value["provider_error"]=status;});
                    reject(Domain::ErrorCodes::ProcessExitNonzero,"ComfyUI execution failed: "+status.dump());
                }
                if(status.value("completed",false) && status.value("status_str",std::string{})=="success") {
                    mutate(e,c,[](Json& value){value["remote_state"]="completed";}); break;
                }
            }
            auto queue=request("GET","/queue",Json::object(),config,freshAuthority,c);auto state=std::string{"not_observed"};
            const auto events=backend_->perform("progress",Json{{"action","poll"},{"client_id","forge-"+r.at("job_id").get<std::string>()},{"prompt_id",r.at("prompt_id")}}.dump(),config,freshAuthority,c);
            if(events) {
                const auto observed=parse(events.value()).value("events",Json::array());
                if(observed.is_array() && !observed.empty()) mutate(e,c,[&](Json& value){
                    value["provider_events"]=observed;for(const auto& event:observed) {
                        const auto data=event.value("data",Json::object());
                        if(event.value("type",std::string{})=="progress" && data.is_object())value["progress"]=data;
                    }
                });
            }
            for(const auto* group:{"queue_running","queue_pending"}) for(const auto& item:queue.value(group,Json::array()))
                if(item.is_array() && item.size()>2U && item[1]==r.at("prompt_id")) {
                    if(!ComfyDetail::promptGraphMatches(r.at("graph"),item[2])) reject(Domain::ErrorCodes::IntegrityFailure,"Provider queue graph differs from the sealed request.");
                    state=std::string{group}=="queue_running"?"running":"queued";
                }
            mutate(e,c,[&](Json& value){value["remote_state"]=state;value["state"]=state=="running"?"running":"queued";value["phase"]=value.at("state");});
            std::unique_lock lock{e->mutex};e->changed.wait_for(lock,std::chrono::milliseconds{250});
        }
        auto outputs=history.value("outputs",Json::object());
        for(const auto& id:expected) if(!outputs.contains(id.get<std::string>())) reject(Domain::ErrorCodes::IntegrityFailure,"An intended output was omitted by provider execution: "+id.get<std::string>());
        mutate(e,c,[](Json& value){value["state"]="fetching";value["phase"]="collecting_artifacts";value["remote_state"]="completed";});
        auto currentAuthority=fresh(r,c); auto suffix=final?"final":"preview";
        auto destination=native(r.at("output_directory").get<std::string>())/suffix; auto u=destination.u8string();
        const Json collectionArguments{{"prompt_id",r.at("prompt_id")},{"outputs",outputs},{"output_directory",std::string{reinterpret_cast<const char*>(u.data()),u.size()}},
            {"namespace","ForgeConductor/"+r.at("project_id").get<std::string>()+"/"+r.at("job_id").get<std::string>()+"/"+suffix},
            {"expected_outputs",expected}};
        check(c);auto collected=parse(take(backend_->perform("collect",collectionArguments.dump(),config,currentAuthority,c)));
        const auto collectionCleanup=workerContext(current(e),{},10U);
        mutate(e,collectionCleanup,[&](Json& value){value["collection_outcome"]=collected;});
        auto artifacts=collected.value("artifacts",Json::array());
        if(!collected.value("ok",true)) {
            const auto failure=collected.value("error",Json::object());
            reject(failure.value("code",std::string{Domain::ErrorCodes::IntegrityFailure}),failure.value("message",std::string{"Provider outputs are partial or unverifiable: "+collected.value("unresolved",Json::array()).dump()}));
        }
        check(c);
        if(artifacts.empty()) reject(Domain::ErrorCodes::IntegrityFailure,"Workflow produced no verified publishable artifacts.");
        if(std::none_of(artifacts.begin(),artifacts.end(),[&](const auto& file){
            const auto type=file.value("media_type",std::string{});const auto metadata=file.value("metadata",Json::object());
            const auto kind=r.at("media_kind").get<std::string>();
            const bool motionRequired=r.value("motion_preview_required",kind=="video");
            const bool image=type.starts_with("image/") && !motionRequired;
            const bool motion=(motionRequired || kind!="image") && decodedMotion(file);
            return metadata.value("decoded",false) && (image || motion);
        })) reject(Domain::ErrorCodes::IntegrityFailure,"Preview has no decoded reviewable image or playable motion video.");
        for(const auto& id:expected) if(std::none_of(artifacts.begin(),artifacts.end(),[&](const auto& file){return file.value("node_id",std::string{})==id.get<std::string>();}))
            reject(Domain::ErrorCodes::IntegrityFailure,"Required output has no verified artifact: "+id.get<std::string>());
        for(const auto& id:r.at(final?"final_motion_output_nodes":"preview_motion_output_nodes"))
            if(std::none_of(artifacts.begin(),artifacts.end(),[&](const auto& file){return file.value("node_id",std::string{})==id.get<std::string>() && decodedMotion(file);}))
                reject(Domain::ErrorCodes::IntegrityFailure,"Intended motion output has no decoded playable artifact: "+id.get<std::string>());
        auto observation=final?Json::object():observe(a.projectId(),c);
        if(!final && observation.at("conversation_id")!=r.at("conversation_id"))
            reject(Domain::ErrorCodes::Conflict,"LM Studio selected conversation changed while rendering; reattach from the admitted conversation to publish its preview.");
        mutate(e,c,[&](Json& value) {
            if(value.value("cancellation_requested",false) || value.value("publication_suppressed",false)) reject(Domain::ErrorCodes::Cancelled,"Local artifact publication was cancelled.");
            value["artifacts"]=artifacts;value["state"]=final?"completed":"awaiting_preview_approval";value["phase"]=value.at("state");value["error"]=nullptr;
            if(!final) { value["preview_artifacts"]=artifacts;value["conversation_id"]=observation.at("conversation_id");
                value["conversation_prefix"]=observation.at("user_messages");value["approval_boundary"]=observation.at("user_messages").size(); }
        });
    }
    void cancel(const std::shared_ptr<Entry>& e,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        auto r=current(e);scope(r,a,true);
        if(r.at("state")=="completed") reject(Domain::ErrorCodes::Conflict,"Completed artifacts cannot be cancelled.");
        mutate(e,c,[](Json& value){value["cancellation_requested"]=true;value["publication_suppressed"]=true;value["state"]="cancelled";value["phase"]="cancelled";});
        e->stop.request_stop(); e->changed.notify_all();
        if(r.at("prompt_id").is_string()) {
            auto config=settings(c); if(configJson(config)!=r.at("configuration")) return;
            auto outcome=call("cancel",{{"prompt_id",r.at("prompt_id")},{"graph",r.value("graph",Json::object())}},config,a,c);
            mutate(e,c,[&](Json& value){value["cancel_outcome"]=outcome;});
        }
    }
    void resume(const std::shared_ptr<Entry>& e,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& a,const Domain::OperationContext& c) {
        std::unique_lock lock{e->mutex};scope(e->record,a,true);
        if(!e->finished) reject(Domain::ErrorCodes::Conflict,"ComfyUI job is still owned by an active worker.");
        if(configJson(config)!=e->record.at("configuration")) reject(Domain::ErrorCodes::Conflict,"ComfyUI provider configuration changed.");
        if(e->record.at("state")=="awaiting_preview_approval" || e->record.at("state")=="completed") return;
        if(e->record.value("remote_state",std::string{})=="rejected" || e->record.value("remote_state",std::string{})=="failed")
            reject(Domain::ErrorCodes::Conflict,"The provider reported a definite failure; revise the workflow and request a new preview.");
        if(e->record.at("operation")=="render" && !e->record.at("prompt_id").is_string()) reject(Domain::ErrorCodes::Conflict,"There is no exact generation to reattach; a new preview must be requested.");
        auto next=e->record;next["state"]="before_dispatch";next["publication_suppressed"]=false;next["cancellation_requested"]=false;next["error"]=nullptr;next["phase"]="waiting_provider_lease";
        if(next.contains("collection_outcome") && next.at("collection_outcome").is_object()) next["collection_outcome"]["previous_attempt"]=true;
        reserveActive(e);
        try {persist(next,c);e->record=std::move(next);} catch(...) {releaseActive(e);throw;}
        bool attach=e->record.at("operation")=="render";lock.unlock();launch(e,config,attach);
    }
    Contracts::IWorkspaceAuthority& authority_;
    Contracts::IAtomicFileStore& files_;
    Contracts::IConfigurationStore& configuration_;
    Contracts::IWorkspaceAuthority& storageAuthority_;
    Contracts::WorkspaceAuthority storageScope_;
    Domain::PathText jobsRoot_;
    Contracts::IUuidGenerator& uuids_;
    Contracts::IClock& clock_;
    Contracts::IHasher& hasher_;
    std::shared_ptr<Contracts::IComfyUiBackend> backend_;
    std::mutex observerMutex_,entriesMutex_;
    std::function<Domain::Result<std::string>(const Domain::ProjectId&,std::string_view,const Domain::OperationContext&)> observer_;
    std::map<std::string,std::shared_ptr<Entry>> entries_;
    std::atomic<std::size_t> activeJobs_{};
    std::timed_mutex providerMutex_;
    std::atomic<bool> stopped_{};
};

WindowsComfyUiService::WindowsComfyUiService(Contracts::IWorkspaceAuthority& authority,
    Contracts::IAtomicFileStore& files,Contracts::IConfigurationStore& configuration,
    Contracts::IWorkspaceAuthority& storageAuthority,const Contracts::WorkspaceAuthority& storageScope,
    Domain::PathText jobsRoot,Contracts::IUuidGenerator& uuids,Contracts::IClock& clock,Contracts::IHasher& hasher,
    std::shared_ptr<Contracts::IComfyUiBackend> backend)
    : implementation_(std::make_unique<Impl>(authority,files,configuration,storageAuthority,storageScope,std::move(jobsRoot),uuids,clock,hasher,std::move(backend))) {}
WindowsComfyUiService::~WindowsComfyUiService() noexcept { shutdown(); }
Domain::Result<std::string> WindowsComfyUiService::execute(std::string_view name,std::string_view arguments,
    const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& c) noexcept {
    return implementation_->execute(name,arguments,authority,c);
}
void WindowsComfyUiService::setConversationObserver(std::function<Domain::Result<std::string>(const Domain::ProjectId&,std::string_view,const Domain::OperationContext&)> observer) { implementation_->observer(std::move(observer)); }
void WindowsComfyUiService::shutdown() noexcept { if(implementation_) implementation_->shutdown(); }
} // namespace ForgeConductor::NativeTools::Windows
