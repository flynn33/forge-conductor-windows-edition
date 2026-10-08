#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/NativeTools/Windows/WindowsImageProviderService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsImageProviderHttpTransport.h"
#include "NativeTools/Windows/ImageProviderCodec.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <Windows.h>
#include <wincrypt.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Manual, opt-in qualification. This executable is deliberately not a CTest.
// It preserves its new private home, never starts the provider, and never interrupts it.
namespace {
namespace Domain = ForgeConductor::Domain;
namespace Contracts = ForgeConductor::Contracts;
namespace Windows = ForgeConductor::Infrastructure::Windows;
namespace Native = ForgeConductor::NativeTools::Windows;
namespace Codec = Native::Detail;
using Json = nlohmann::json;
using namespace std::chrono_literals;
using Deadline = std::chrono::steady_clock::time_point;

struct Failure final : std::runtime_error {
    std::string code;
    Failure(std::string c, std::string message) : std::runtime_error{std::move(message)}, code{std::move(c)} {}
};
void require(bool value, const char* message) { if (!value) throw Failure{"smoke_assertion", message}; }
template<class T> T take(Domain::Result<T> result) {
    if (!result) throw Failure{result.error().code, result.error().message};
    return std::move(result).value();
}
template<class T> T parse(std::string_view value) { return take(T::parse(value)); }
std::string utf8(std::wstring_view value) { return take(Windows::Detail::strictUtf16ToUtf8(value)); }
std::filesystem::path nativePath(std::string_view value) { return take(Windows::Detail::strictUtf8ToUtf16(value)); }
Domain::PathText pathText(const std::filesystem::path& value) { return take(Domain::PathText::create(utf8(value.native()))); }
std::span<const std::byte> bytes(std::string_view value) { return std::as_bytes(std::span{value.data(), value.size()}); }
std::string hash(std::span<const std::byte> value) { Windows::BCryptSha256Hasher hasher; return take(hasher.sha256(value)).value(); }
Domain::OperationContext context(Deadline deadline) {
    return {parse<Domain::OperationId>("30000000-0000-4000-8000-000000000003"), deadline, {},
        parse<Domain::CorrelationId>("image-provider-manual-smoke")};
}
std::vector<std::byte> read(const std::filesystem::path& path) {
    require(std::filesystem::file_size(path) <= 16U * 1024U * 1024U, "Evidence file exceeds the smoke read bound.");
    std::ifstream stream{path, std::ios::binary}; require(static_cast<bool>(stream), "Reading evidence failed.");
    std::vector<char> data{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    require(!stream.bad(), "Evidence read was incomplete.");
    return {reinterpret_cast<const std::byte*>(data.data()), reinterpret_cast<const std::byte*>(data.data() + data.size())};
}
void write(const std::filesystem::path& path, std::span<const std::byte> value) {
    require(!std::filesystem::exists(path), "The smoke helper will not replace existing evidence.");
    std::ofstream stream{path, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(value.size()));
    stream.flush(); require(static_cast<bool>(stream), "Writing smoke evidence failed.");
}
Json metadata(Json value) {
    if (value.contains("image_base64")) {
        value["smoke_preview_base64_bytes"] = value.at("image_base64").get_ref<const std::string&>().size();
        value.erase("image_base64"); value["smoke_preview_base64_omitted"] = true;
    }
    return value;
}
struct Options final { std::filesystem::path home; Domain::ImageProviderConfig provider; unsigned timeout{180U}; };
Options options(int argc, wchar_t** argv) {
    Options result; std::map<std::wstring, std::wstring> values;
    for (int i = 1; i < argc; i += 2) {
        require(i + 1 < argc, "Every option requires a value.");
        const std::wstring key{argv[i]};
        require(key == L"--home" || key == L"--endpoint" || key == L"--checkpoint" || key == L"--profile" || key == L"--timeout-sec", "Unknown smoke option.");
        require(values.emplace(key, argv[i + 1]).second, "Duplicate smoke option.");
    }
    for (const auto* key : {L"--home", L"--endpoint", L"--checkpoint", L"--profile"})
        require(values.contains(key) && !values.at(key).empty(), "Required explicit smoke option is missing.");
    result.home = std::filesystem::path{values.at(L"--home")};
    require(result.home.is_absolute() && !std::filesystem::exists(result.home), "--home must be an absolute, previously nonexistent private directory.");
    require(std::filesystem::is_directory(result.home.parent_path()), "The new private home's parent must already exist.");
    result.provider = {true, utf8(values.at(L"--endpoint")), utf8(values.at(L"--profile")), utf8(values.at(L"--checkpoint"))};
    take(Domain::validateImageProviderConfig(result.provider));
    if (values.contains(L"--timeout-sec")) {
        const auto text = utf8(values.at(L"--timeout-sec"));
        const auto converted = std::from_chars(text.data(), text.data() + text.size(), result.timeout);
        require(converted.ec == std::errc{} && converted.ptr == text.data() + text.size() && result.timeout >= 1U && result.timeout <= 600U,
            "--timeout-sec must be an integer from 1 through 600.");
    }
    return result;
}
class Configuration final : public Contracts::IConfigurationStore {
public:
    explicit Configuration(Domain::ImageProviderConfig provider) : config_{Domain::defaultAppConfig()} { config_.imageProvider = std::move(provider); }
    Domain::Result<Domain::AppConfig> load(const Domain::OperationContext&) noexcept override { return Domain::Result<Domain::AppConfig>::success(config_); }
    Domain::Result<Domain::AppConfig> reload(const Domain::OperationContext& c) noexcept override { return load(c); }
    Domain::Result<Domain::AppConfig> update(const Domain::AppConfigPatch&, const Domain::OperationContext&) noexcept override {
        return Domain::Result<Domain::AppConfig>::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized, "The private smoke configuration is immutable."));
    }
    void shutdown() noexcept override {}
private:
    Domain::AppConfig config_;
};

class TracedTransport final : public Contracts::IImageProviderHttpTransport {
public:
    struct Owned final { std::string graphHash; bool runningObserved{}; unsigned promptAttempts{}; bool actualAccepted{}; };
    explicit TracedTransport(Domain::ImageProviderConfig provider, const std::filesystem::path& trace)
        : provider_{std::move(provider)}, trace_{trace, std::ios::binary} { require(static_cast<bool>(trace_), "Opening private HTTP trace failed."); }
    std::atomic<bool> injectLostAcknowledgement{};
    std::atomic<bool> injected{};
    std::atomic<bool> traceFailed{};
    unsigned promptAttempts(std::string_view id) { std::lock_guard lock{mutex_}; return owned_.at(std::string{id}).promptAttempts; }
    bool running(std::string_view id) { std::lock_guard lock{mutex_}; const auto found = owned_.find(std::string{id}); return found != owned_.end() && found->second.runningObserved; }
    bool accepted(std::string_view id) { std::lock_guard lock{mutex_}; const auto found = owned_.find(std::string{id}); return found != owned_.end() && found->second.actualAccepted; }
    std::string graphHash(std::string_view id) { std::lock_guard lock{mutex_}; return owned_.at(std::string{id}).graphHash; }
    Contracts::ImageProviderHttpResponse get(std::string route, Deadline deadline) {
        return take(request(provider_, "GET", route, {}, {}, 512U * 1024U, context(deadline)));
    }
    static Json json(const Contracts::ImageProviderHttpResponse& response) {
        require(response.status == 200U, "Owned observation returned a non-200 HTTP status.");
        return Json::parse(reinterpret_cast<const char*>(response.body.data()), reinterpret_cast<const char*>(response.body.data() + response.body.size()));
    }
    Domain::Result<Contracts::ImageProviderHttpResponse> request(const Domain::ImageProviderConfig& provider, std::string_view method,
        std::string_view route, std::string_view type, std::span<const std::byte> body, std::size_t maximum,
        const Domain::OperationContext& c) noexcept override {
        try {
            require(provider == provider_, "The service tried to use a different provider configuration.");
            require((method == "GET" && (route.starts_with("/object_info/") || route.starts_with("/history/") || route.starts_with("/view?") || route == "/queue")) ||
                (method == "POST" && (route == "/prompt" || route == "/upload/image" || route == "/queue")), "An unexpected provider effect was refused.");
            std::string id;
            if (method == "POST" && route == "/prompt") {
                const auto input = Json::parse(reinterpret_cast<const char*>(body.data()), reinterpret_cast<const char*>(body.data() + body.size()));
                id = input.at("prompt_id").get<std::string>(); take(Domain::Uuid::parse(id));
                std::lock_guard lock{mutex_}; auto& owned = owned_[id]; ++owned.promptAttempts;
                require(owned.promptAttempts == 1U && owned_.size() <= 5U, "Generation replay or unbounded smoke job admission was refused.");
                owned.graphHash = hash(bytes(input.at("prompt").dump()));
            }
            if (method == "POST" && route == "/queue") {
                const auto deletion = Json::parse(reinterpret_cast<const char*>(body.data()), reinterpret_cast<const char*>(body.data() + body.size()));
                require(deletion.size() == 1U && deletion.at("delete").is_array() && deletion.at("delete").size() == 1U, "A global queue modification was refused.");
                std::lock_guard lock{mutex_}; require(owned_.contains(deletion.at("delete").at(0).get<std::string>()), "Deletion of a foreign prompt was refused.");
            }
            auto result = native_.request(provider, method, route, type, body, maximum, c);
            Json event{{"method", method}, {"route", route}, {"request_content_type", type}, {"request_bytes", body.size()},
                {"request_sha256", hash(body)}, {"response_limit_bytes", maximum}, {"injected_delivery_fault", false}};
            bool lose{};
            if (result) {
                const auto& response = result.value(); event["actual_http_status"] = response.status;
                event["response_content_type"] = response.contentType; event["response_bytes"] = response.body.size(); event["response_sha256"] = hash(response.body);
                auto value = Json::parse(reinterpret_cast<const char*>(response.body.data()), reinterpret_cast<const char*>(response.body.data() + response.body.size()), nullptr, false);
                if (response.status >= 400U && response.body.size() <= 8192U && !value.is_discarded()) event["actual_error_json"] = value;
                if (route == "/prompt" && method == "POST" && response.status == 200U && value.is_object() && value.contains("prompt_id") &&
                    value.at("prompt_id").is_string() && value.at("prompt_id") == id &&
                    value.contains("node_errors") && value.at("node_errors").is_object() && value.at("node_errors").empty()) {
                    std::lock_guard lock{mutex_}; owned_.at(id).actualAccepted = true;
                    lose = injectLostAcknowledgement.exchange(false); if (lose) { injected.store(true); event["injected_delivery_fault"] = true; }
                }
                if (route == "/queue" && method == "GET" && value.is_object() && value.contains("queue_running") && value.at("queue_running").is_array()) {
                    std::lock_guard lock{mutex_};
                    for (const auto& row : value.at("queue_running")) if (row.is_array() && row.size() >= 3U && row.at(1).is_string()) {
                        const auto found = owned_.find(row.at(1).get<std::string>());
                        if (found != owned_.end() && hash(bytes(row.at(2).dump())) == found->second.graphHash) found->second.runningObserved = true;
                    }
                }
            } else event["native_transport_error"] = Json{{"code", result.error().code}, {"message", result.error().message}};
            { std::lock_guard lock{mutex_}; require(++traceCount_ <= 8192U, "The bounded smoke HTTP trace is full.");
                event["sequence"] = traceCount_; trace_ << event.dump() << '\n'; trace_.flush(); require(static_cast<bool>(trace_), "HTTP trace persistence failed."); }
            if (lose) return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(Domain::ErrorCodes::TransportClosed,
                "SMOKE INJECTION: actual HTTP 200 acknowledgement was observed, then deliberately withheld from the service."));
            return result;
        } catch (const std::exception& e) {
            traceFailed.store(true); return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, e.what()));
        }
    }
private:
    Domain::ImageProviderConfig provider_;
    Native::WindowsImageProviderHttpTransport native_;
    std::mutex mutex_;
    std::map<std::string, Owned> owned_;
    std::ofstream trace_;
    std::size_t traceCount_{};
};

class Fixture final {
public:
    Options options;
    Windows::WindowsAtomicFileStore files;
    Windows::WindowsUuidGenerator uuids;
    Windows::SystemClock clock;
    Windows::BCryptSha256Hasher hasher;
    Configuration configuration;
    std::shared_ptr<TracedTransport> transport;
    std::unique_ptr<Windows::WindowsWorkspaceAuthority> issuer, storageIssuer;
    std::unique_ptr<Contracts::WorkspaceAuthority> authority, storage;
    std::unique_ptr<Native::WindowsImageProviderService> service;
    explicit Fixture(Options opts) : options{std::move(opts)}, configuration{options.provider} {
        require(std::filesystem::create_directory(options.home), "Creating the new private home failed or it already exists.");
        options.home = std::filesystem::canonical(options.home);
        require(std::filesystem::create_directory(options.home / "workspace") && std::filesystem::create_directory(options.home / "private"), "Creating private smoke roots failed.");
        transport = std::make_shared<TracedTransport>(options.provider, options.home / "http-trace.jsonl");
        using Access = Domain::FileAccess;
        issuer = std::make_unique<Windows::WindowsWorkspaceAuthority>(std::vector<Windows::WindowsWorkspaceAuthorityPolicy>{{
            parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"), project(), parse<Domain::ClientId>("forge-conductor-manager"),
            {pathText(options.home / "workspace")}, Access::Write, {Access::Read, Access::Write, Access::Create}, {}, false, 1U}});
        storageIssuer = std::make_unique<Windows::WindowsWorkspaceAuthority>(std::vector<Windows::WindowsWorkspaceAuthorityPolicy>{{
            parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000002"), project(), parse<Domain::ClientId>("image-provider-smoke-storage"),
            {pathText(options.home / "private")}, Access::Write, {Access::Read, Access::Write, Access::Create}, {}, false, 1U}});
        authority = std::make_unique<Contracts::WorkspaceAuthority>(take(issuer->authorityFor(project(), context(deadline()))));
        storage = std::make_unique<Contracts::WorkspaceAuthority>(take(storageIssuer->authorityFor(project(), context(deadline())))); reopen();
    }
    static Domain::ProjectId project() { return parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001"); }
    Deadline deadline() const { return std::chrono::steady_clock::now() + std::chrono::seconds{options.timeout}; }
    void reopen() {
        service.reset(); service = std::make_unique<Native::WindowsImageProviderService>(*issuer, files, configuration, *storageIssuer, *storage,
            pathText(options.home / "private"), uuids, clock, hasher, transport);
    }
    Json execute(std::string_view name, const Json& args, Deadline end) { return Json::parse(take(service->execute(name, args.dump(), *authority, context(end)))); }
    Json args(std::string_view file, unsigned dimension = 128U, unsigned steps = 2U) const {
        return {{"prompt", "A small ceramic red teapot on a plain cream studio backdrop, soft light"},
            {"negative_prompt", "text, watermark"}, {"path", pathText(options.home / "workspace" / file).value()},
            {"seed", 81723U}, {"width", dimension}, {"height", dimension}, {"steps", steps}, {"cfg", 4.0},
            {"timeout_sec", options.timeout}, {"preview_max_dimension", 256U}};
    }
    Json wait(const Json& job, Deadline end) {
        for (;;) {
            require(std::chrono::steady_clock::now() < end, "The smoke job observation deadline expired.");
            auto status = execute("image_job_status", {{"job_id", job.at("job_id")}, {"wait_sec", 1U}}, end);
            if (status.at("done") == true) return status;
        }
    }
    Json drain(const Json& job, Deadline end) {
        const auto id = job.at("prompt_id").get<std::string>();
        require(transport->accepted(id), "No actual exact-ID HTTP 200 acceptance was observed; no next job may start.");
        while (std::chrono::steady_clock::now() < end) {
            const auto history = TracedTransport::json(transport->get("/history/" + id, (std::min)(end, std::chrono::steady_clock::now() + 10s)));
            if (history.contains(id)) {
                const auto& item = history.at(id); const auto& prompt = item.at("prompt");
                require(prompt.is_array() && prompt.size() >= 3U && prompt.at(1) == id && hash(bytes(prompt.at(2).dump())) == transport->graphHash(id),
                    "Exact-ID history does not match the submitted native workflow.");
                const auto& status = item.at("status");
                if (status.value("completed", false) || status.value("status_str", "") == "error")
                    return {{"prompt_id", id}, {"actual_history_status", status}, {"workflow_sha256", transport->graphHash(id)}};
            }
            std::this_thread::sleep_for(100ms);
        }
        throw Failure{"remote_terminal_unverified", "The exact remote history did not become terminal; subsequent jobs were suppressed."};
    }
    Json manifest() const {
        Json result = Json::object(); std::size_t count{};
        for (const auto* leaf : {"workspace", "private"}) for (const auto& entry : std::filesystem::recursive_directory_iterator{options.home / leaf}) {
            require(++count <= 512U && !entry.is_symlink(), "The private evidence tree is unexpectedly large or linked.");
            const auto name = utf8(std::filesystem::relative(entry.path(), options.home).native());
            result[name] = {{"directory", entry.is_directory()}, {"modified", entry.last_write_time().time_since_epoch().count()}};
            if (entry.is_regular_file()) { const auto content = read(entry.path()); result[name]["bytes"] = content.size(); result[name]["sha256"] = hash(content); }
        }
        return result;
    }
};

std::vector<std::byte> base64(const std::string& input) {
    require(input.size() <= 512U * 1024U, "Preview exceeds its 512 KiB encoded bound."); DWORD count{};
    require(::CryptStringToBinaryA(input.c_str(), static_cast<DWORD>(input.size()), CRYPT_STRING_BASE64, nullptr, &count, nullptr, nullptr) != FALSE, "Preview base64 is invalid.");
    std::vector<std::byte> result(count);
    require(::CryptStringToBinaryA(input.c_str(), static_cast<DWORD>(input.size()), CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(result.data()), &count, nullptr, nullptr) != FALSE && count == result.size(), "Preview base64 was incomplete.");
    return result;
}
Json artifact(const Json& result, Deadline end) {
    require(result.at("state") == "completed" && result.at("done") == true && result.at("artifact_published") == true, "Actual native generation/edit did not publish a completed artifact.");
    const auto content = read(nativePath(result.at("path").get<std::string>()));
    const auto decoded = take(Codec::decodeProviderImage(content, context(end)));
    require(hash(content) == result.at("artifact_sha256").get<std::string>() && content.size() == result.at("bytes_written") && hash(decoded.rgba) == result.at("decoded_rgba8_sha256").get<std::string>(), "Native artifact metadata does not match actual PNG bytes/pixels.");
    require(decoded.width == result.at("width") && decoded.height == result.at("height"), "Actual output dimensions disagree with native metadata.");
    const auto preview = base64(result.at("image_base64").get<std::string>());
    const auto pixels = take(Codec::decodeProviderImage(preview, context(end)));
    require(result.at("image_mime_type") == "image/png" && hash(preview) == result.at("preview_png_sha256").get<std::string>() && pixels.width == result.at("preview_width") && pixels.height == result.at("preview_height"), "Actual preview MIME/hash/dimensions disagree with native metadata.");
    return {{"png_bytes", content.size()}, {"png_sha256", hash(content)}, {"rgba8_sha256", hash(decoded.rgba)},
        {"width", decoded.width}, {"height", decoded.height}, {"preview_png_bytes", preview.size()}, {"preview_png_sha256", hash(preview)}};
}
void sourceImages(Fixture& f) {
    Codec::ImageProviderPixels source{128U, 128U, std::vector<std::byte>(128U * 128U * 4U)}, mask = source;
    for (unsigned y{}; y < 128U; ++y) for (unsigned x{}; x < 128U; ++x) {
        const auto offset = (y * 128U + x) * 4U;
        source.rgba[offset] = static_cast<std::byte>((x * 3U + 17U) & 255U); source.rgba[offset + 1U] = static_cast<std::byte>((y * 5U + 31U) & 255U);
        source.rgba[offset + 2U] = std::byte{181}; source.rgba[offset + 3U] = static_cast<std::byte>((x + y) & 255U);
        mask.rgba[offset] = static_cast<std::byte>(x < 64U ? 0U : 255U); mask.rgba[offset + 1U] = std::byte{93};
        mask.rgba[offset + 2U] = std::byte{17}; mask.rgba[offset + 3U] = std::byte{255};
    }
    write(f.options.home / "workspace" / "source.png", take(Codec::encodeProviderImage(source, context(f.deadline()))));
    write(f.options.home / "workspace" / "mask.png", take(Codec::encodeProviderImage(mask, context(f.deadline()))));
}
Json maskedPixels(Fixture& f, const Json& result) {
    const auto original = take(Codec::decodeProviderImage(read(f.options.home / "workspace" / "source.png"), context(f.deadline())));
    const auto generated = take(Codec::decodeProviderImage(read(nativePath(result.at("path").get<std::string>())), context(f.deadline())));
    std::size_t unchanged{}, changedInside{};
    for (unsigned y{}; y < 128U; ++y) for (unsigned x{}; x < 128U; ++x) {
        const auto offset = (y * 128U + x) * 4U;
        const auto same = std::equal(original.rgba.begin() + offset, original.rgba.begin() + offset + 4U, generated.rgba.begin() + offset);
        if (x < 64U) { require(same, "A red-zero mask pixel lost exact original RGBA values."); ++unchanged; }
        else if (!same) ++changedInside;
    }
    return {{"outside_mask_rgba_exact_pixels", unchanged}, {"inside_mask_pixels_changed", changedInside},
        {"instruction_following_verified", false}, {"generation_quality_verified", false}};
}
Json run(Fixture& f) {
    Json report{{"schema_version", 1U}, {"qualification_scope", "direct native service and real WinHTTP with fixture-issued private scopes"},
        {"manager_ipc_verified", false}, {"lm_studio_qwen_verified", false}, {"generation_quality_verified", false},
        {"instruction_following_verified", false}, {"source_identity_bound", false},
        {"resource_bounds", {{"maximum_owned_prompt_posts", 5U}, {"maximum_dimension", 256U}, {"maximum_requested_steps", 6U},
            {"maximum_http_trace_entries", 8192U}, {"maximum_trace_error_json_bytes", 8192U},
            {"timeout_scope", "each local job and each separate admission/observation/history-drain phase; not a total helper deadline"}}},
        {"home", pathText(f.options.home).value()}, {"endpoint", f.options.provider.endpoint}, {"checkpoint", f.options.provider.checkpoint},
        {"profile", f.options.provider.profile}, {"timeout_sec", f.options.timeout}, {"cases", Json::array()}, {"source_hashes", nullptr}};
    auto& cases = report["cases"];
    const auto record = [&](std::string name, Json value) { value["name"] = std::move(name); cases.push_back(std::move(value)); };
    std::string activeCase{"provider_status"}; Json lastNative; bool remotePending{};
    try {
        const auto status = f.execute("image_provider_status", Json::object(), f.deadline());
        lastNative = status;
        record("provider_status", {{"outcome", status.at("available") == true ? "pass" : "fail"}, {"native", status}});
        require(status.at("configured") == true && status.at("available") == true, "Actual provider inventory/checkpoint qualification failed.");
        activeCase = "running_cancellation";
        auto trial = f.execute("image_generate", f.args("cancellation-trial.png", 256U, 6U), f.deadline()); lastNative = trial; remotePending = true;
        const auto trialId = trial.at("prompt_id").get<std::string>(); const auto observeEnd = f.deadline(); bool cancelled{}, runningObserved{};
        while (std::chrono::steady_clock::now() < observeEnd) {
            if (f.transport->running(trialId)) {
                runningObserved = true;
                trial = f.execute("image_job_cancel", {{"job_id", trial.at("job_id")}}, observeEnd); lastNative = trial;
                cancelled = trial.at("cancellation_requested") == true && trial.at("publication_suppressed") == true; break;
            }
            auto current = f.execute("image_job_status", {{"job_id", trial.at("job_id")}}, observeEnd);
            if (current.at("done") == true) { trial = std::move(current); break; }
            std::this_thread::sleep_for(20ms);
        }
        trial = f.wait(trial, f.deadline()); lastNative = trial;
        Json cancelCase{{"outcome", cancelled ? "pass" : "unverified"}, {"actual_running_observed", runningObserved}, {"native", metadata(trial)}};
        if (cancelled) require(trial.at("cancellation_requested") == true && trial.at("publication_suppressed") == true && trial.at("artifact_published") == false &&
            !std::filesystem::exists(f.options.home / "workspace" / "cancellation-trial.png"), "Running cancellation did not suppress local publication.");
        else cancelCase["reason"] = runningObserved ? "The job completed before the cancel call could suppress publication; running cancellation was not qualified." :
            "No exact-ID queue_running response was observed before the local worker finished; running cancellation was not qualified.";
        cancelCase["remote_terminal"] = f.drain(trial, f.deadline()); remotePending = false; record("running_cancellation", std::move(cancelCase));
        sourceImages(f);
        for (const auto* name : {"generation", "unmasked_edit", "masked_edit"}) {
            activeCase = name;
            auto args = f.args(std::string{name} + ".png", 128U, 4U);
            if (std::string_view{name} != "generation") { args["source_path"] = pathText(f.options.home / "workspace" / "source.png").value(); args["denoise"] = 0.65; }
            if (std::string_view{name} == "masked_edit") args["mask_path"] = pathText(f.options.home / "workspace" / "mask.png").value();
            auto admitted = f.execute(std::string_view{name} == "generation" ? "image_generate" : "image_edit", args, f.deadline()); lastNative = admitted; remotePending = true;
            auto final = f.wait(admitted, f.deadline()); lastNative = final; auto terminal = f.drain(final, f.deadline()); remotePending = false;
            Json evidence{{"outcome", "pass"}, {"native", metadata(final)}, {"remote_terminal", terminal}, {"artifact", artifact(final, f.deadline())}};
            if (std::string_view{name} == "masked_edit") evidence["mask_pixels"] = maskedPixels(f, final);
            require(f.transport->promptAttempts(final.at("prompt_id").get<std::string>()) == 1U, "A qualified case attempted generation replay."); record(name, std::move(evidence));
        }
        activeCase = "controlled_lost_acknowledgement_recovery"; f.transport->injectLostAcknowledgement.store(true);
        auto lost = f.execute("image_generate", f.args("lost-ack.png", 128U, 2U), f.deadline());
        lastNative = lost; remotePending = true;
        const auto unknown = f.wait(lost, f.deadline()); lastNative = unknown; auto terminal = f.drain(unknown, f.deadline()); remotePending = false;
        require(f.transport->injected.load() && unknown.at("state") == "unknown" && unknown.at("submission_acknowledged") == false && unknown.at("artifact_published") == false,
            "The controlled actual-200 delivery fault did not remain an unpublished ambiguous job.");
        f.reopen(); const auto before = f.manifest();
        const auto observed = f.execute("image_job_status", {{"job_id", lost.at("job_id")}}, f.deadline());
        require(f.manifest() == before && !std::filesystem::exists(f.options.home / "workspace" / "lost-ack.png") && observed.at("artifact_published") == false,
            "Read-only recovered status changed private receipts or published an artifact.");
        const auto resumed = f.execute("image_job_resume", {{"job_id", lost.at("job_id")}}, f.deadline());
        const auto completed = f.wait(resumed, f.deadline()); lastNative = completed;
        require(completed.at("prompt_id") == lost.at("prompt_id") && f.transport->promptAttempts(lost.at("prompt_id").get<std::string>()) == 1U,
            "Explicit resume changed the exact prompt ID or attempted a duplicate generation POST.");
        record("controlled_lost_acknowledgement_recovery", {{"outcome", "pass"}, {"fault_kind", "injected delivery fault after actual matching HTTP 200; not a real network failure"},
            {"ambiguous_native", metadata(unknown)}, {"read_only_native", metadata(observed)}, {"private_files_unchanged_by_status", true},
            {"remote_terminal", terminal}, {"native", metadata(completed)}, {"artifact", artifact(completed, f.deadline())}, {"generation_post_attempts", 1U}});
        require(!f.transport->traceFailed.load(), "HTTP trace or a safety guard failed.");
        report["outcome"] = cancelled ? "pass" : "partial";
    } catch (const Failure& e) { report["outcome"] = "fail"; report["error"] = {{"code", e.code}, {"message", e.what()}}; }
    catch (const std::exception& e) { report["outcome"] = "fail"; report["error"] = {{"code", "smoke_exception"}, {"message", e.what()}}; }
    if (report.at("outcome") == "fail") {
        report["failed_case"] = activeCase; report["last_native"] = metadata(lastNative);
        report["remote_execution_terminal_unverified"] = remotePending;
        const auto already = std::any_of(cases.begin(), cases.end(), [&](const auto& item) { return item.at("name") == activeCase; });
        if (!already) record(activeCase, {{"outcome", "fail"}, {"error", report.at("error")}, {"last_native", metadata(lastNative)}});
    }
    for (const auto* name : {"provider_status", "running_cancellation", "generation", "unmasked_edit", "masked_edit", "controlled_lost_acknowledgement_recovery"})
        if (std::none_of(cases.begin(), cases.end(), [&](const auto& item) { return item.at("name") == name; }))
            record(name, {{"outcome", "skipped"}, {"reason", "An earlier observed failure stopped subsequent jobs."}});
    f.service->shutdown(); report["preserved_files"] = f.manifest(); report["http_trace_path"] = pathText(f.options.home / "http-trace.jsonl").value();
    report["finished_utc_milliseconds"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return report;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        Fixture fixture{options(argc, argv)}; const auto report = run(fixture); const auto encoded = report.dump(2);
        write(fixture.options.home / "smoke-report.json", bytes(encoded)); std::cout << encoded << '\n';
        return report.at("outcome") == "pass" ? 0 : report.at("outcome") == "partial" ? 2 : 1;
    } catch (const std::exception& e) {
        std::cerr << Json{{"outcome", "fail"}, {"error", e.what()}, {"usage", "--home ABS_NEW_PRIVATE_HOME --endpoint EXPLICIT_LOOPBACK_URL --checkpoint BASENAME.safetensors --profile sd1 [--timeout-sec 1..600]"}}.dump() << '\n';
        return 1;
    }
}
