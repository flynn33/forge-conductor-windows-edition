#include "ForgeConductor/NativeTools/Windows/WindowsImageProviderService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsImageProviderHttpTransport.h"
#include "ForgeConductor/Domain/ManagedRunModels.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "ImageProviderCodec.h"
#include "NativeFileOperations.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <unordered_set>

namespace ForgeConductor::NativeTools::Windows {
namespace {
using Json = nlohmann::json;
using State = Domain::ImageJobState;
using Record = Domain::ImageJobRecord;
using Pixels = Detail::ImageProviderPixels;
constexpr std::size_t MaximumImageBytes = 16U * 1024U * 1024U;
constexpr std::size_t MaximumJsonBytes = 512U * 1024U;
constexpr std::size_t MaximumReceiptBytes = 128U * 1024U;
constexpr std::size_t MaximumActiveJobs = 8U;
struct Failure final { Domain::Error error; };
[[noreturn]] void reject(std::string_view code, std::string message) {
    throw Failure{Domain::makeError(code, std::move(message))};
}
template<class T> T take(Domain::Result<T> result) {
    if (!result) throw Failure{result.error()}; return std::move(result).value();
}
void take(Domain::Result<void> result) { if (!result) throw Failure{result.error()}; }
void check(const Domain::OperationContext& context) {
    take(Infrastructure::Windows::Detail::validateOperationContext(context,
        std::chrono::steady_clock::now(), "image provider job"));
}
Json parse(std::string_view input, std::size_t maximum, const Domain::OperationContext& context) {
    check(context);
    if (input.size() > maximum || !Domain::isValidUtf8(input) || input.find('\0') != std::string_view::npos)
        reject(Domain::ErrorCodes::PayloadTooLarge, "Image-provider JSON exceeds its UTF-8 byte bound.");
    std::vector<std::unordered_set<std::string>> keys;
    auto value = Json::parse(input, [&](int depth, Json::parse_event_t event, Json& item) {
        check(context);
        if (depth > 32) reject(Domain::ErrorCodes::LimitExceeded, "Image-provider JSON depth exceeds 32.");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            if (keys.empty() || !keys.back().insert(item.get<std::string>()).second)
                reject(Domain::ErrorCodes::InvalidRequest, "Image-provider JSON contains duplicate keys.");
        } else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
    if (!value.is_object()) reject(Domain::ErrorCodes::InvalidRequest, "Image-provider JSON must be an object.");
    return value;
}
std::span<const std::byte> bytes(std::string_view value) {
    return std::as_bytes(std::span{value.data(), value.size()});
}
std::string text(const Json& object, std::string_view name, std::size_t maximum,
    bool required = true, std::string fallback = {}) {
    const auto item = object.find(std::string{name});
    if (item == object.end()) {
        if (required) reject(Domain::ErrorCodes::InvalidRequest, std::string{name} + " is required.");
        return fallback;
    }
    if (!item->is_string()) reject(Domain::ErrorCodes::InvalidRequest, std::string{name} + " must be text.");
    auto result = item->get<std::string>();
    if ((required && result.empty()) || result.size() > maximum || result.find('\0') != std::string::npos || !Domain::isValidUtf8(result))
        reject(Domain::ErrorCodes::InvalidRequest, std::string{name} + " is outside its UTF-8 bound.");
    return result;
}
std::uint64_t integer(const Json& object, std::string_view name, std::uint64_t fallback,
    std::uint64_t minimum, std::uint64_t maximum, bool required = false) {
    const auto item = object.find(std::string{name});
    if (item == object.end()) {
        if (required) reject(Domain::ErrorCodes::InvalidRequest, std::string{name} + " is required.");
        return fallback;
    }
    if (!item->is_number_integer() || (!item->is_number_unsigned() && item->get<std::int64_t>() < 0))
        reject(Domain::ErrorCodes::InvalidRequest, std::string{name} + " must be a bounded integer.");
    const auto result = item->get<std::uint64_t>();
    if (result < minimum || result > maximum) reject(Domain::ErrorCodes::InvalidRequest, std::string{name} + " is outside its bounds.");
    return result;
}
double number(const Json& object, std::string_view name, double fallback, double minimum, double maximum) {
    const auto item = object.find(std::string{name}); if (item == object.end()) return fallback;
    if (!item->is_number()) reject(Domain::ErrorCodes::InvalidRequest, std::string{name} + " must be numeric.");
    auto result = item->get<double>();
    if (!std::isfinite(result) || result < minimum || result > maximum)
        reject(Domain::ErrorCodes::InvalidRequest, std::string{name} + " is outside its bounds.");
    return result;
}
Domain::PathText path(std::string_view value) {
    auto native = std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(value.data()), value.size()}};
    if (!native.is_absolute()) reject(Domain::ErrorCodes::InvalidRequest, "Image-provider paths must be absolute.");
    const auto preferred = native.make_preferred().u8string();
    return take(Domain::PathText::create(std::string_view{reinterpret_cast<const char*>(preferred.data()),preferred.size()}));
}
Domain::ImageJobRequest request(const Json& args, bool edit) {
    const std::set<std::string> names{"prompt", "negative_prompt", "path", "source_path", "mask_path", "seed",
        "width", "height", "steps", "cfg", "denoise", "timeout_sec", "preview_max_dimension"};
    for (const auto& [name, unused] : args.items()) if (!names.contains(name))
        reject(Domain::ErrorCodes::InvalidRequest, "Unknown image-provider request field.");
    Domain::ImageJobRequest result;
    result.prompt = text(args, "prompt", 4096U); result.negativePrompt = text(args, "negative_prompt", 4096U, false);
    result.destination = path(text(args, "path", 32768U)).value();
    auto extension = std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(result.destination.data()), result.destination.size()}}.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](char8_t c) { return c >= u8'A' && c <= u8'Z' ? static_cast<char8_t>(c + 32) : c; });
    if (extension != u8".png") reject(Domain::ErrorCodes::InvalidRequest, "Image destination must use .png.");
    if (edit) result.source = path(text(args, "source_path", 32768U)).value();
    else if (args.contains("source_path") || args.contains("mask_path"))
        reject(Domain::ErrorCodes::InvalidRequest, "Generation cannot contain source or mask paths.");
    if (args.contains("mask_path")) result.mask = path(text(args, "mask_path", 32768U)).value();
    result.seed = integer(args, "seed", 0U, 0U, 9007199254740991ULL, true);
    result.width = static_cast<std::uint32_t>(integer(args, "width", 0U, 64U, 1024U, true));
    result.height = static_cast<std::uint32_t>(integer(args, "height", 0U, 64U, 1024U, true));
    if (result.width % 8U || result.height % 8U) reject(Domain::ErrorCodes::InvalidRequest, "Image dimensions must be multiples of eight.");
    result.steps = static_cast<std::uint32_t>(integer(args, "steps", 20U, 1U, 100U));
    result.cfg = number(args, "cfg", 7.0, 0.0, 20.0); result.denoise = number(args, "denoise", 1.0, 0.05, 1.0);
    result.previewMaxDimension = static_cast<std::uint32_t>(integer(args, "preview_max_dimension", 256U, 128U, 2048U));
    result.timeoutSeconds = static_cast<std::uint32_t>(integer(args, "timeout_sec", 1800U, 1U, 3600U));
    return result;
}
Json encodedRequest(const Domain::ImageJobRequest& r) {
    Json result{{"prompt",r.prompt},{"negative_prompt",r.negativePrompt},{"path",r.destination},{"seed",r.seed},
        {"width",r.width},{"height",r.height},{"steps",r.steps},{"cfg",r.cfg},{"denoise",r.denoise},
        {"timeout_sec",r.timeoutSeconds},{"preview_max_dimension",r.previewMaxDimension}};
    if (r.source) result["source_path"] = *r.source; if (r.mask) result["mask_path"] = *r.mask;
    return result;
}
Json providerJson(const Domain::ImageProviderConfig& c) {
    return {{"enabled",c.enabled},{"endpoint",c.endpoint},{"profile",c.profile},{"checkpoint",c.checkpoint}};
}
Json optional(const std::optional<std::string>& value) { return value ? Json(*value) : Json(nullptr); }
std::optional<std::string> optionalHash(const Json& value, const char* name) {
    if (value.at(name).is_null()) return std::nullopt;
    auto hash = text(value, name, 64U); take(Domain::Sha256Digest::parse(hash)); return hash;
}
Json payload(const Record& r) {
    Json value{{"schema_version",1U},{"kind","forge_image_job"},{"job_id",r.jobId},
        {"scope",{{"project_id",r.scope.projectId},{"client_id",r.scope.clientId},{"roots",r.scope.roots},
            {"grants",r.scope.grants},{"denials",r.scope.denials},{"generation",r.scope.generation}}},
        {"provider",providerJson(r.provider)},{"request",encodedRequest(r.request)},
        {"state",static_cast<unsigned>(r.state)},{"prompt_id",r.promptId},{"graph_json",r.graphJson},{"graph_sha256",r.graphSha256},
        {"source_rgba_sha256",optional(r.sourceRgbaSha256)},{"mask_rgba_sha256",optional(r.maskRgbaSha256)},
        {"source_png_sha256",optional(r.sourcePngSha256)},{"mask_png_sha256",optional(r.maskPngSha256)},
        {"destination_existed",r.destinationExisted},{"destination_before_sha256",optional(r.destinationBeforeSha256)},
        {"remote_state",r.remoteState},{"submission_acknowledged",r.submissionAcknowledged},
        {"publication_suppressed",r.publicationSuppressed},{"cancellation_requested",r.cancellationRequested},
        {"queue_delete_accepted",r.queueDeleteAccepted},{"recovered",r.recovered},
        {"created_utc_ms",r.createdUtcMilliseconds},{"owner_host_pid",r.ownerHostPid},
        {"owner_host_creation_time",r.ownerHostCreationTime},{"owner_released",r.ownerReleased},
        {"artifact_sha256",optional(r.artifactSha256)},
        {"artifact_rgba_sha256",optional(r.artifactRgbaSha256)},{"artifact_bytes",r.artifactBytes},
        {"artifact_published",r.artifactPublished}};
    value["error"] = r.error ? Json{{"code",r.error->code},{"message",r.error->message},{"retryable",r.error->retryable}} : Json(nullptr);
    return value;
}
Record decodedRecord(const Json& p, std::string_view id, const Contracts::WorkspaceAuthority& authority) {
    if (p.at("schema_version") != 1U || p.at("kind") != "forge_image_job" || text(p,"job_id",36U) != id ||
        p.at("scope").at("project_id") != authority.projectId().value())
        reject(Domain::ErrorCodes::IntegrityFailure, "Image-job receipt identity does not match its project/path.");
    Record r; r.jobId = std::string{id};
    r.scope.projectId = p.at("scope").at("project_id").get<std::string>();
    r.scope.clientId = text(p.at("scope"), "client_id", 128U);
    r.scope.roots = p.at("scope").at("roots").get<std::vector<std::string>>();
    const auto accesses = [&](const char* key) {
        const auto& values=p.at("scope").at(key);
        if(!values.is_array()||values.size()>5U)reject(Domain::ErrorCodes::IntegrityFailure,"Image-job receipt access list exceeds its bound.");
        std::vector<Domain::FileAccess> result;
        for(const auto& value:values){
            if(!value.is_number_integer()||(!value.is_number_unsigned()&&value.get<std::int64_t>()<0)||value.get<std::uint64_t>()>4U)
                reject(Domain::ErrorCodes::IntegrityFailure,"Image-job receipt access must use exact bounded integers.");
            result.push_back(static_cast<Domain::FileAccess>(value.get<unsigned>()));
        }return result;
    };
    r.scope.grants = accesses("grants"); r.scope.denials = accesses("denials");
    r.scope.generation = integer(p.at("scope"), "generation", 0U, 1U, UINT64_MAX, true);
    if (r.scope.roots.empty() || r.scope.roots.size() > 32U || r.scope.grants.empty() || r.scope.grants.size() > 6U || r.scope.denials.size() > 6U)
        reject(Domain::ErrorCodes::IntegrityFailure, "Image-job receipt scope exceeds its bound.");
    for (const auto& root : r.scope.roots) static_cast<void>(path(root));
    if (std::set<std::string>{r.scope.roots.begin(),r.scope.roots.end()}.size() != r.scope.roots.size() ||
        std::set<Domain::FileAccess>{r.scope.grants.begin(),r.scope.grants.end()}.size() != r.scope.grants.size() ||
        std::set<Domain::FileAccess>{r.scope.denials.begin(),r.scope.denials.end()}.size() != r.scope.denials.size())
        reject(Domain::ErrorCodes::IntegrityFailure, "Image-job receipt scope contains duplicate entries.");
    for (const auto grant : r.scope.grants) if (grant != Domain::FileAccess::Read && grant != Domain::FileAccess::Write && grant != Domain::FileAccess::Create)
        reject(Domain::ErrorCodes::IntegrityFailure, "Image-job receipt contains an unsupported grant.");
    for (const auto denial : r.scope.denials) if (static_cast<unsigned>(denial) > static_cast<unsigned>(Domain::FileAccess::Execute) ||
        std::find(r.scope.grants.begin(),r.scope.grants.end(),denial) != r.scope.grants.end())
        reject(Domain::ErrorCodes::IntegrityFailure, "Image-job receipt scope contains an invalid denial.");
    const auto& c = p.at("provider"); r.provider = {c.at("enabled").get<bool>(),text(c,"endpoint",256U),text(c,"profile",32U),text(c,"checkpoint",128U)};
    take(Domain::validateImageProviderConfig(r.provider));
    r.request = request(p.at("request"), p.at("request").contains("source_path"));
    r.state = static_cast<State>(integer(p,"state",0U,0U,static_cast<unsigned>(State::Unknown),true));
    r.promptId = text(p,"prompt_id",36U); take(Domain::Uuid::parse(r.promptId));
    r.graphJson = text(p,"graph_json",16384U,false); r.graphSha256 = text(p,"graph_sha256",64U,false);
    if (!r.graphSha256.empty()) take(Domain::Sha256Digest::parse(r.graphSha256));
    r.sourceRgbaSha256 = optionalHash(p,"source_rgba_sha256"); r.maskRgbaSha256 = optionalHash(p,"mask_rgba_sha256");
    r.sourcePngSha256 = optionalHash(p,"source_png_sha256"); r.maskPngSha256 = optionalHash(p,"mask_png_sha256");
    r.destinationExisted = p.at("destination_existed").get<bool>(); r.destinationBeforeSha256 = optionalHash(p,"destination_before_sha256");
    if (r.destinationExisted != r.destinationBeforeSha256.has_value()) reject(Domain::ErrorCodes::IntegrityFailure,"Image-job destination precondition is incomplete.");
    r.remoteState = text(p,"remote_state",32U); r.submissionAcknowledged = p.at("submission_acknowledged").get<bool>();
    r.publicationSuppressed = p.at("publication_suppressed").get<bool>(); r.cancellationRequested = p.at("cancellation_requested").get<bool>();
    r.queueDeleteAccepted = p.at("queue_delete_accepted").get<bool>(); r.recovered = p.at("recovered").get<bool>();
    r.createdUtcMilliseconds = p.at("created_utc_ms").get<std::int64_t>();
    r.ownerHostPid = static_cast<std::uint32_t>(integer(p,"owner_host_pid",0U,1U,UINT32_MAX,true));
    r.ownerHostCreationTime = integer(p,"owner_host_creation_time",0U,1U,UINT64_MAX,true);
    r.ownerReleased = p.at("owner_released").get<bool>();
    r.artifactSha256 = optionalHash(p,"artifact_sha256"); r.artifactRgbaSha256 = optionalHash(p,"artifact_rgba_sha256");
    r.artifactBytes = integer(p,"artifact_bytes",0U,0U,MaximumImageBytes,true);
    r.artifactPublished = p.at("artifact_published").get<bool>();
    if (r.graphJson.empty() != r.graphSha256.empty() ||
        r.request.source.has_value() != r.sourcePngSha256.has_value() || r.request.source.has_value() != r.sourceRgbaSha256.has_value() ||
        r.request.mask.has_value() != r.maskPngSha256.has_value() || r.request.mask.has_value() != r.maskRgbaSha256.has_value() ||
        r.artifactSha256.has_value() != r.artifactRgbaSha256.has_value() ||
        r.artifactSha256.has_value() != (r.artifactBytes != 0U) ||
        (r.artifactPublished && !r.artifactSha256) || (r.state == State::Completed && !r.artifactPublished))
        reject(Domain::ErrorCodes::IntegrityFailure, "Image-job receipt contains inconsistent phase/artifact facts.");
    if (!p.at("error").is_null()) r.error = Domain::makeError(text(p.at("error"),"code",128U),
        text(p.at("error"),"message",2048U),p.at("error").at("retryable").get<bool>());
    if (payload(r) != p) reject(Domain::ErrorCodes::IntegrityFailure, "Image-job receipt shape is not canonical.");
    return r;
}
std::string stateName(State state) {
    switch (state) {
    case State::BeforeDispatch:return "before_dispatch"; case State::Submitting:return "submitting";
    case State::Queued:return "queued"; case State::Running:return "running"; case State::Fetching:return "fetching";
    case State::Completed:return "completed"; case State::Failed:return "failed"; case State::Cancelled:return "cancelled"; case State::Unknown:return "unknown";
    } return "unknown";
}
bool terminal(State state) { return state == State::Completed || state == State::Failed || state == State::Cancelled || state == State::Unknown; }
std::string percent(std::string_view input) {
    constexpr char hex[] = "0123456789ABCDEF"; std::string out;
    for (unsigned char c : input) {
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c>>4U]; out += hex[c&15U]; }
    } return out;
}
Json graph(const Record& r, const std::optional<std::string>& source, const std::optional<std::string>& mask) {
    const auto node = [](const char* type, Json input) { return Json{{"class_type",type},{"inputs",std::move(input)}}; };
    Json out{{"1",node("CheckpointLoaderSimple",{{"ckpt_name",r.provider.checkpoint}})},
        {"2",node("CLIPTextEncode",{{"text",r.request.prompt},{"clip",Json::array({"1",1})}})},
        {"3",node("CLIPTextEncode",{{"text",r.request.negativePrompt},{"clip",Json::array({"1",1})}})},
        {"5",node("KSampler",{{"model",Json::array({"1",0})},{"positive",Json::array({"2",0})},{"negative",Json::array({"3",0})},
            {"latent_image",Json::array({"4",0})},{"seed",r.request.seed},{"steps",r.request.steps},{"cfg",r.request.cfg},
            {"sampler_name","dpmpp_2m"},{"scheduler","karras"},{"denoise",r.request.denoise}})},
        {"6",node("VAEDecode",{{"samples",Json::array({"5",0})},{"vae",Json::array({"1",2})}})},
        {"7",node("SaveImage",{{"images",Json::array({"6",0})},{"filename_prefix","ForgeConductor/"+r.scope.projectId+"/"+r.jobId+"/image"}})}};
    if (!source) out["4"] = node("EmptyLatentImage",{{"width",r.request.width},{"height",r.request.height},{"batch_size",1}});
    else {
        out["8"] = node("LoadImage",{{"image",*source}});
        out["4"] = node(mask ? "VAEEncodeForInpaint" : "VAEEncode",{{"pixels",Json::array({"8",0})},{"vae",Json::array({"1",2})}});
        if (mask) { out["9"] = node("LoadImageMask",{{"image",*mask},{"channel","red"}});
            out["4"]["inputs"]["mask"] = Json::array({"9",0}); out["4"]["inputs"]["grow_mask_by"] = 0; }
    }
    return out;
}
}

class WindowsImageProviderService::Impl final {
    struct Remote final {
        std::string state{"unknown"};
        std::optional<std::string> filename;
        std::optional<std::string> subfolder;
        std::optional<Domain::Error> error;
    };
    struct Entry final {
        std::mutex control;
        std::mutex workerMutex;
        std::mutex mutex;
        std::condition_variable changed;
        Record record;
        std::optional<Pixels> original;
        std::optional<Pixels> mask;
        std::optional<Remote> observed;
        std::stop_source cancellation;
        std::jthread worker;
        std::atomic<bool> finished{true};
    };
public:
    Impl(Contracts::IWorkspaceAuthority& authority, Contracts::IAtomicFileStore& files,
        Contracts::IConfigurationStore& configuration, Contracts::IWorkspaceAuthority& storageAuthority,
        const Contracts::WorkspaceAuthority& storageScope, Domain::PathText jobsRoot,
        Contracts::IUuidGenerator& uuids, Contracts::IClock& clock, Contracts::IHasher& hasher,
        std::shared_ptr<Contracts::IImageProviderHttpTransport> transport)
        : authority_{authority}, files_{files}, configuration_{configuration}, storageAuthority_{storageAuthority},
          storageScope_{storageScope}, jobsRoot_{std::move(jobsRoot)}, uuids_{uuids}, clock_{clock}, hasher_{hasher},
          transport_{transport ? std::move(transport) : std::make_shared<WindowsImageProviderHttpTransport>()} {}
    ~Impl() { shutdown(); }

    Domain::Result<std::string> execute(std::string_view name, std::string_view serialized,
        const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& context) noexcept {
        try {
            check(context);
            if (stopped_.load()) reject(Domain::ErrorCodes::HostCapabilityUnavailable, "Image-provider owner has shut down.");
            auto args = parse(serialized, 64U * 1024U, context);
            Json result;
            if (name == "image_provider_status") {
                if (!args.empty()) reject(Domain::ErrorCodes::InvalidRequest, "image_provider_status accepts no arguments.");
                result = providerStatus(context);
            } else if (name == "image_generate" || name == "image_edit") {
                result = start(request(args, name == "image_edit"), authority, context);
            } else if (name == "image_job_status" || name == "image_job_cancel" || name == "image_job_resume") {
                for (const auto& [key, unused] : args.items()) if (key != "job_id" && !(name == "image_job_status" && key == "wait_sec"))
                    reject(Domain::ErrorCodes::InvalidRequest, "Unknown image-job argument.");
                auto id = take(Domain::Uuid::parse(text(args,"job_id",36U))).value();
                auto entry = lookup(id, authority, context);
                if (name == "image_job_cancel") cancel(entry, authority, context);
                else if (name == "image_job_resume") resume(entry, authority, context);
                else {
                    const auto wait = integer(args,"wait_sec",0U,0U,60U);
                    std::unique_lock lock{entry->mutex};
                    const auto end = (std::min)(context.deadline, std::chrono::steady_clock::now() + std::chrono::seconds{wait});
                    while (!entry->finished.load() && std::chrono::steady_clock::now() < end) {
                        check(context); entry->changed.wait_for(lock, std::chrono::milliseconds{50});
                    }
                    lock.unlock();
                    if (entry->finished.load()) observeRecovered(entry, authority, context);
                }
                result = snapshot(entry, authority, context);
            } else reject(Domain::ErrorCodes::InvalidRequest, "Unknown image-provider tool.");
            check(context); return Domain::Result<std::string>::success(result.dump());
        } catch (const Failure& error) { return Domain::Result<std::string>::failure(error.error); }
        catch (const Json::exception&) { return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "Malformed image-provider request or response shape.")); }
        catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Image-provider operation failed.")); }
    }

    void shutdown() noexcept {
        if (stopped_.exchange(true)) return;
        std::vector<std::shared_ptr<Entry>> entries;
        { std::lock_guard lock{entriesMutex_}; for (const auto& [unused, entry] : entries_) entries.push_back(entry); }
        for (auto& entry : entries) {
            if (entry->finished.load()) continue;
            { std::lock_guard lock{entry->mutex};
                if (entry->record.artifactPublished) continue;
                entry->record.publicationSuppressed = true; entry->cancellation.request_stop(); }
        }
        for (auto& entry : entries) join(entry);
    }
private:
    std::string hash(std::span<const std::byte> value, const Domain::OperationContext& context) {
        check(context); auto digest = take(hasher_.sha256(value)).value(); check(context); return digest;
    }
    Domain::OperationContext internalContext(const Record& record, std::stop_token stop = {},
        std::chrono::seconds timeout = std::chrono::seconds{30}) {
        return {take(Domain::OperationId::parse(record.jobId)), clock_.monotonicNow() + timeout,
            stop, take(Domain::CorrelationId::parse("image-job-" + record.jobId))};
    }
    Domain::PathText storagePath(const Record& record, const char* leaf) {
        const auto root = std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(jobsRoot_.value().data()), jobsRoot_.value().size()}};
        const auto native = (root / record.scope.projectId / record.jobId / leaf).generic_u8string();
        return path(std::string_view{reinterpret_cast<const char*>(native.data()),native.size()});
    }
    Contracts::AuthorizedPath stored(const Record& record, const char* leaf, Domain::FileAccess access,
        const Domain::OperationContext& context) {
        return take(storageAuthority_.authorize(storageScope_,{storagePath(record,leaf),std::nullopt,access,
            access == Domain::FileAccess::Create},context));
    }
    void writeStored(const Record& record, const char* leaf, std::span<const std::byte> value,
        const Domain::OperationContext& context) {
        auto existing = readStored(record,leaf,MaximumImageBytes,context);
        if (!existing && existing.error().code != Domain::ErrorCodes::RecordNotFound) throw Failure{existing.error()};
        auto output = stored(record,leaf,existing ? Domain::FileAccess::Write : Domain::FileAccess::Create,context);
        take(Detail::ensureAuthorizedParentDirectories(output,context)); take(files_.replace(output,value,false,context));
    }
    Domain::Result<std::vector<std::byte>> readStored(const Record& record, const char* leaf,
        std::size_t maximum, const Domain::OperationContext& context) {
        auto access = storageAuthority_.authorize(storageScope_,{storagePath(record,leaf),std::nullopt,Domain::FileAccess::Read,false},context);
        if (!access) return Domain::Result<std::vector<std::byte>>::failure(access.error());
        return files_.read(access.value(),maximum,context);
    }
    void persist(const Record& record, const Domain::OperationContext& context) {
        auto p = payload(record); auto encoded = p.dump();
        if (encoded.size() > MaximumReceiptBytes) reject(Domain::ErrorCodes::PayloadTooLarge,"Image-job receipt exceeds 128 KiB.");
        const auto envelope = Json{{"payload",std::move(p)},{"sha256",hash(bytes(encoded),context)}}.dump();
        writeStored(record,"receipt.json",bytes(envelope),context);
    }
    void scope(const Record& record, const Contracts::WorkspaceAuthority& authority, bool effects) {
        if (record.scope.projectId != authority.projectId().value() || record.scope.clientId != authority.callerId().value())
            reject(Domain::ErrorCodes::ProjectScopeMismatch,"Image job is not owned by this authenticated project/principal.");
        for (const auto& root : record.scope.roots) if (std::none_of(authority.trustedRoots().begin(),authority.trustedRoots().end(),
            [&](const auto& candidate){return candidate.value() == root;}))
            reject(Domain::ErrorCodes::PathOutsideAuthority,"Image-job roots are outside the current narrowed scope.");
        for (const auto access : record.scope.grants) {
            if (!effects && access != Domain::FileAccess::Read) continue;
            if (std::find(authority.grants().begin(),authority.grants().end(),access) == authority.grants().end() ||
                std::find(authority.denials().begin(),authority.denials().end(),access) != authority.denials().end())
                reject(Domain::ErrorCodes::Unauthorized,"Image-job access is outside the current narrowed scope.");
        }
    }
    Contracts::WorkspaceAuthority fresh(const Record& record, const Domain::OperationContext& context) {
        auto current = take(authority_.authorityFor(take(Domain::ProjectId::parse(record.scope.projectId)),context));
        scope(record,current,true); std::vector<Domain::PathText> roots;
        for (const auto& root : record.scope.roots) roots.push_back(path(root));
        return take(authority_.narrow(current,roots,record.scope.grants,false,current.generation()+1U,context));
    }
    Domain::ImageProviderConfig settings(const Domain::OperationContext& context) {
        auto provider = take(configuration_.reload(context)).imageProvider;
        take(Domain::validateImageProviderConfig(provider)); return provider;
    }
    void sameProvider(const Record& record, const Domain::OperationContext& context) {
        if (settings(context) != record.provider || !record.provider.enabled)
            reject(Domain::ErrorCodes::Conflict,"Image provider configuration changed; job will not be attached to another endpoint/profile.");
    }
    Contracts::ImageProviderHttpResponse http(const Domain::ImageProviderConfig& provider, std::string_view method,
        std::string_view route, std::string_view contentType, std::span<const std::byte> body,
        std::size_t maximum, const Domain::OperationContext& context) {
        return take(transport_->request(provider,method,route,contentType,body,maximum,context));
    }
    Json json(const Contracts::ImageProviderHttpResponse& response, const Domain::OperationContext& context) {
        if (response.status != 200U) reject(Domain::ErrorCodes::HostCapabilityUnavailable,
            "Image provider returned HTTP " + std::to_string(response.status) + '.');
        return parse(std::string_view{reinterpret_cast<const char*>(response.body.data()),response.body.size()},MaximumJsonBytes,context);
    }
    void preflight(const Domain::ImageProviderConfig& provider, bool edit, bool mask, const Domain::OperationContext& context) {
        const std::map<std::string,std::vector<std::string>> inputs{
            {"CheckpointLoaderSimple",{"ckpt_name"}}, {"CLIPTextEncode",{"text","clip"}},
            {"KSampler",{"model","seed","steps","cfg","sampler_name","scheduler","positive","negative","latent_image","denoise"}},
            {"VAEDecode",{"samples","vae"}}, {"SaveImage",{"images","filename_prefix"}},
            {"EmptyLatentImage",{"width","height","batch_size"}}, {"LoadImage",{"image"}},
            {"VAEEncode",{"pixels","vae"}}, {"VAEEncodeForInpaint",{"pixels","vae","mask","grow_mask_by"}},
            {"LoadImageMask",{"image","channel"}}};
        std::vector<std::string> nodes{"CheckpointLoaderSimple","CLIPTextEncode","KSampler","VAEDecode","SaveImage"};
        if (edit) { nodes.push_back("LoadImage"); nodes.push_back(mask ? "VAEEncodeForInpaint" : "VAEEncode");
            if (mask) nodes.push_back("LoadImageMask"); }
        else nodes.push_back("EmptyLatentImage");
        for (const auto& node : nodes) {
            auto info = json(http(provider,"GET","/object_info/"+node,{}, {},MaximumJsonBytes,context),context);
            if (!info.contains(node) || !info.at(node).is_object()) reject(Domain::ErrorCodes::HostCapabilityUnavailable,"Required image-provider core node is unavailable: " + node);
            const auto& required = info.at(node).at("input").at("required");
            for (const auto& field : inputs.at(node)) if (!required.contains(field) || !required.at(field).is_array() || required.at(field).empty())
                reject(Domain::ErrorCodes::HostCapabilityUnavailable,"Required image-provider node input is unavailable: "+node+'.'+field);
            const auto choice = [&](const char* key,const std::string& value) {
                const auto& choices = required.at(key).at(0);
                if (!choices.is_array() || std::find(choices.begin(),choices.end(),Json(value)) == choices.end())
                    reject(Domain::ErrorCodes::HostCapabilityUnavailable,"Configured image-provider choice is absent from current node inventory: "+node+'.'+key);
            };
            if (node == "CheckpointLoaderSimple") {
                choice("ckpt_name",provider.checkpoint);
            }
            if (node == "KSampler") { choice("sampler_name","dpmpp_2m"); choice("scheduler","karras"); }
            if (node == "LoadImageMask") choice("channel","red");
        }
    }
    Json providerStatus(const Domain::OperationContext& context) {
        auto provider = settings(context);
        Json result{{"ok",true},{"configured",provider.enabled},{"available",false},{"provider","comfyui"},
            {"configuration",providerJson(provider)},{"model_loaded_verified",false},{"active_interrupt_supported",false},
            {"generation_quality_verified",false},{"error",nullptr}};
        if (!provider.enabled) return result;
        try { preflight(provider,false,false,context); preflight(provider,true,true,context); result["available"] = true; }
        catch (const Failure& error) { result["error"] = {{"code",error.error.code},{"message",error.error.message}}; }
        return result;
    }
    Domain::Result<std::vector<std::byte>> readPath(const Contracts::WorkspaceAuthority& authority,
        const std::string& filename, const Domain::OperationContext& context) {
        auto access = authority_.authorize(authority,{path(filename),std::nullopt,Domain::FileAccess::Read,false},context);
        if (!access) return Domain::Result<std::vector<std::byte>>::failure(access.error());
        return files_.read(access.value(),MaximumImageBytes,context);
    }
    void captureRoot(Record& record, const Contracts::AuthorizedPath& access) {
        const auto& root = access.authorityRoot().value();
        if (std::find(record.scope.roots.begin(),record.scope.roots.end(),root) == record.scope.roots.end()) record.scope.roots.push_back(root);
    }
    static std::uint64_t hostCreation() {
        FILETIME creation{},exit{},kernel{},user{};
        if (!::GetProcessTimes(::GetCurrentProcess(),&creation,&exit,&kernel,&user))
            reject(Domain::ErrorCodes::HostCapabilityUnavailable,"The image-job host identity cannot be observed.");
        return (static_cast<std::uint64_t>(creation.dwHighDateTime)<<32U)|creation.dwLowDateTime;
    }
    static std::optional<bool> ownerAlive(const Record& r) {
        const auto process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,r.ownerHostPid);
        if (!process) { if (::GetLastError() == ERROR_INVALID_PARAMETER) return false; return std::nullopt; }
        FILETIME creation{},exit{},kernel{},user{};
        const auto measured = ::GetProcessTimes(process,&creation,&exit,&kernel,&user);
        const auto actual = (static_cast<std::uint64_t>(creation.dwHighDateTime)<<32U)|creation.dwLowDateTime;
        const auto waited = ::WaitForSingleObject(process,0U); ::CloseHandle(process);
        if (!measured || waited == WAIT_FAILED) return std::nullopt;
        return actual == r.ownerHostCreationTime && waited == WAIT_TIMEOUT;
    }
    void join(const std::shared_ptr<Entry>& entry) {
        std::lock_guard lock{entry->workerMutex};if(entry->worker.joinable())entry->worker.join();
    }
    void makeCacheRoom() {
        if(entries_.size()<256U)return;
        auto candidate=entries_.end();std::int64_t oldest=INT64_MAX;
        for(auto iterator=entries_.begin();iterator!=entries_.end();++iterator){
            if(iterator->second.use_count()!=1)continue;
            std::lock_guard lock{iterator->second->mutex};
            if(iterator->second->finished.load()&&iterator->second->record.createdUtcMilliseconds<=oldest){candidate=iterator;oldest=iterator->second->record.createdUtcMilliseconds;}
        }
        if(candidate==entries_.end())reject(Domain::ErrorCodes::LimitExceeded,"The Manager image-job cache has no unborrowed finished entry to retire.");
        join(candidate->second);entries_.erase(candidate);
    }
    void admissionCount(const Record& record, const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& context) {
        auto receipt = storagePath(record,"receipt.json");
        const auto native = std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(receipt.value().data()),receipt.value().size()}}.parent_path().parent_path().generic_u8string();
        auto projectDirectory = path(std::string_view{reinterpret_cast<const char*>(native.data()),native.size()});
        auto access = storageAuthority_.authorize(storageScope_,{projectDirectory,std::nullopt,Domain::FileAccess::Read,false},context);
        if (!access) { if (access.error().code == Domain::ErrorCodes::RecordNotFound) return; throw Failure{access.error()}; }
        auto opened = Detail::openAuthorizedDirectory(access.value(),context);
        if (!opened) { if (opened.error().code == Domain::ErrorCodes::RecordNotFound) return; throw Failure{opened.error()}; }
        const auto entries = take(Detail::enumerateDirectory(opened.value().handle.get(),64U,context));
        if (entries.size() < 32U) return;
        std::vector<Record> expired;
        for (const auto& item : entries) {
            check(context);
            if (!item.isDirectory() || item.isReparsePoint()) continue;
            try {
                const auto id = take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(item.name));
                if (!Domain::Uuid::parse(id)) continue;
                const auto key = record.scope.projectId+'/'+id;
                if (const auto cached=entries_.find(key);cached!=entries_.end() &&
                    (!cached->second->finished.load() || cached->second.use_count()!=1)) continue;
                Record stub;stub.jobId=id;stub.scope.projectId=record.scope.projectId;
                auto content=take(readStored(stub,"receipt.json",MaximumReceiptBytes+256U,context));
                auto envelope=parse(std::string_view{reinterpret_cast<const char*>(content.data()),content.size()},MaximumReceiptBytes+256U,context);
                if (envelope.size()!=2U || text(envelope,"sha256",64U)!=hash(bytes(envelope.at("payload").dump()),context)) continue;
                auto candidate=decodedRecord(envelope.at("payload"),id,authority);scope(candidate,authority,false);
                if (!candidate.graphJson.empty() && hash(bytes(candidate.graphJson),context)!=candidate.graphSha256) continue;
                if (!candidate.ownerReleased) {const auto alive=ownerAlive(candidate);if(!alive||*alive)continue;}
                const bool completed=candidate.state==State::Completed && candidate.artifactPublished;
                const bool failed=candidate.state==State::Failed && (candidate.graphJson.empty() || candidate.remoteState=="error" || candidate.remoteState=="rejected");
                if (completed || failed) expired.push_back(std::move(candidate));
            } catch(const Failure& failure) {
                if (failure.error.code==Domain::ErrorCodes::Cancelled || failure.error.code==Domain::ErrorCodes::DeadlineExceeded) throw;
                // Unverified or inaccessible evidence is retained and still occupies its slot.
            } catch(const Json::exception&) {}
        }
        std::sort(expired.begin(),expired.end(),[](const auto& a,const auto& b){return a.createdUtcMilliseconds<b.createdUtcMilliseconds;});
        auto count=entries.size();
        for (const auto& stale : expired) {
            if(count<32U)break;check(context);
            const auto file=std::filesystem::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(storagePath(stale,"receipt.json").value()))};
            auto parent=take(Domain::PathText::create(take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(file.parent_path().native()))));
            auto authorized=take(storageAuthority_.authorize(storageScope_,{parent,std::nullopt,Domain::FileAccess::Delete,true},context));
            auto directory=take(Detail::openAuthorizedObject(authorized,Domain::FileAccess::Delete,
                Infrastructure::Windows::Detail::MissingPathPolicy::Reject,context,
                DELETE|FILE_LIST_DIRECTORY|FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE));
            if(!directory.isDirectory())reject(Domain::ErrorCodes::IntegrityFailure,"Retained image evidence is not a directory.");
            auto childResult=Detail::enumerateDirectory(directory.handle.get(),4U,context);
            if(!childResult){check(context);continue;}const auto& children=childResult.value();
            if(std::any_of(children.begin(),children.end(),[](const auto& child){return child.isDirectory()||child.isReparsePoint()||
                (child.name!=L"receipt.json"&&child.name!=L"source.png"&&child.name!=L"mask.png");}))continue;
            // Receipt removal is last; interrupted cleanup never removes a published user destination.
            for(const auto* leaf:{L"source.png",L"mask.png",L"receipt.json"}) {
                if(std::none_of(children.begin(),children.end(),[&](const auto& child){return child.name==leaf;}))continue;
                const auto expected=directory.canonicalPath+L'\\'+leaf;
                auto child=take(Detail::openChildObject(directory.handle.get(),leaf,expected,DELETE|FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ|FILE_SHARE_WRITE,context));take(Detail::deleteOpenedObject(child.handle.get(),context));
            }
            take(Detail::deleteOpenedObject(directory.handle.get(),context));
            const auto key=stale.scope.projectId+'/'+stale.jobId;if(const auto cached=entries_.find(key);cached!=entries_.end())join(cached->second);
            entries_.erase(key);--count;
        }
        if(count>=32U)reject(Domain::ErrorCodes::LimitExceeded,
            "The 32 image-job slots contain live, unknown, cancelled or unverified evidence; they are preserved. No new job was admitted.");
    }
    Json start(Domain::ImageJobRequest requested, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) {
        auto provider = settings(context);
        if (!provider.enabled) reject(Domain::ErrorCodes::HostCapabilityUnavailable,"Image provider is disabled; configure an explicit endpoint/profile/checkpoint first.");
        auto entry = std::make_shared<Entry>(); auto& r = entry->record;
        r.jobId = take(uuids_.next()).value(); r.promptId = take(uuids_.next()).value();
        r.scope.projectId = authority.projectId().value(); r.scope.clientId = authority.callerId().value();
        r.scope.generation = authority.generation(); r.scope.denials = authority.denials(); r.provider = std::move(provider); r.request = std::move(requested);
        r.createdUtcMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(clock_.utcNow().time_since_epoch()).count();
        r.ownerHostPid = ::GetCurrentProcessId(); r.ownerHostCreationTime = hostCreation();
        auto old = readPath(authority,r.request.destination,context);
        if (!old && old.error().code != Domain::ErrorCodes::RecordNotFound) throw Failure{old.error()};
        r.destinationExisted = old.hasValue(); if (old) r.destinationBeforeSha256 = hash(old.value(),context);
        auto target = take(authority_.authorize(authority,{path(r.request.destination),std::nullopt,
            old ? Domain::FileAccess::Write : Domain::FileAccess::Create,!old},context));
        r.request.destination = target.canonicalPath().value(); captureRoot(r,target);
        r.scope.grants = {Domain::FileAccess::Read,old ? Domain::FileAccess::Write : Domain::FileAccess::Create};
        std::optional<std::vector<std::byte>> sourceBytes,maskBytes;
        const auto input = [&](std::optional<std::string>& filename, std::optional<Pixels>& pixels,
            std::optional<std::string>& rgbaHash, std::optional<std::string>& pngHash,
            std::optional<std::vector<std::byte>>& snapshot) {
            if (!filename) return;
            auto access = take(authority_.authorize(authority,{path(*filename),std::nullopt,Domain::FileAccess::Read,false},context));
            *filename = access.canonicalPath().value(); captureRoot(r,access);
            pixels = take(Detail::decodeProviderImage(take(files_.read(access,MaximumImageBytes,context)),context));
            if (pixels->width != r.request.width || pixels->height != r.request.height)
                reject(Domain::ErrorCodes::InvalidRequest,"Source/mask dimensions must exactly match requested width/height.");
            rgbaHash = hash(pixels->rgba,context); snapshot = take(Detail::encodeProviderImage(*pixels,context)); pngHash = hash(*snapshot,context);
        };
        input(r.request.source,entry->original,r.sourceRgbaSha256,r.sourcePngSha256,sourceBytes);
        input(r.request.mask,entry->mask,r.maskRgbaSha256,r.maskPngSha256,maskBytes);
        std::lock_guard admission{entriesMutex_};
        if (stopped_.load()) reject(Domain::ErrorCodes::HostCapabilityUnavailable,"Image-provider owner has shut down.");
        makeCacheRoom();
        if (std::count_if(entries_.begin(),entries_.end(),[](const auto& pair){return !pair.second->finished.load();}) >= MaximumActiveJobs)
            reject(Domain::ErrorCodes::LimitExceeded,"At most eight image jobs may be active in this Manager.");
        admissionCount(r,authority,context);
        auto existing = readStored(r,"receipt.json",MaximumReceiptBytes,context);
        if (existing) reject(Domain::ErrorCodes::Conflict,"Image job UUID already has a receipt; it will not be replayed.");
        if (existing.error().code != Domain::ErrorCodes::RecordNotFound) throw Failure{existing.error()};
        if (sourceBytes) writeStored(r,"source.png",*sourceBytes,context);
        if (maskBytes) writeStored(r,"mask.png",*maskBytes,context);
        persist(r,context); entries_.emplace(r.scope.projectId+"/"+r.jobId,entry);
        launch(entry,false);
        return snapshot(entry,authority,context);
    }
    std::shared_ptr<Entry> lookup(const std::string& id, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) {
        const auto key = authority.projectId().value()+"/"+id;
        std::lock_guard lock{entriesMutex_};
        if (const auto found = entries_.find(key); found != entries_.end()) {
            std::lock_guard entryLock{found->second->mutex}; scope(found->second->record,authority,false); return found->second; }
        makeCacheRoom();
        Record stub; stub.jobId = id; stub.scope.projectId = authority.projectId().value();
        auto raw = take(readStored(stub,"receipt.json",MaximumReceiptBytes+256U,context));
        auto envelope = parse(std::string_view{reinterpret_cast<const char*>(raw.data()),raw.size()},MaximumReceiptBytes+256U,context);
        if (envelope.size() != 2U || !envelope.contains("payload") || !envelope.contains("sha256") ||
            text(envelope,"sha256",64U) != hash(bytes(envelope.at("payload").dump()),context))
            reject(Domain::ErrorCodes::IntegrityFailure,"Image-job receipt seal does not match its bytes.");
        auto entry = std::make_shared<Entry>();
        try { entry->record = decodedRecord(envelope.at("payload"),id,authority); }
        catch (const Json::exception&) { reject(Domain::ErrorCodes::IntegrityFailure,"Image-job receipt has an invalid typed shape."); }
        scope(entry->record,authority,false);
        if (!entry->record.graphJson.empty() && hash(bytes(entry->record.graphJson),context) != entry->record.graphSha256)
            reject(Domain::ErrorCodes::IntegrityFailure,"Image-job workflow seal does not match.");
        if (!terminal(entry->record.state)) {
            entry->record.state = State::Unknown; entry->record.publicationSuppressed = true; entry->record.recovered = true;
            entry->record.error = Domain::makeError(Domain::ErrorCodes::TransportClosed,
                "Previous Manager ended before a final receipt; status will only observe the exact remote ID. Resume requires fresh authorization.");
        }
        entries_.emplace(key,entry); return entry;
    }
    Json snapshot(const std::shared_ptr<Entry>& entry, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) {
        Record record; std::optional<Remote> observed;
        { std::lock_guard lock{entry->mutex}; record = entry->record; observed = entry->observed; }
        scope(record,authority,false);
        Json result{{"ok",true},{"job_id",record.jobId},{"project_id",record.scope.projectId},
            {"state",stateName(record.state)},{"done",entry->finished.load()},{"provider","comfyui"},
            {"prompt_id",record.promptId},{"remote_state",record.remoteState},{"submission_acknowledged",record.submissionAcknowledged},
            {"publication_suppressed",record.publicationSuppressed},{"cancellation_requested",record.cancellationRequested},
            {"queue_delete_accepted",record.queueDeleteAccepted},{"remote_cancel_confirmed",false},
            {"remote_execution_may_continue",record.remoteState != "not_submitted" && record.remoteState != "completed" && record.remoteState != "error" && record.remoteState != "rejected"},
            {"recovered",record.recovered},{"path",record.request.destination},{"width",record.request.width},{"height",record.request.height},
            {"seed",record.request.seed},{"workflow_sha256",record.graphSha256.empty()?Json(nullptr):Json(record.graphSha256)},
            {"artifact_published",record.artifactPublished},{"artifact_sha256",record.artifactPublished?optional(record.artifactSha256):Json(nullptr)},
            {"decoded_rgba8_sha256",record.artifactPublished?optional(record.artifactRgbaSha256):Json(nullptr)},
            {"bytes_written",record.artifactPublished?Json(record.artifactBytes):Json(nullptr)},
            {"receipt_path",storagePath(record,"receipt.json").value()},{"receipt_integrity","sha256_not_origin_authentication"},
            {"observed_remote_state",observed?Json(observed->state):Json(nullptr)},
            {"observed_remote_error",observed&&observed->error?Json{{"code",observed->error->code},{"message",observed->error->message}}:Json(nullptr)},
            {"error",record.error?Json{{"code",record.error->code},{"message",record.error->message},{"retryable",record.error->retryable}}:Json(nullptr)}};
        if (record.artifactPublished) {
            if (!record.artifactSha256 || !record.artifactRgbaSha256 || record.artifactBytes == 0U)
                reject(Domain::ErrorCodes::IntegrityFailure,"Published image-job receipt lacks actual artifact facts.");
            auto content = take(readPath(authority,record.request.destination,context));
            if (content.size() != record.artifactBytes || hash(content,context) != *record.artifactSha256)
                reject(Domain::ErrorCodes::IntegrityFailure,"Published image artifact changed after the receipt was sealed.");
            auto image = take(Detail::decodeProviderImage(content,context));
            if (image.width != record.request.width || image.height != record.request.height || hash(image.rgba,context) != *record.artifactRgbaSha256)
                reject(Domain::ErrorCodes::IntegrityFailure,"Published image pixels disagree with their sealed receipt.");
            auto preview = take(Detail::previewProviderImage(image,record.request.previewMaxDimension,context));
            result["image_base64"] = std::move(preview.base64); result["image_mime_type"] = "image/png";
            result["preview_width"] = preview.width; result["preview_height"] = preview.height;
            result["preview_max_dimension_requested"] = record.request.previewMaxDimension;
            result["preview_encoded_bytes"] = result.at("image_base64").get_ref<const std::string&>().size();
            result["preview_encoded_byte_limit"] = Domain::MaximumManagedImagePreviewBase64Bytes;
            result["preview_reduced_for_byte_limit"] = preview.reducedForBytes; result["preview_png_sha256"] = std::move(preview.sha256);
        }
        return result;
    }
    std::string upload(const Record& r, const char* leaf, const char* suffix, const Domain::OperationContext& context) {
        auto content = take(readStored(r,leaf,MaximumImageBytes,context));
        const auto expected = std::string_view{leaf} == "source.png" ? r.sourcePngSha256 : r.maskPngSha256;
        if (!expected || hash(content,context) != *expected) reject(Domain::ErrorCodes::IntegrityFailure,"Captured image snapshot changed before provider upload.");
        const auto boundary = "ForgeConductor"+r.jobId+suffix;
        const auto filename = r.jobId+'-'+suffix+".png";
        std::string body = "--"+boundary+"\r\nContent-Disposition: form-data; name=\"type\"\r\n\r\ninput\r\n--"+boundary+
            "\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\nfalse\r\n--"+boundary+
            "\r\nContent-Disposition: form-data; name=\"image\"; filename=\""+filename+"\"\r\nContent-Type: image/png\r\n\r\n";
        body.append(reinterpret_cast<const char*>(content.data()),content.size()); body += "\r\n--"+boundary+"--\r\n";
        auto value = json(http(r.provider,"POST","/upload/image","multipart/form-data; boundary="+boundary,bytes(body),MaximumJsonBytes,context),context);
        const auto name = text(value,"name",128U);
        if (!name.starts_with(r.jobId+'-') || !name.ends_with(".png") ||
            std::any_of(name.begin(),name.end(),[](unsigned char c){return !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c==' '||c=='('||c==')');}) ||
            text(value,"type",16U) != "input" || !text(value,"subfolder",128U,false).empty())
            reject(Domain::ErrorCodes::IntegrityFailure,"Provider upload receipt does not name the owned input namespace.");
        return name;
    }
    Remote remote(const Record& record, const Domain::OperationContext& context) {
        auto history = json(http(record.provider,"GET","/history/"+record.promptId,{}, {},MaximumJsonBytes,context),context);
        if (history.contains(record.promptId)) {
            const auto& actual = history.at(record.promptId); const auto& prompt = actual.at("prompt");
            if (!prompt.is_array() || prompt.size() < 3U || prompt.at(1) != record.promptId || !prompt.at(2).is_object() ||
                hash(bytes(prompt.at(2).dump()),context) != record.graphSha256)
                reject(Domain::ErrorCodes::IntegrityFailure,"Remote history prompt/workflow does not match the sealed exact job ID.");
            const auto& status = actual.at("status"); const auto state = text(status,"status_str",16U);
            if (state == "error") return {"error",{},{},Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
                "The exact image-provider workflow failed; remote status is error.")};
            if (state != "success" || !status.at("completed").is_boolean() || !status.at("completed").get<bool>())
                return {};
            const auto& images = actual.at("outputs").at("7").at("images");
            if (!images.is_array() || images.size() != 1U) reject(Domain::ErrorCodes::IntegrityFailure,"Provider returned an unexpected number of SaveImage artifacts.");
            const auto filename = text(images.at(0),"filename",128U);
            const auto subfolder = text(images.at(0),"subfolder",256U);
            auto normalized = subfolder; std::replace(normalized.begin(),normalized.end(),'\\','/');
            const auto expected = "ForgeConductor/"+record.scope.projectId+"/"+record.jobId;
            if (text(images.at(0),"type",16U) != "output" || normalized != expected || !filename.starts_with("image_") ||
                !filename.ends_with("_.png") || std::any_of(filename.begin(),filename.end(),[](unsigned char c){return !((c>='0'&&c<='9')||c=='_'||c=='.'||(c>='a'&&c<='z'));}))
                reject(Domain::ErrorCodes::IntegrityFailure,"Provider output does not belong to the exact owned SaveImage namespace.");
            return {"completed",filename,subfolder,{}};
        }
        auto queue = json(http(record.provider,"GET","/queue",{}, {},MaximumJsonBytes,context),context);
        const auto find = [&](const char* key) {
            const auto& rows = queue.at(key);
            if (!rows.is_array()) reject(Domain::ErrorCodes::MalformedMessage,"Provider queue is not an array.");
            for (const auto& row : rows) {
                if (!row.is_array() || row.size() < 3U) reject(Domain::ErrorCodes::MalformedMessage,"Provider queue entry is malformed.");
                if (row.at(1) == record.promptId) {
                    if (!row.at(2).is_object() || hash(bytes(row.at(2).dump()),context) != record.graphSha256)
                        reject(Domain::ErrorCodes::IntegrityFailure,"Provider queue reused the job ID with a different workflow.");
                    return true;
                }
            } return false;
        };
        if (find("queue_running")) return {"running",{},{},{}};
        if (find("queue_pending")) return {"queued",{},{},{}};
        return {};
    }
    void observeRecovered(const std::shared_ptr<Entry>& entry, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) {
        Record record; { std::lock_guard lock{entry->mutex}; record = entry->record; }
        if ((record.state != State::Unknown && record.state != State::Cancelled) || record.graphJson.empty()) return;
        scope(record,authority,false); Remote observed;
        try { sameProvider(record,context); observed=remote(record,context); }
        catch(const Failure& failure) {
            if(failure.error.code==Domain::ErrorCodes::Cancelled||failure.error.code==Domain::ErrorCodes::DeadlineExceeded)throw;
            observed={"unavailable",{},{},failure.error};
        } catch(const Json::exception&) {
            observed={"unavailable",{},{},Domain::makeError(Domain::ErrorCodes::MalformedMessage,"Exact-ID provider observation has an invalid typed shape.")};
        }
        { std::lock_guard lock{entry->mutex}; entry->observed = std::move(observed); }
    }
    void update(const std::shared_ptr<Entry>& entry, const Domain::OperationContext& context,
        const std::function<void(Record&)>& change) {
        { std::lock_guard lock{entry->mutex}; change(entry->record); persist(entry->record,context); }
        entry->changed.notify_all();
    }
    bool destinationUnchanged(const Record& record, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context, bool allowPublishedIntent) {
        auto current = readPath(authority,record.request.destination,context);
        if (!current && current.error().code != Domain::ErrorCodes::RecordNotFound) throw Failure{current.error()};
        if (current && allowPublishedIntent && record.artifactSha256 && record.artifactRgbaSha256 &&
            current.value().size() == record.artifactBytes && hash(current.value(),context) == *record.artifactSha256) {
            auto pixels = take(Detail::decodeProviderImage(current.value(),context));
            if (pixels.width == record.request.width && pixels.height == record.request.height &&
                hash(pixels.rgba,context) == *record.artifactRgbaSha256) return true;
        }
        if (record.destinationExisted != current.hasValue() ||
            (current && (!record.destinationBeforeSha256 || hash(current.value(),context) != *record.destinationBeforeSha256)))
            reject(Domain::ErrorCodes::Conflict,"Image destination changed since admission; no artifact was written.");
        static_cast<void>(take(authority_.authorize(authority,{path(record.request.destination),std::nullopt,
            record.destinationExisted ? Domain::FileAccess::Write : Domain::FileAccess::Create,!record.destinationExisted},context)));
        return false;
    }
    void publish(const std::shared_ptr<Entry>& entry, const Record& record, const Remote& observed,
        const Domain::OperationContext& context) {
        if (!observed.filename || !observed.subfolder) reject(Domain::ErrorCodes::IntegrityFailure,"Completed provider job lacks its exact image tuple.");
        auto response = http(record.provider,"GET","/view?filename="+percent(*observed.filename)+"&subfolder="+percent(*observed.subfolder)+"&type=output",{}, {},MaximumImageBytes,context);
        constexpr std::array<unsigned char,8> magic{137U,80U,78U,71U,13U,10U,26U,10U};
        if (response.status != 200U || !response.contentType.starts_with("image/png") || response.body.size() < magic.size() ||
            !std::equal(magic.begin(),magic.end(),response.body.begin(),[](unsigned char a,std::byte b){return a == std::to_integer<unsigned char>(b);}))
            reject(Domain::ErrorCodes::MalformedMessage,"Provider output is not a complete PNG response.");
        auto image = take(Detail::decodeProviderImage(response.body,context));
        if (image.width != record.request.width || image.height != record.request.height)
            reject(Domain::ErrorCodes::IntegrityFailure,"Provider PNG dimensions differ from the admitted workflow.");
        if (entry->mask) {
            if (!entry->original) reject(Domain::ErrorCodes::IntegrityFailure,"Masked edit lacks its captured original pixels.");
            take(Detail::compositeProviderMask(image,*entry->original,*entry->mask,context));
        }
        auto content = take(Detail::encodeProviderImage(image,context));
        auto artifactHash = hash(content,context); auto rgbaHash = hash(image.rgba,context);
        static_cast<void>(take(Detail::previewProviderImage(image,record.request.previewMaxDimension,context)));
        auto current = fresh(record,context); sameProvider(record,context);
        std::lock_guard lock{entry->mutex}; check(context);
        if (entry->record.publicationSuppressed || entry->record.cancellationRequested)
            reject(Domain::ErrorCodes::Cancelled,"Image publication was cancelled.");
        const bool alreadyPublished = destinationUnchanged(entry->record,current,context,true);
        if (alreadyPublished && (entry->record.artifactSha256 != artifactHash || entry->record.artifactRgbaSha256 != rgbaHash ||
            entry->record.artifactBytes != content.size()))
            reject(Domain::ErrorCodes::IntegrityFailure,"Remote artifact changed after the persisted publication intent.");
        entry->record.state = State::Fetching; entry->record.remoteState = "completed";
        entry->record.artifactSha256 = artifactHash; entry->record.artifactRgbaSha256 = rgbaHash; entry->record.artifactBytes = content.size();
        persist(entry->record,context);
        if (!alreadyPublished) {
            auto output = take(authority_.authorize(current,{path(record.request.destination),std::nullopt,
                record.destinationExisted ? Domain::FileAccess::Write : Domain::FileAccess::Create,!record.destinationExisted},context));
            take(Detail::ensureAuthorizedParentDirectories(output,context)); take(files_.replace(output,content,false,context));
        }
        entry->record.artifactPublished = true; entry->record.state = State::Completed; entry->record.error.reset();
        persist(entry->record,context); entry->changed.notify_all();
    }
    void capturedInputs(const std::shared_ptr<Entry>& entry, const Record& record,
        const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& context) {
        const auto restore = [&](const std::optional<std::string>& source, const char* leaf,
            const std::optional<std::string>& pngHash, const std::optional<std::string>& rgbaHash, std::optional<Pixels>& pixels) {
            if (!source) return;
            static_cast<void>(take(authority_.authorize(authority,{path(*source),std::nullopt,Domain::FileAccess::Read,false},context)));
            const auto content = take(readStored(record,leaf,MaximumImageBytes,context));
            if (!pngHash || hash(content,context) != *pngHash) reject(Domain::ErrorCodes::IntegrityFailure,"Captured image snapshot seal does not match.");
            auto restored = take(Detail::decodeProviderImage(content,context));
            if (restored.width != record.request.width || restored.height != record.request.height || !rgbaHash || hash(restored.rgba,context) != *rgbaHash)
                reject(Domain::ErrorCodes::IntegrityFailure,"Captured image snapshot pixel seal does not match.");
            pixels = std::move(restored);
        };
        restore(record.request.source,"source.png",record.sourcePngSha256,record.sourceRgbaSha256,entry->original);
        restore(record.request.mask,"mask.png",record.maskPngSha256,record.maskRgbaSha256,entry->mask);
    }
    void cancel(const std::shared_ptr<Entry>& entry, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) {
        std::lock_guard control{entry->control};
        Record record;
        {
            std::lock_guard lock{entry->mutex}; scope(entry->record,authority,false);
            auto current = take(authority_.authorityFor(authority.projectId(),context)); scope(entry->record,current,false);
            if (entry->record.artifactPublished || entry->record.state == State::Completed) return;
            if (entry->finished.load() && !entry->record.ownerReleased) {
                const auto alive = ownerAlive(entry->record);
                if (!alive || *alive) reject(Domain::ErrorCodes::Conflict,"The previous image-job owner is live or unconfirmed; local cancellation cannot control another owner.");
                entry->record.ownerReleased = true;
            }
            record = entry->record;
            entry->record.publicationSuppressed = true; entry->record.cancellationRequested = true;
            entry->record.state = State::Cancelled;
            entry->record.error = Domain::makeError(Domain::ErrorCodes::Cancelled,
                "Local publication was cancelled; active shared-provider execution is not interrupted.");
            persist(entry->record,context);
            entry->cancellation.request_stop();
        }
        entry->changed.notify_all();
        if (record.graphJson.empty() || record.remoteState != "queued") return;
        // ComfyUI deletes only the supplied pending ID. It cannot prove that a running task stopped.
        try {
            scope(record,authority,true); static_cast<void>(fresh(record,context)); sameProvider(record,context);
            const auto body = Json{{"delete",Json::array({record.promptId})}}.dump();
            const auto response = http(record.provider,"POST","/queue","application/json",bytes(body),MaximumJsonBytes,context);
            if (response.status == 200U) {
                std::lock_guard lock{entry->mutex}; entry->record.queueDeleteAccepted = true; persist(entry->record,context);
            }
        } catch (const Failure&) { /* The sealed local suppression remains authoritative. */ }
    }
    void resume(const std::shared_ptr<Entry>& entry, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) {
        std::lock_guard control{entry->control};
        std::lock_guard admission{entriesMutex_};
        if (stopped_.load()) reject(Domain::ErrorCodes::HostCapabilityUnavailable,"Image-provider owner has shut down.");
        if (std::count_if(entries_.begin(),entries_.end(),[](const auto& pair){return !pair.second->finished.load();}) >= MaximumActiveJobs)
            reject(Domain::ErrorCodes::LimitExceeded,"At most eight image jobs may be active in this Manager.");
        {
            std::lock_guard lock{entry->mutex}; auto& record = entry->record;
            scope(record,authority,true); auto current = fresh(record,context); sameProvider(record,context);
            if (!entry->finished.load()) reject(Domain::ErrorCodes::Conflict,"Image job is already owned by an active worker.");
            if (record.artifactPublished && record.state == State::Completed) return;
            if (!record.ownerReleased) {
                const auto alive = ownerAlive(record);
                if (!alive || *alive) reject(Domain::ErrorCodes::Conflict,"The previous image-job owner is live or cannot be proven stopped; reattachment is refused.");
            }
            if (record.graphJson.empty()) reject(Domain::ErrorCodes::Conflict,"No exact submitted workflow exists; resume cannot create or replay a generation.");
            static_cast<void>(destinationUnchanged(record,current,context,true));
            capturedInputs(entry,record,current,context);
            const auto observed = remote(record,context); entry->observed = observed;
            if (observed.state == "unknown") {
                // Exact-ID absence is an observation, not permission to repeat an ambiguous POST.
                return;
            }
            if (observed.state == "error") reject(Domain::ErrorCodes::HostCapabilityUnavailable,"The exact remote workflow failed; resume cannot replay it.");
            entry->cancellation = std::stop_source{};
            record.publicationSuppressed = false; record.cancellationRequested = false; record.recovered = true;
            record.state = observed.state == "running" ? State::Running : observed.state == "completed" ? State::Fetching : State::Queued;
            record.remoteState = observed.state; record.error.reset(); record.ownerReleased = false;
            record.ownerHostPid = ::GetCurrentProcessId(); record.ownerHostCreationTime = hostCreation();
            persist(record,context);
        }
        launch(entry,true);
    }
    void poll(const std::shared_ptr<Entry>& entry, const Domain::OperationContext& context) {
        for (;;) {
            check(context);
            Record record; { std::lock_guard lock{entry->mutex}; record = entry->record; }
            static_cast<void>(fresh(record,context)); sameProvider(record,context);
            auto observed = remote(record,context);
            if (observed.state == "error") {
                update(entry,context,[&](auto& r){r.remoteState = "error"; r.state = State::Failed; r.error = observed.error;}); return;
            }
            if (observed.state == "completed") { publish(entry,record,observed,context); return; }
            if (observed.state == "queued" || observed.state == "running") {
                update(entry,context,[&](auto& r){r.remoteState = observed.state; r.state = observed.state == "running" ? State::Running : State::Queued;});
            }
            std::unique_lock lock{entry->mutex}; entry->changed.wait_for(lock,std::chrono::milliseconds{250});
        }
    }
    void run(const std::shared_ptr<Entry>& entry, bool attach, std::stop_token cancellation) noexcept {
        Record initial; { std::lock_guard lock{entry->mutex}; initial = entry->record; }
        const auto context = internalContext(initial,cancellation,std::chrono::seconds{initial.request.timeoutSeconds});
        try {
            static_cast<void>(fresh(initial,context)); sameProvider(initial,context);
            if (!attach) {
                preflight(initial.provider,initial.request.source.has_value(),initial.request.mask.has_value(),context);
                std::optional<std::string> source,mask;
                if (initial.request.source) source = upload(initial,"source.png","source",context);
                if (initial.request.mask) mask = upload(initial,"mask.png","mask",context);
                const auto workflow = graph(initial,source,mask);
                const auto encoded = workflow.dump(); const auto digest = hash(bytes(encoded),context);
                update(entry,context,[&](auto& r){r.graphJson = encoded; r.graphSha256 = digest; r.state = State::Submitting; r.remoteState = "submission_unknown";});
                // The sealed intent precedes the only generation POST. A failed/ambiguous response is never retried.
                static_cast<void>(fresh(initial,context)); sameProvider(initial,context); check(context);
                const auto outgoing = Json{{"prompt_id",initial.promptId},{"client_id",initial.jobId},{"prompt",workflow}}.dump();
                const auto response = http(initial.provider,"POST","/prompt","application/json",bytes(outgoing),MaximumJsonBytes,context);
                if (response.status == 400U) {
                    const auto rejection = parse(std::string_view{reinterpret_cast<const char*>(response.body.data()),response.body.size()},MaximumJsonBytes,context);
                    if (!rejection.contains("error") || !rejection.contains("node_errors") || !rejection.at("node_errors").is_object())
                        reject(Domain::ErrorCodes::MalformedMessage,"Provider rejection is malformed; submission remains unknown.");
                    update(entry,context,[](auto& r){r.remoteState = "rejected"; r.state = State::Failed;
                        r.error = Domain::makeError(Domain::ErrorCodes::InvalidRequest,"The provider rejected the exact workflow with HTTP 400.");});
                } else {
                    const auto accepted = json(response,context);
                    if (text(accepted,"prompt_id",36U) != initial.promptId || !accepted.at("node_errors").is_object() || !accepted.at("node_errors").empty())
                        reject(Domain::ErrorCodes::IntegrityFailure,"Provider submission receipt disagrees with the exact saved prompt ID/workflow.");
                    update(entry,context,[](auto& r){r.submissionAcknowledged = true; r.remoteState = "queued"; r.state = State::Queued;});
                    poll(entry,context);
                }
            } else poll(entry,context);
        } catch (const Failure& failure) { failWorker(entry,failure.error); }
        catch (const Json::exception&) { failWorker(entry,Domain::makeError(Domain::ErrorCodes::MalformedMessage,"Provider response has an invalid typed shape.")); }
        catch (...) { failWorker(entry,Domain::makeError(Domain::ErrorCodes::InternalFailure,"Image-provider worker failed.")); }
        try {
            std::lock_guard lock{entry->mutex}; entry->record.ownerReleased = true;
            persist(entry->record,internalContext(entry->record));
        } catch (...) { /* Retain actual in-memory facts; a reconnect treats an unfinalized durable record as unknown. */ }
        entry->finished.store(true); entry->changed.notify_all();
    }
    void failWorker(const std::shared_ptr<Entry>& entry, Domain::Error error) noexcept {
        try {
            std::lock_guard lock{entry->mutex}; auto& r = entry->record;
            r.error = std::move(error); r.publicationSuppressed = true;
            if (r.cancellationRequested) r.state = State::Cancelled;
            else r.state = r.graphJson.empty() ? State::Failed : State::Unknown;
            persist(r,internalContext(r));
        } catch (...) {}
    }
    void launch(const std::shared_ptr<Entry>& entry, bool attach) {
        std::lock_guard worker{entry->workerMutex};
        if (entry->worker.joinable()) entry->worker.join();
        std::stop_token cancellation;
        { std::lock_guard lock{entry->mutex}; cancellation = entry->cancellation.get_token(); entry->finished.store(false); }
        try { entry->worker = std::jthread{[this,entry,attach,cancellation]{run(entry,attach,cancellation);}}; }
        catch (...) { entry->finished.store(true); failWorker(entry,Domain::makeError(Domain::ErrorCodes::InternalFailure,"Image-provider worker could not be started.")); throw; }
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
    std::shared_ptr<Contracts::IImageProviderHttpTransport> transport_;
    std::atomic<bool> stopped_{};
    std::mutex entriesMutex_;
    std::map<std::string,std::shared_ptr<Entry>> entries_;
};

WindowsImageProviderService::WindowsImageProviderService(Contracts::IWorkspaceAuthority& authority,
    Contracts::IAtomicFileStore& files, Contracts::IConfigurationStore& configuration,
    Contracts::IWorkspaceAuthority& storageAuthority, const Contracts::WorkspaceAuthority& storageScope,
    Domain::PathText jobsRoot, Contracts::IUuidGenerator& uuids, Contracts::IClock& clock,
    Contracts::IHasher& hasher, std::shared_ptr<Contracts::IImageProviderHttpTransport> transport)
    : implementation_{std::make_unique<Impl>(authority,files,configuration,storageAuthority,storageScope,
        std::move(jobsRoot),uuids,clock,hasher,std::move(transport))} {}
WindowsImageProviderService::~WindowsImageProviderService() noexcept = default;
Domain::Result<std::string> WindowsImageProviderService::execute(std::string_view name,std::string_view args,
    const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context) noexcept {
    return implementation_->execute(name,args,authority,context);
}
void WindowsImageProviderService::shutdown() noexcept { implementation_->shutdown(); }
} // namespace ForgeConductor::NativeTools::Windows
