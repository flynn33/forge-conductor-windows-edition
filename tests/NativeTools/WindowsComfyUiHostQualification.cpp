#include <winsock2.h>
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAlphaManagerProfile.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioConversationReader.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioChatControl.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeClient.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/NativeTools/Windows/WindowsComfyUiBackend.h"
#include "ForgeConductor/NativeTools/Windows/WindowsComfyUiService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsDesktopArtifactService.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "NativeTools/Windows/ComfyUiNativeSupport.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cctype>
#include <array>
#include <filesystem>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Opt-in host exercise, deliberately excluded from ctest and the default build.
// It reads real saved LM Studio evidence and cannot fabricate preview delivery
// or operator approval. Explicit native UI actions send only qualification
// requests through the existing chat-control path. Configuration is in memory.
namespace {
namespace FC = ForgeConductor;
namespace IW = FC::Infrastructure::Windows;
namespace NW = FC::NativeTools::Windows;
namespace CD = NW::ComfyDetail;
namespace Fs = std::filesystem;
using Json = nlohmann::json;

template<class T> T take(FC::Domain::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
template<class T> T identifier(std::string_view value) { return take(T::parse(value)); }
std::string utf8(const Fs::path& path) { return take(IW::Detail::strictUtf16ToUtf8(path.native())); }
FC::Domain::PathText pathText(const Fs::path& path) { return take(FC::Domain::PathText::create(utf8(path))); }
Fs::path native(std::string_view value) { return Fs::path{take(IW::Detail::strictUtf8ToUtf16(value))}; }
Json readJson(const Fs::path& path, std::uintmax_t maximum = 16U * 1024U * 1024U) {
    if (!Fs::is_regular_file(path) || Fs::file_size(path) > maximum)
        throw std::runtime_error("Qualification JSON is missing or exceeds the " + std::to_string(maximum) + " byte read bound: " + utf8(path));
    std::ifstream stream{path, std::ios::binary};
    if (!stream) throw std::runtime_error("Cannot open qualification JSON: " + utf8(path));
    return Json::parse(stream);
}
void writeJson(const Fs::path& path, const Json& value) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    const auto text = value.dump(2);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) throw std::runtime_error("Cannot save qualification evidence: " + utf8(path));
}
FC::Domain::ProjectId project() {
    return identifier<FC::Domain::ProjectId>("749987b6-2664-4d2f-a7b5-c1c82f9cb5e8");
}

class Configuration final : public FC::Contracts::IConfigurationStore {
public:
    FC::Domain::AppConfig value;
    FC::Domain::Result<FC::Domain::AppConfig> load(const FC::Domain::OperationContext&) noexcept override {
        return FC::Domain::Result<FC::Domain::AppConfig>::success(value);
    }
    FC::Domain::Result<FC::Domain::AppConfig> reload(const FC::Domain::OperationContext& context) noexcept override { return load(context); }
    FC::Domain::Result<FC::Domain::AppConfig> update(const FC::Domain::AppConfigPatch&, const FC::Domain::OperationContext&) noexcept override {
        return FC::Domain::Result<FC::Domain::AppConfig>::failure(FC::Domain::makeError(
            FC::Domain::ErrorCodes::Unauthorized, "The host qualification configuration is read only."));
    }
    void shutdown() noexcept override {}
};

struct Arguments final {
    std::string action, kind;
    std::map<std::string, std::string> options;
    std::string option(std::string_view key, std::string fallback = {}) const {
        const auto found = options.find(std::string{key});
        return found == options.end() ? std::move(fallback) : found->second;
    }
    unsigned number(std::string_view key, unsigned fallback, unsigned maximum) const {
        const auto text = option(key);
        if (text.empty()) return fallback;
        std::size_t used{}; const auto value = std::stoul(text, &used);
        if (used != text.size() || value == 0U || value > maximum)
            throw std::runtime_error("Invalid numeric option --" + std::string{key});
        return static_cast<unsigned>(value);
    }
};
Arguments arguments(int count, wchar_t** values) {
    if (count < 2) throw std::runtime_error(
        "Usage: ComfyUiHostQualification status|start|stop|frontend-serialization|dependency-identity|view-playback|desktop-comfy-input|preview image|t2v|i2v|job-status|resume|final|prepare "
        "--output <absolute isolated directory> [--job UUID] [--plan UUID] [--input image] [--prompt text] "
        "[--workflow JSON] [--arguments JSON] [--seconds 1..30] [--draft-seconds 1..30] [--timeout 1..7200]. "
        "Read-only dependency identity: dependency-identity --managed-runtime <output/forge-home/comfy-jobs> --workflow <workflows JSON>. "
        "Native UI: lmstudio-observe|lmstudio-activate|lmstudio-idle|lmstudio-pause|lmstudio-send|lmstudio-clear-owned-draft|lmstudio-owned-draft-status --message FILE --expected-conversation ID [--new-chat true|false]. "
        "Controlled playback: view-playback --job UUID; requires an actual sealed published video receipt and exact idle owned provider. "
        "Video link: lmstudio-video-link --expected-conversation ID --job UUID [--observe true|false] [--arguments observed-scroll.json]; requires an actual selected saved assistant link and visible native Hyperlink. "
        "Optional scroll arguments contain exactly window_id, pid, x, y, delta; one existing guarded desktop_scroll replaces the viewport button click. "
        "Observed renderer trigger: --trigger-arguments observed-trigger.json contains exactly window_id, pid, x, y, capture_path, capture_sha256; clicks once only after a pixel-identical fresh capture, records the resulting window, and returns before browser input. "
        "Observed renderer popover: --open-popover true|false selects only its unique visible Open in browser button beside the exact saved URL, and verifies the actual postclick foreground address. "
        "Native tool delivery: --browser-open true|false uses existing browser_open with only the sealed saved video URL, excludes link/viewport input, and observes the actual foreground address separately from launch acceptance. "
        "Private Manager: private-manager-status|private-manager-drain --arguments JSON requires exactly home, executable, pid, creation_filetime, sha256, version; home must be this output's existing forge-home. Drain rechecks idle native work and waits for the exact process's normal exit without stopping ComfyUI.");
    Arguments result; result.action = take(IW::Detail::strictUtf16ToUtf8(values[1]));
    for (int index = 2; index < count; ++index) {
        auto key = take(IW::Detail::strictUtf16ToUtf8(values[index]));
        if (!key.starts_with("--")) {
            if (result.action != "preview" || !result.kind.empty()) throw std::runtime_error("Unexpected positional argument.");
            result.kind = std::move(key); continue;
        }
        key.erase(0U, 2U);
        if (++index == count || !result.options.emplace(key, take(IW::Detail::strictUtf16ToUtf8(values[index]))).second)
            throw std::runtime_error("Missing or duplicate --" + key);
    }
    const std::vector<std::string> allowed{"output","job","plan","input","prompt","workflow","arguments","trigger-arguments","open-popover","browser-open","seconds","draft-seconds","timeout","installation","endpoint","lm-root","resources","message","expected-conversation","new-chat","lm-executable","managed-runtime","observe"};
    for (const auto& [key, value] : result.options) {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) throw std::runtime_error("Unknown option --" + key);
    }
    if (result.option("output").empty() || !native(result.option("output")).is_absolute())
        throw std::runtime_error("--output must explicitly select an absolute isolated qualification directory.");
    if (!result.option("managed-runtime").empty() && result.action != "dependency-identity")
        throw std::runtime_error("--managed-runtime is restricted to the read-only dependency-identity action.");
    if (result.options.contains("observe") && result.action != "lmstudio-video-link")
        throw std::runtime_error("--observe is restricted to native video-link qualification.");
    if (result.options.contains("trigger-arguments") && result.action != "lmstudio-video-link")
        throw std::runtime_error("--trigger-arguments is restricted to native video-link qualification.");
    if (result.options.contains("open-popover") && result.action != "lmstudio-video-link")
        throw std::runtime_error("--open-popover is restricted to native video-link qualification.");
    if (result.options.contains("browser-open") && result.action != "lmstudio-video-link")
        throw std::runtime_error("--browser-open is restricted to native video-link qualification.");
    return result;
}

class Harness final {
public:
    explicit Harness(Arguments options) : options_{std::move(options)}, root_{native(options_.option("output"))},
        started_{std::chrono::steady_clock::now()} {
        Fs::create_directories(root_ / L"workspace"); Fs::create_directories(root_ / L"private"); Fs::create_directories(root_ / L"evidence");
        root_ = Fs::canonical(root_);
        for (const auto& path : {root_, root_ / L"workspace", root_ / L"private", root_ / L"evidence"})
            if (Fs::is_symlink(Fs::symlink_status(path))) throw std::runtime_error("Qualification directories must not be symbolic links.");
        configuration_.value.comfyUi.enabled = true;
        configuration_.value.comfyUi.automaticSetup = true;
        configuration_.value.comfyUi.installationPath = options_.option("installation", "A:\\ComfyUI\\ComfyUI_windows_portable");
        configuration_.value.comfyUi.endpoint = options_.option("endpoint", "http://127.0.0.1:8188");
        configuration_.value.comfyUi.generationTimeoutSeconds = options_.number("timeout", 1800U, 7200U);
        take(FC::Domain::validateComfyUiConfig(configuration_.value.comfyUi));
        issuer_ = std::make_unique<IW::WindowsWorkspaceAuthority>(std::vector<IW::WindowsWorkspaceAuthorityPolicy>{{
            identifier<FC::Domain::AuthorityId>("06c2b31b-9628-4a51-a64d-cf6a36beac66"), project(),
            identifier<FC::Domain::ClientId>("comfy-host-qualification"), {pathText(root_ / L"workspace")}, FC::Domain::FileAccess::Write,
            {FC::Domain::FileAccess::Read, FC::Domain::FileAccess::Write, FC::Domain::FileAccess::Create, FC::Domain::FileAccess::Execute}, {}, true, 1U}});
        storageIssuer_ = std::make_unique<IW::WindowsWorkspaceAuthority>(std::vector<IW::WindowsWorkspaceAuthorityPolicy>{{
            identifier<FC::Domain::AuthorityId>("578a02ed-1e60-4be1-b8dc-0e059de28c4d"), project(),
            identifier<FC::Domain::ClientId>("comfy-host-qualification-storage"), {pathText(root_ / L"private")}, FC::Domain::FileAccess::Write,
            {FC::Domain::FileAccess::Read, FC::Domain::FileAccess::Write, FC::Domain::FileAccess::Create}, {}, false, 1U}});
        scope_ = std::make_unique<FC::Contracts::WorkspaceAuthority>(take(issuer_->authorityFor(project(), context())));
        storageScope_ = std::make_unique<FC::Contracts::WorkspaceAuthority>(take(storageIssuer_->authorityFor(project(), context())));
        auto backendRoot = root_ / L"private" / L"runtime";
        if (options_.action == "dependency-identity") {
            const auto configured = native(required("managed-runtime"));
            if (!configured.is_absolute()) throw std::runtime_error("--managed-runtime must be absolute.");
            const auto expected = root_ / L"forge-home" / L"comfy-jobs";
            for (const auto& directory : {root_ / L"forge-home", expected}) {
                const auto attributes = GetFileAttributesW(directory.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    throw std::runtime_error("Managed identity inspection requires existing regular isolated runtime directories.");
            }
            backendRoot = Fs::canonical(configured);
            if (CompareStringOrdinal(backendRoot.c_str(), -1, Fs::canonical(expected).c_str(), -1, TRUE) != CSTR_EQUAL)
                throw std::runtime_error("--managed-runtime must select this isolated output's forge-home/comfy-jobs directory.");
        }
        backend_ = std::make_shared<NW::WindowsComfyUiBackend>(*issuer_, pathText(backendRoot));
        service_ = std::make_unique<NW::WindowsComfyUiService>(*issuer_, files_, configuration_, *storageIssuer_, *storageScope_,
            pathText(root_ / L"private" / L"jobs"), uuids_, clock_, hasher_, backend_);
        Fs::path lmRoot;
        if (!options_.option("lm-root").empty()) lmRoot = native(options_.option("lm-root"));
        else {
            std::array<wchar_t, 32768> profile{};
            const auto length = GetEnvironmentVariableW(L"USERPROFILE", profile.data(), static_cast<DWORD>(profile.size()));
            if (!length || length >= profile.size()) throw std::runtime_error("USERPROFILE is unavailable.");
            lmRoot = Fs::path{profile.data()} / L".lmstudio";
        }
        const auto conversationRoot = pathText(lmRoot);
        lmRoot_ = lmRoot;
        lmExecutable_ = native(options_.option("lm-executable", utf8(lmRoot.parent_path() / L"AppData" / L"Local" / L"Programs" / L"LM Studio" / L"LM Studio.exe")));
        service_->setConversationObserver([conversationRoot](const FC::Domain::ProjectId&,
            std::string_view boundConversation, const FC::Domain::OperationContext& context) {
            auto result = IW::WindowsLMStudioConversationReader::read(conversationRoot, context);
            if (!result) return FC::Domain::Result<std::string>::failure(result.error());
            if (!result.value()) return FC::Domain::Result<std::string>::failure(FC::Domain::makeError(
                FC::Domain::ErrorCodes::HostCapabilityUnavailable, "No actual selected saved LM Studio conversation is available."));
            const auto projectConversation = [](const IW::LMStudioConversationObservation& observation) {
                Json users = Json::array(), tools = Json::array();
                for (const auto& user : observation.userMessageEvidence) users.push_back({{"text", user.text},
                    {"message_index", user.messageIndex}, {"selected_version", user.selectedVersion},
                    {"forge_generated", user.forgeGenerated}});
                for (const auto& tool : observation.nativeToolResults) tools.push_back({{"name", tool.name}, {"plugin_identifier", tool.pluginIdentifier},
                    {"message_index", tool.messageIndex}, {"selected_version", tool.selectedVersion}, {"text_bodies", tool.textBodies}});
                return Json{{"conversation_id", observation.conversationId},
                    {"user_messages", std::move(users)}, {"native_tool_results", std::move(tools)}};
            };
            const auto projectError = [](const FC::Domain::Error& error) {
                auto length = std::min(error.message.size(), std::size_t{1024U});
                while (length < error.message.size() && length > 0U &&
                    (static_cast<unsigned char>(error.message[length]) & 0xc0U) == 0x80U) --length;
                return Json{{"code", error.code}, {"message", error.message.substr(0, length)}};
            };
            const auto& observation = *result.value(); auto projection = projectConversation(observation);
            if (!boundConversation.empty() && boundConversation != observation.conversationId) {
                const auto bound = IW::WindowsLMStudioConversationReader::readConversation(conversationRoot, boundConversation, context);
                if (bound && bound.value()) projection["bound_conversation"] = projectConversation(*bound.value());
                else if (!bound) projection["bound_conversation_error"] = projectError(bound.error());
                else projection["bound_conversation_error"] = {{"code", FC::Domain::ErrorCodes::HostCapabilityUnavailable},
                    {"message", "The ComfyUI plan's bound saved LM Studio conversation is unavailable."}};
                const auto refreshed = IW::WindowsLMStudioConversationReader::read(conversationRoot, context);
                if (!refreshed) return FC::Domain::Result<std::string>::failure(refreshed.error());
                if (!refreshed.value() || projectConversation(*refreshed.value()) != projectConversation(observation))
                    return FC::Domain::Result<std::string>::failure(FC::Domain::makeError(FC::Domain::ErrorCodes::Conflict,
                        "LM Studio selected conversation evidence changed while checking its saved conversations.", true));
            }
            return FC::Domain::Result<std::string>::success(projection.dump());
        });
        runId_ = take(uuids_.next()).value(); evidencePath_ = root_ / L"evidence" / (native(runId_ + ".json"));
        evidence_ = {{"format", "forge.comfyui.host_qualification"}, {"version", 1U}, {"run_id", runId_}, {"action", options_.action},
            {"qualification_kind", "native_host_harness"}, {"lm_studio_request_verified", false}, {"operator_quality_accepted", false},
            {"root", utf8(root_)}, {"lm_studio_root", utf8(lmRoot_)}, {"installation", configuration_.value.comfyUi.installationPath}, {"endpoint", configuration_.value.comfyUi.endpoint},
            {"events", Json::array()}, {"result", Json(nullptr)}};
        save();
    }
    ~Harness() { if (service_) service_->shutdown(); }
    int run() {
        try {
            Json result;
            if (options_.action.starts_with("lmstudio-")) result = nativeChat();
            else if (options_.action == "private-manager-status" || options_.action == "private-manager-drain") result = privateManager();
            else if (options_.action == "view-playback") result = viewPlayback();
            else if (options_.action == "desktop-comfy-input") result = desktopComfyInput();
            else {
            const auto initial = invoke("comfy_status", Json::object());
            event("initial_status", initial);
            if (options_.action == "status") result = initial;
            else if (options_.action == "start" || options_.action == "stop") result = wait(invoke("comfy_control", {{"action", options_.action}, {"timeout_sec", 120U}}));
            else if (options_.action == "frontend-serialization") {
                if (!initial.value("available",false)) start();
                result = frontend();
            }
            else if (options_.action == "dependency-identity") {
                auto operation = context(options_.number("timeout", 1800U, 7200U));
                const auto source = take(issuer_->authorize(*scope_, {pathText(native(required("workflow"))), std::nullopt,
                    FC::Domain::FileAccess::Read, false}, operation));
                const auto bytes = take(files_.read(source, 16U * 1024U * 1024U, operation));
                const auto args = Json::parse(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
                if (!args.is_object() || args.size() != 1U || !args.contains("workflows") ||
                    !args.at("workflows").is_array() || args.at("workflows").empty() || args.at("workflows").size() > 2U ||
                    !std::all_of(args.at("workflows").begin(), args.at("workflows").end(), [](const Json& graph) { return graph.is_object(); }))
                    throw std::runtime_error("Dependency identity requires one or two workflow objects under the workflows key.");
                result = Json::parse(take(backend_->perform("identity", args.dump(), configuration_.value.comfyUi, *scope_, operation)));
                event("read_only_dependency_identity", result);
            }
            else if (options_.action == "preview") result = preview();
            else if (options_.action == "job-status") result = invoke("comfy_job_status", {{"job_id", required("job")}, {"wait_sec", 0U}});
            else if (options_.action == "resume") result = wait(invoke("comfy_job_resume", {{"job_id", required("job")}}));
            else if (options_.action == "final") result = wait(invoke("comfy_run", {{"stage", "final"}, {"plan_id", required("plan")}}));
            else if (options_.action == "prepare") {
                Json args = options_.option("arguments").empty() ? Json::object() : readJson(native(options_.option("arguments")));
                if (!options_.option("workflow").empty()) args["workflow"] = readJson(native(options_.option("workflow")));
                result = wait(invoke("comfy_prepare", args));
            } else throw std::runtime_error("Unknown qualification action.");
            event("final_status", invoke("comfy_status", Json::object()));
            }
            evidence_["result"] = result; save();
            std::cout << Json{{"evidence_path", utf8(evidencePath_)}, {"result", result}}.dump(2) << '\n';
            const auto state = result.value("state", std::string{});
            return state == "failed" || state == "unknown" || state == "cancelled" || !result.value("ok", true) ? 1 : 0;
        } catch (const std::exception& error) {
            evidence_["error"] = error.what(); save();
            std::cerr << Json{{"evidence_path", utf8(evidencePath_)}, {"error", error.what()}}.dump(2) << '\n'; return 1;
        }
    }
private:
    FC::Domain::OperationContext context(unsigned seconds = 75U) {
        return {identifier<FC::Domain::OperationId>(take(uuids_.next()).value()), std::chrono::steady_clock::now() + std::chrono::seconds{seconds}, {},
            identifier<FC::Domain::CorrelationId>("comfy-host-qualification")};
    }
    std::string required(std::string_view key) const {
        const auto value = options_.option(key); if (value.empty()) throw std::runtime_error("--" + std::string{key} + " is required."); return value;
    }
    Json invoke(std::string_view name, const Json& args) { return Json::parse(take(service_->execute(name, args.dump(), *scope_, context()))); }
    Json privateManager() {
        const auto request = readJson(native(required("arguments")), 65536U);
        const std::vector<std::string> fields{"home", "executable", "pid", "creation_filetime", "sha256", "version"};
        if (!request.is_object() || request.size() != fields.size() ||
            std::any_of(fields.begin(), fields.end(), [&](const auto& key) { return !request.contains(key); }) ||
            !request.at("pid").is_number_unsigned() || !request.at("creation_filetime").is_number_unsigned())
            throw std::runtime_error("Private Manager arguments must contain exactly the six typed process/profile fields.");
        const auto home = native(request.at("home").get<std::string>());
        const auto image = native(request.at("executable").get<std::string>());
        const auto expectedHome = root_ / L"forge-home";
        const auto attributes = GetFileAttributesW(expectedHome.c_str());
        if (!home.is_absolute() || attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            CompareStringOrdinal(Fs::canonical(home).c_str(), -1, Fs::canonical(expectedHome).c_str(), -1, TRUE) != CSTR_EQUAL ||
            !image.is_absolute() || image.filename() != L"ForgeConductor.Manager.exe" ||
            !Fs::is_regular_file(image) || Fs::file_size(image) > 128U * 1024U * 1024U)
            throw std::runtime_error("Private Manager control requires the existing isolated forge-home and exact Manager image.");
        const auto expectedDigest = identifier<FC::Domain::Sha256Digest>(request.at("sha256").get<std::string>()).value();
        const auto version = request.at("version").get<std::string>();
        const auto widePid = request.at("pid").get<std::uint64_t>();
        const auto creation = request.at("creation_filetime").get<std::uint64_t>();
        if (!widePid || widePid > std::numeric_limits<DWORD>::max() || !creation || version.empty() || version.size() > 64U)
            throw std::runtime_error("Private Manager process identity or product version is invalid.");
        const auto pid = static_cast<DWORD>(widePid);
        CD::Handle retained{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)};
        if (!retained) throw std::runtime_error("The exact private Manager process cannot be retained.");
        const auto owner = take(IW::WindowsCurrentUserIdentity::load());
        const auto verify = [&] {
            FILETIME created{}, exited{}, kernel{}, user{};
            std::array<wchar_t, 32768> actualImage{}; DWORD imageLength = static_cast<DWORD>(actualImage.size());
            if (WaitForSingleObject(retained.get(), 0U) != WAIT_TIMEOUT ||
                !GetProcessTimes(retained.get(), &created, &exited, &kernel, &user) ||
                ((static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime) != creation ||
                !QueryFullProcessImageNameW(retained.get(), 0U, actualImage.data(), &imageLength) ||
                CompareStringOrdinal(actualImage.data(), static_cast<int>(imageLength), image.c_str(), -1, TRUE) != CSTR_EQUAL)
                throw std::runtime_error("The retained private Manager PID, creation time, or image changed.");
            HANDLE rawToken{};
            if (!OpenProcessToken(retained.get(), TOKEN_QUERY, &rawToken))
                throw std::runtime_error("The private Manager's Windows owner cannot be observed.");
            CD::Handle token{rawToken}; DWORD bytes{};
            GetTokenInformation(token.get(), TokenUser, nullptr, 0U, &bytes);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0U || bytes > 65536U)
                throw std::runtime_error("The private Manager's Windows owner has no bounded token-user record.");
            std::vector<std::byte> buffer(bytes);
            if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), bytes, &bytes) ||
                !EqualSid(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
                    const_cast<std::byte*>(owner.sidBytes().data())))
                throw std::runtime_error("The retained private Manager is not owned by the current Windows user.");
            try {
                if (CD::fileFacts(image, context(15U)).at("sha256") != expectedDigest)
                    throw std::runtime_error("The private Manager executable seal changed.");
            } catch (const CD::Failure& failure) { throw std::runtime_error(failure.error.code); }
        };
        verify();
        auto profile = take(IW::WindowsAlphaManagerProfile::create(pathText(Fs::canonical(home))));
        IW::WindowsManagerInstanceLeaseOptions leaseOptions; leaseOptions.purposeSuffix = profile.purposeSuffix();
        auto names = take(IW::WindowsManagerInstanceLease::namesFor(owner, leaseOptions));
        IW::DpapiSecureStorage secure{std::wstring{profile.secureStorageRegistrySubkey()}};
        IW::WindowsManagerAuthenticationTokenGenerator generator;
        IW::WindowsManagerAuthenticationTokenStore tokens{secure, generator};
        const auto existing = take(tokens.load(context(10U)));
        if (!existing) throw std::runtime_error("The selected private Manager has no existing IPC material; nothing was created.");
        auto clock = std::make_shared<IW::SystemClock>();
        auto client = take(IW::WindowsManagerNamedPipeClient::create(clock, std::wstring{names.pipeName()}, *existing));
        const auto number = [](std::string_view text) {
            std::uint64_t value{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                throw std::runtime_error("The native Manager work count is not an exact integer.");
            return value;
        };
        const auto inspect = [&] {
            const auto status = take(client->status(context(10U)));
            if (!status.ok || !status.isManager || status.processId != pid || status.version != version ||
                status.home.value() != profile.dataRoot().value())
                throw std::runtime_error("Native Manager status differs from the exact selected process, version, or private profile.");
            const auto runtime = take(client->operational({FC::Manager::ManagerOperationalArea::Runtimes,
                FC::Manager::ManagerOperationalAction::Inspect, std::nullopt, {}, std::nullopt}, context(15U)));
            const auto agents = take(client->operational({FC::Manager::ManagerOperationalArea::Agents,
                FC::Manager::ManagerOperationalAction::Inspect, std::nullopt, {}, std::nullopt}, context(15U)));
            const auto count = [&](const auto& page, std::string_view prefix) {
                std::optional<std::uint64_t> result;
                for (const auto& line : page.lines) if (line.starts_with(prefix)) {
                    if (result) throw std::runtime_error("The native Manager work count is duplicated.");
                    result = number(std::string_view{line}.substr(prefix.size()));
                }
                if (!result) throw std::runtime_error("The native Manager work count is unavailable.");
                return *result;
            };
            const auto operations = count(runtime, "Owned operations: "), children = count(runtime, "Child processes: ");
            const auto open = count(agents, "Open sessions: ");
            std::uint64_t found{}, active{}; bool inOpen{};
            const std::string separator{" \xC2\xB7 "};
            for (const auto& line : agents.lines) {
                if (line.starts_with("Open sessions: ")) { inOpen = true; continue; }
                if (line.starts_with("Recent sessions: ")) break;
                if (!inOpen) continue;
                const auto first = line.find(separator), second = first == std::string::npos ? first : line.find(separator, first + separator.size());
                if (first == std::string::npos || second == std::string::npos ||
                    !FC::Domain::SessionId::parse(std::string_view{line}.substr(0U, first)))
                    throw std::runtime_error("The native Manager open-session row is unverifiable.");
                const auto start = second + separator.size(), end = (std::min)(line.find(separator, start), line.find('\n', start));
                const auto state = line.substr(start, end == std::string::npos ? end : end - start);
                if (state != "open" && state != "active" && state != "running" && state != "started")
                    throw std::runtime_error("The native Manager open-session state is unknown.");
                ++found; if (state != "open") ++active;
            }
            if (found != open) throw std::runtime_error("The native Manager open-session count is incomplete.");
            return Json{{"ok", true}, {"pid", pid}, {"version", status.version}, {"home", status.home.value()},
                {"service_active", status.serviceActive}, {"http_listening", status.httpListening},
                {"owned_operations", operations}, {"child_processes", children}, {"open_sessions", open},
                {"active_sessions", active}, {"idle", operations == 0U && children == 0U && active == 0U}};
        };
        auto result = inspect(); verify();
        event("private_manager_status", result);
        result.update({{"creation_filetime", creation}, {"image", utf8(image)}, {"image_sha256", expectedDigest},
            {"shutdown_requested", false}, {"forced_termination", false}, {"provider_stop_requested", false}});
        if (options_.action == "private-manager-drain") {
            if (!result.at("idle").get<bool>()) throw std::runtime_error("Private Manager work is active; shutdown was not requested.");
            const auto second = inspect(); verify(); event("private_manager_second_status", second);
            if (!second.at("idle").get<bool>()) throw std::runtime_error("Private Manager work became active; shutdown was not requested.");
            event("private_manager_shutdown_request", {{"pid", pid}, {"creation_filetime", creation}});
            const auto requested = client->requestShutdown(context(20U));
            if (!requested) throw std::runtime_error("Private Manager shutdown acknowledgement: " + requested.error().code);
            result["shutdown_requested"] = true; event("private_manager_shutdown_acknowledged", result);
            client->shutdown();
            if (WaitForSingleObject(retained.get(), 60000U) != WAIT_OBJECT_0)
                throw std::runtime_error("Graceful shutdown was acknowledged, but the retained Manager did not exit within 60 seconds.");
            DWORD exitCode{};
            if (!GetExitCodeProcess(retained.get(), &exitCode)) throw std::runtime_error("The retained Manager's exit code is unavailable.");
            result["exact_process_exited"] = true; result["exit_code"] = exitCode;
            event("private_manager_exit", result);
            if (exitCode != 0U) throw std::runtime_error("The private Manager exited with a nonzero code.");
        } else client->shutdown();
        return result;
    }
    void save() {
        evidence_["elapsed_seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        evidence_["recorded_unix_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        writeJson(evidencePath_, evidence_);
    }
    void event(std::string_view type, const Json& value) {
        evidence_["events"].push_back({{"type", type}, {"elapsed_seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count()}, {"value", value}}); save();
    }
    Json wait(Json job) {
        event("job_admitted", job); if (!job.contains("job_id")) return job;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{configuration_.value.comfyUi.generationTimeoutSeconds + 60U};
        auto nextResources = std::chrono::steady_clock::now(); std::string previous;
        for (;;) {
            job = invoke("comfy_job_status", {{"job_id", job.at("job_id")}, {"wait_sec", 2U}});
            const auto state = job.value("state", std::string{});
            if (state != previous) { event("job_phase", job); previous = state; std::cout << state << '\n'; }
            if (job.value("done", false)) { event("job_terminal", job); return job; }
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Qualification wait exceeded the configured generation timeout; use job-status/resume for exact reconciliation.");
            if (std::chrono::steady_clock::now() >= nextResources) {
                event("resources_and_provider_queue", invoke("comfy_status", Json::object())); nextResources = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            }
        }
    }
    void start() {
        const auto result = wait(invoke("comfy_control", {{"action", "start"}, {"timeout_sec", 120U}}));
        if (result.value("state", std::string{}) != "completed") throw std::runtime_error("ComfyUI startup did not complete; inspect qualification evidence and runtime.log.");
    }
    Json conversation() {
        const auto observed = take(IW::WindowsLMStudioConversationReader::read(pathText(lmRoot_),context()));
        if (!observed) throw std::runtime_error("LM Studio has no selected saved native conversation.");
        const auto& current = *observed; Json users = Json::array(), tools = Json::array();
        for (const auto& user : current.userMessageEvidence) {
            const auto digest = take(hasher_.sha256(std::as_bytes(std::span{user.text.data(),user.text.size()}))).value();
            users.push_back({{"message_index",user.messageIndex},{"selected_version",user.selectedVersion},{"sha256",digest},{"bytes",user.text.size()}});
        }
        for (const auto& tool : current.nativeToolResults) {
            Json receipts = Json::array();
            if (tool.name.starts_with("comfy_")) for (const auto& body : tool.textBodies) {
                auto value = Json::parse(body,nullptr,false); if (value.is_object()) receipts.push_back(std::move(value));
            }
            tools.push_back({{"name",tool.name},{"plugin_identifier",tool.pluginIdentifier},{"message_index",tool.messageIndex},
                {"selected_version",tool.selectedVersion},{"comfy_receipts",receipts}});
        }
        return {{"ok",true},{"conversation_id",current.conversationId},{"conversation_path",current.conversationPath},
            {"lm_project_identifier",current.projectIdentifier},{"tools_active",current.toolsActive},{"stop_reason",current.stopReason},
            {"used_tokens",current.usedTokens},{"context_capacity",current.contextCapacity},{"user_message_evidence",users},{"native_tool_results",tools}};
    }
    Json nativeChat() {
        const auto before = conversation(); event("native_conversation_before",before);
        if (options_.action == "lmstudio-observe") return before;
        if (options_.action == "lmstudio-video-link") return videoLink(before);
        if (options_.action == "lmstudio-clear-owned-draft") return clearOwnedDraft(before);
        if (options_.action == "lmstudio-owned-draft-status") return clearOwnedDraft(before,true);
        if (options_.action == "lmstudio-activate") {
            const auto result = IW::WindowsLMStudioChatControl::activate(pathText(lmExecutable_),context());
            if (!result) throw std::runtime_error(result.error().code+": "+result.error().message);
            return {{"ok",true},{"activated",true},{"conversation",conversation()}};
        }
        if (options_.action == "lmstudio-idle") return {{"ok",true},{"idle",take(IW::WindowsLMStudioChatControl::idle(pathText(lmExecutable_),context()))},{"conversation",before}};
        if (options_.action == "lmstudio-pause") {
            const auto expected = required("expected-conversation");
            if (before.at("conversation_id") != expected) throw std::runtime_error("Selected LM Studio conversation changed before qualification pause.");
            const auto paused = take(IW::WindowsLMStudioChatControl::pauseAtToolBoundary(pathText(lmExecutable_),expected,context()));
            return {{"ok",true},{"paused_at_tool_boundary",paused},{"conversation",conversation()}};
        }
        if (options_.action != "lmstudio-send") throw std::runtime_error("Unknown native LM Studio action.");
        const auto expected = required("expected-conversation");
        if (before.at("conversation_id") != expected) throw std::runtime_error("Selected LM Studio conversation changed before qualification send.");
        const auto source = native(required("message"));
        if (!Fs::is_regular_file(source) || Fs::file_size(source) > 65536U) throw std::runtime_error("Native qualification message must be a regular UTF-8 file of at most 64 KiB.");
        std::ifstream stream{source,std::ios::binary}; const std::string message{std::istreambuf_iterator<char>{stream},{}};
        if (!stream.eof() && stream.fail()) throw std::runtime_error("Cannot read native qualification message.");
        if (message.empty() || !FC::Domain::isValidUtf8(message)) throw std::runtime_error("Native qualification message must be nonempty UTF-8.");
        std::string normalized; for (const unsigned char character : message) if (std::isalnum(character) || std::isspace(character)) normalized.push_back(static_cast<char>(std::tolower(character)));
        const auto first = normalized.find_first_not_of(" \t\r\n"),last = normalized.find_last_not_of(" \t\r\n");
        normalized = first == std::string::npos ? std::string{} : normalized.substr(first,last-first+1U);
        if (normalized == "approved" || normalized == "yes" || normalized == "render final")
            throw std::runtime_error("The qualification harness cannot send an operator approval reply.");
        const auto create = options_.option("new-chat","true");
        if (create != "true" && create != "false") throw std::runtime_error("--new-chat must be true or false.");
        auto operation = context(150U);
        const auto result = IW::WindowsLMStudioChatControl::send(pathText(lmExecutable_),message,create=="true",operation,expected,
            [this](std::string_view successor) { event("native_successor_created",{{"conversation_id",successor}}); },
            [this](const IW::LMStudioChatEffectReceipt& effect) {
                try {
                    event("native_chat_effect",{{"effect",effect.effect==IW::LMStudioChatEffect::NewChat?"new_chat":"send"},
                        {"stage",effect.stage==IW::LMStudioChatEffectStage::BeforeDispatch?"before_dispatch":"confirmed"},
                        {"conversation_id",effect.conversationId},{"previous_user_messages",effect.previousUserMessages}});
                    return FC::Domain::Result<void>::success();
                } catch (const std::exception& error) { return FC::Domain::Result<void>::failure(FC::Domain::makeError(FC::Domain::ErrorCodes::InternalFailure,error.what())); }
            });
        event("native_send_result",{{"ok",static_cast<bool>(result)},{"error",result?Json(nullptr):Json{{"code",result.error().code},{"message",result.error().message}}}});
        if (!result) throw std::runtime_error(result.error().code+": "+result.error().message);
        const auto after = conversation(); event("native_conversation_after",after);
        evidence_["native_qualification_prompt_sent"] = true;
        return {{"ok",true},{"native_qualification_prompt_sent",true},{"conversation",after}};
    }
    Json videoLink(const Json& before) {
        try {
            const auto expected=required("expected-conversation"),job=required("job");
            const auto observation=options_.option("observe","false");if(observation!="true" && observation!="false")throw std::runtime_error("--observe must be true or false.");
            const auto popoverOption=options_.option("open-popover","false");if(popoverOption!="true" && popoverOption!="false")throw std::runtime_error("--open-popover must be true or false.");
            const auto openPopover=popoverOption=="true";if(openPopover && (observation=="true" || !options_.option("arguments").empty() || !options_.option("trigger-arguments").empty()))throw std::runtime_error("Popover browser input cannot include read-only observation, viewport or trigger input.");
            const auto browserOption=options_.option("browser-open","false");if(browserOption!="true" && browserOption!="false")throw std::runtime_error("--browser-open must be true or false.");
            const auto browserOpen=browserOption=="true";if(browserOpen && (observation=="true" || openPopover || !options_.option("arguments").empty() || !options_.option("trigger-arguments").empty()))throw std::runtime_error("Native browser launch cannot include observation, viewport, trigger or popover input.");
            const auto observeOnly=observation=="true";if(observeOnly && (!options_.option("arguments").empty() || !options_.option("trigger-arguments").empty()))throw std::runtime_error("Read-only video-link observation cannot include input arguments.");
            if(!options_.option("arguments").empty() && !options_.option("trigger-arguments").empty())throw std::runtime_error("Observed renderer trigger cannot include viewport input arguments.");
            if (before.at("conversation_id")!=expected || before.at("tools_active").get<bool>() ||
                !take(IW::WindowsLMStudioChatControl::idle(pathText(lmExecutable_),context())))
                throw std::runtime_error("Video-link qualification requires the exact idle selected native conversation.");
            auto operation=context(180U);
            Json triggerArgs,triggerSeal,observedCaptureSeal;Fs::path triggerPath,observedCapturePath;
            if(!options_.option("trigger-arguments").empty()){
                const auto supplied=native(options_.option("trigger-arguments"));if(!supplied.is_absolute())throw std::runtime_error("Observed renderer-trigger arguments must use an absolute workspace path.");
                const auto authorized=take(issuer_->authorize(*scope_,{pathText(supplied),std::nullopt,FC::Domain::FileAccess::Read,false},operation));
                triggerPath=native(authorized.canonicalPath().value());triggerSeal=CD::fileFacts(triggerPath,operation);triggerArgs=readJson(triggerPath,64U*1024U);
                const auto bounded=[&](const char* key,std::int64_t minimum,std::int64_t maximum){const auto& value=triggerArgs.at(key);
                    if(!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>()>static_cast<std::uint64_t>(maximum)))return false;
                    const auto number=value.get<std::int64_t>();return number>=minimum && number<=maximum;};
                if(!triggerArgs.is_object() || triggerArgs.size()!=6U || !triggerArgs.contains("window_id") || !triggerArgs.contains("pid") || !triggerArgs.contains("x") || !triggerArgs.contains("y") || !triggerArgs.contains("capture_path") || !triggerArgs.contains("capture_sha256") ||
                    !bounded("window_id",1,(std::numeric_limits<std::int64_t>::max)()) || !bounded("pid",1,MAXDWORD) || !bounded("x",0,MAXLONG) || !bounded("y",0,MAXLONG) ||
                    !triggerArgs.at("capture_path").is_string() || !triggerArgs.at("capture_sha256").is_string())
                    throw std::runtime_error("Observed renderer-trigger arguments must contain exactly bounded window_id, pid, x, y and capture_path, capture_sha256 strings.");
                const auto suppliedCapture=native(triggerArgs.at("capture_path").get<std::string>());
                if(!suppliedCapture.is_absolute() || !CD::contained(root_/L"workspace",suppliedCapture))throw std::runtime_error("Observed renderer-trigger capture must be inside the qualification workspace.");
                const auto authorizedCapture=take(issuer_->authorize(*scope_,{pathText(suppliedCapture),std::nullopt,FC::Domain::FileAccess::Read,false},operation));
                observedCapturePath=native(authorizedCapture.canonicalPath().value());observedCaptureSeal=CD::fileFacts(observedCapturePath,operation);
                if(observedCaptureSeal.at("sha256")!=triggerArgs.at("capture_sha256") || CD::fileFacts(triggerPath,operation)!=triggerSeal)
                    throw std::runtime_error("Observed renderer-trigger arguments or captured pixels changed while reading.");
                event("native_video_trigger_arguments",{{"arguments",triggerArgs},{"seal",triggerSeal},{"capture_seal",observedCaptureSeal}});
            }
            Json scrollArgs,scrollSeal;Fs::path scrollPath;
            if(!options_.option("arguments").empty()){
                const auto supplied=native(options_.option("arguments"));if(!supplied.is_absolute())throw std::runtime_error("Observed video-scroll arguments must use an absolute workspace path.");
                const auto authorized=take(issuer_->authorize(*scope_,{pathText(supplied),std::nullopt,FC::Domain::FileAccess::Read,false},operation));
                scrollPath=native(authorized.canonicalPath().value());scrollSeal=CD::fileFacts(scrollPath,operation);scrollArgs=readJson(scrollPath,64U*1024U);
                if(CD::fileFacts(scrollPath,operation)!=scrollSeal)throw std::runtime_error("Observed video-scroll argument file changed while reading.");
                const auto bounded=[&](const char* key,std::int64_t minimum,std::int64_t maximum){const auto& value=scrollArgs.at(key);
                    if(!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>()>static_cast<std::uint64_t>(maximum)))return false;
                    const auto number=value.get<std::int64_t>();return number>=minimum && number<=maximum;};
                if(!scrollArgs.is_object() || scrollArgs.size()!=5U || !scrollArgs.contains("window_id") || !scrollArgs.contains("pid") || !scrollArgs.contains("x") || !scrollArgs.contains("y") || !scrollArgs.contains("delta") ||
                    !bounded("window_id",1,(std::numeric_limits<std::int64_t>::max)()) || !bounded("pid",1,MAXDWORD) || !bounded("x",0,MAXLONG) || !bounded("y",0,MAXLONG) ||
                    !bounded("delta",-12000,12000) || scrollArgs.at("delta").get<std::int64_t>()==0)
                    throw std::runtime_error("Observed video-scroll arguments must contain exactly bounded window_id, pid, x, y, delta integers.");
                event("native_video_scroll_arguments",{{"arguments",scrollArgs},{"seal",scrollSeal}});
            }
            Json artifacts=Json::array();
            for(const auto& tool:before.at("native_tool_results")) {
                if(tool.at("plugin_identifier")!="mcp/forge-conductor")continue;
                for(const auto& receipt:tool.at("comfy_receipts")) {
                    if(receipt.value("job_id",std::string{})!=job || receipt.value("publication_suppressed",false))continue;
                    for(const auto* group:{"artifacts","preview_artifacts"}) for(const auto& artifact:receipt.value(group,Json::array())) {
                        if(!artifact.value("media_type",std::string{}).starts_with("video/") || !artifact.contains("provider_view_url"))continue;
                        const auto metadata=artifact.value("metadata",Json::object());
                        if(!metadata.value("decoded",false) || metadata.value("duration",0.0)<=0.0 || metadata.value("frame_count",0.0)<2.0)continue;
                        if(std::none_of(artifacts.begin(),artifacts.end(),[&](const Json& old){return old.at("path")==artifact.at("path") && old.at("sha256")==artifact.at("sha256") && old.at("provider_view_url")==artifact.at("provider_view_url");})) artifacts.push_back(artifact);
                    }
                }
            }
            if(artifacts.size()!=1U)throw std::runtime_error("The selected native job must deliver exactly one distinct decoded video artifact.");
            const auto artifact=artifacts.at(0); const auto url=artifact.at("provider_view_url").get<std::string>();
            const auto& descriptor=artifact.at("descriptor");
            const auto descriptorUrl=configuration_.value.comfyUi.endpoint+"/view?filename="+CD::encode(descriptor.at("filename").get<std::string>())+
                "&subfolder="+CD::encode(descriptor.at("subfolder").get<std::string>())+"&type="+CD::encode(descriptor.at("type").get<std::string>());
            if(configuration_.value.comfyUi.endpoint!="http://127.0.0.1:8188" || url!=descriptorUrl || descriptor.at("type")!="output")
                throw std::runtime_error("Saved video URL differs from its actual owned loopback output descriptor.");
            Fs::path launchReceiptPath;Json launchReceiptSeal,launchJob;
            if(browserOpen){auto folder=descriptor.at("subfolder").get<std::string>();std::replace(folder.begin(),folder.end(),'\\','/');const auto folderPath=native(folder);
                const auto project=utf8(folderPath.parent_path().parent_path().filename()),stage=utf8(folderPath.filename());
                if(identifier<FC::Domain::OperationId>(job).value()!=job || identifier<FC::Domain::ProjectId>(project).value()!=project || (stage!="preview" && stage!="final") || folder!="ForgeConductor/"+project+"/"+job+"/"+stage)
                    throw std::runtime_error("Native browser launch descriptor is outside the exact project/job/render namespace.");
                launchReceiptPath=root_/L"forge-home"/L"comfy-jobs"/native(project)/native(job)/L"receipt.json";CD::regularParents(launchReceiptPath);
                launchReceiptSeal=CD::fileFacts(launchReceiptPath,operation);const auto envelope=CD::readJson(launchReceiptPath);launchJob=envelope.at("payload");
                const auto hash=[&](const std::string& value){return take(hasher_.sha256(std::as_bytes(std::span{value.data(),value.size()}))).value();};
                if(hash(launchJob.dump())!=envelope.at("sha256").get<std::string>() || launchJob.value("schema_version",0U)!=1U || launchJob.value("kind",std::string{})!="forge_comfy_job" ||
                    launchJob.at("job_id")!=job || launchJob.at("project_id")!=project || launchJob.at("stage")!=stage || !launchJob.value("submission_acknowledged",false) ||
                    launchJob.value("publication_suppressed",true) || launchJob.value("cancellation_requested",true) || launchJob.value("remote_state",std::string{})!="completed" ||
                    (launchJob.value("state",std::string{})!="awaiting_preview_approval" && launchJob.value("state",std::string{})!="completed") || hash(launchJob.at("graph").dump())!=launchJob.at("graph_sha256").get<std::string>())
                    throw std::runtime_error("Native browser launch requires an acknowledged completed sealed versioned receipt.");
                Json matches=Json::array();for(const auto& candidate:launchJob.at(stage=="preview"?"preview_artifacts":"artifacts"))if(candidate.at("path")==artifact.at("path") && candidate.at("sha256")==artifact.at("sha256") && candidate.at("bytes")==artifact.at("bytes") && candidate.at("descriptor")==descriptor && candidate.at("provider_view_url")==url)matches.push_back(candidate);
                if(matches.size()!=1U || CD::fileFacts(launchReceiptPath,operation)!=launchReceiptSeal)throw std::runtime_error("Saved native video differs from the exact current receipt artifact.");
                event("native_video_browser_receipt",{{"receipt_path",utf8(launchReceiptPath)},{"seal",launchReceiptSeal},{"job_id",job},{"prompt_id",launchJob.at("prompt_id")},{"graph_sha256",launchJob.at("graph_sha256")},{"artifact",matches.at(0)}});
            }
            const auto videoPath=native(artifact.at("path").get<std::string>());
            const auto authorizedVideo=take(issuer_->authorize(*scope_,{pathText(videoPath),std::nullopt,FC::Domain::FileAccess::Read,false},operation));
            const auto verifyVideo=[&] {
                if(browserOpen && CD::fileFacts(launchReceiptPath,operation)!=launchReceiptSeal)throw std::runtime_error("Sealed video receipt changed during native browser launch qualification.");
                const auto facts=CD::fileFacts(native(authorizedVideo.canonicalPath().value()),operation);
                if(facts.at("sha256")!=artifact.at("sha256") || facts.at("bytes")!=artifact.at("bytes"))throw std::runtime_error("Native delivered video changed before link observation.");
                return facts;
            };
            const auto videoSeal=verifyVideo(); event("native_video_link_artifact",{{"job_id",job},{"artifact",artifact},{"seal",videoSeal}});
            const auto savedPath=native(before.at("conversation_path").get<std::string>());
            const auto selectedAssistant=[&] {
                const auto saved=readJson(savedPath,64U*1024U*1024U);Json parts=Json::array();std::size_t messageIndex{};
                for(const auto& message:saved.at("messages")) {
                    const auto selected=message.at("currentlySelected").get<std::size_t>();const auto& version=message.at("versions").at(selected);
                    if(version.value("role",std::string{})!="assistant"){++messageIndex;continue;}
                    const auto append=[&](const Json& content,std::size_t step) {
                        if(!content.is_array())return;std::size_t partIndex{};
                        for(const auto& part:content){if(part.is_object() && part.value("type",std::string{})=="text" && part.contains("text") && part.at("text").is_string())
                            parts.push_back({{"message_index",messageIndex},{"selected_version",selected},{"step_index",step},{"part_index",partIndex},{"text",part.at("text")}});++partIndex;}
                    };
                    if(version.contains("content"))append(version.at("content"),0U);
                    if(version.contains("steps")){std::size_t step{};for(const auto& value:version.at("steps")){if(value.value("type",std::string{})=="contentBlock" && value.contains("content"))append(value.at("content"),step);++step;}}
                    ++messageIndex;
                }
                return parts;
            };
            const auto assistant=selectedAssistant();const auto assistantBytes=assistant.dump();
            const auto assistantSha=take(hasher_.sha256(std::as_bytes(std::span{assistantBytes.data(),assistantBytes.size()}))).value();
            Json links=Json::array();
            for(const auto& part:assistant) {
                const auto& text=part.at("text").get_ref<const std::string&>();std::size_t offset{};
                while((offset=text.find('[',offset))!=std::string::npos){const auto opening=offset++,labelEnd=text.find("](",offset);
                    if(labelEnd==std::string::npos)break;const auto end=text.find(')',labelEnd+2U);if(end==std::string::npos)break;
                    if(!opening || text[opening-1U]!='!')links.push_back({{"label",text.substr(offset,labelEnd-offset)},{"url",text.substr(labelEnd+2U,end-labelEnd-2U)},
                        {"message_index",part.at("message_index")},{"selected_version",part.at("selected_version")},{"step_index",part.at("step_index")}});
                    offset=end+1U;
                }
                const auto autolink="<"+url+">";offset=0U;
                while((offset=text.find(autolink,offset))!=std::string::npos){links.push_back({{"label",url},{"url",url},{"message_index",part.at("message_index")},
                    {"selected_version",part.at("selected_version")},{"step_index",part.at("step_index")}});offset+=autolink.size();}
            }
            Json matching=Json::array();for(const auto& link:links)if(link.at("url")==url)matching.push_back(link);
            if(matching.size()!=1U)throw std::runtime_error("The selected saved assistant must contain exactly one literal Markdown/autolink to the delivered video URL.");
            const auto anchor=matching.at(0);const auto label=anchor.at("label").get<std::string>();
            if(label.empty() || label.size()>4096U || label.find_first_of("[]*`\\\r\n")!=std::string::npos ||
                std::count_if(links.begin(),links.end(),[&](const Json& link){return link.at("label")==label;})!=1)
                throw std::runtime_error("Actual saved video label is empty, formatted or ambiguous across selected assistant links.");
            event("native_video_saved_anchor",{{"anchor",anchor},{"selected_assistant_sha256",assistantSha}});
            NW::WindowsDesktopArtifactService desktop{*issuer_,files_};
            const auto desktopCall=[&](std::string_view name,const Json& args){return Json::parse(take(desktop.execute(name,args.dump(),*scope_,context(20U))));};
            const auto processFacts=[&](DWORD pid) {
                CD::Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid)};
                FILETIME created{},exited{},kernel{},user{};std::array<wchar_t,32768> image{};DWORD size=static_cast<DWORD>(image.size());
                if(!process || WaitForSingleObject(process.get(),0U)!=WAIT_TIMEOUT || !GetProcessTimes(process.get(),&created,&exited,&kernel,&user) ||
                    !QueryFullProcessImageNameW(process.get(),0U,image.data(),&size))throw std::runtime_error("Observed native process is unavailable.");
                const Fs::path executable{std::wstring{image.data(),size}};
                return Json{{"pid",pid},{"creation_time",(static_cast<std::uint64_t>(created.dwHighDateTime)<<32U)|created.dwLowDateTime},
                    {"executable",utf8(executable)},{"executable_sha256",CD::fileFacts(executable,operation).at("sha256")}};
            };
            const auto readAll=[&](const Json& window) {
                Json elements=Json::array();unsigned offset{};
                for(unsigned page=0U;page<100U;++page){const auto observed=desktopCall("desktop_read",{{"window_id",window.at("window_id")},{"pid",window.at("pid")},{"limit",300U},{"offset",offset}});
                    for(const auto& item:observed.at("elements"))elements.push_back(item);
                    if(!observed.at("has_more").get<bool>())return elements;
                    const auto next=observed.at("next_offset").get<unsigned>();if(next<=offset || next>10000U)throw std::runtime_error("Native accessibility pagination did not progress within its bound.");offset=next;}
                throw std::runtime_error("Native accessibility inventory exceeded the qualification page bound.");
            };
            const auto popoverControls=[&](const Json& controls) {
                Json urls=Json::array(),buttons=Json::array();for(const auto& item:controls){if(item.value("name_truncated",false) || !item.value("enabled",false) || item.value("offscreen",true) || item.at("width").get<int>()<=0 || item.at("height").get<int>()<=0)continue;
                    if(item.at("control_type")==UIA_TextControlTypeId && item.at("name")==url)urls.push_back(item);
                    if(item.at("control_type")==UIA_ButtonControlTypeId && item.at("name")=="Open in browser")buttons.push_back(item);}
                if(urls.size()>1U || buttons.size()>1U)throw std::runtime_error("Visible native URL popover controls are ambiguous.");
                if(urls.size()==1U && buttons.size()==1U)return Json{{"url",urls.at(0)},{"button",buttons.at(0)}};return Json{};
            };
            const auto capture=[&](const Json& window,const char* suffix){const auto image=desktopCall("desktop_capture",{{"window_id",window.at("window_id")},{"pid",window.at("pid")},
                {"path",utf8(root_/L"workspace"/native(runId_+"."+suffix+".png"))},{"preview_max_dimension",256U}});const auto facts=CD::fileFacts(native(image.at("path").get<std::string>()),operation);event(suffix,facts);return facts;};
            const auto ownerPath=root_/L"forge-home"/L"comfy-jobs"/L"runtime-owner.json";
            const auto owner=CD::readJson(ownerPath),ownerSeal=CD::fileFacts(ownerPath,operation),providerProcess=processFacts(owner.at("pid").get<DWORD>());
            const auto observeProvider=[&] {
                if(owner.at("endpoint")!=configuration_.value.comfyUi.endpoint || processFacts(owner.at("pid").get<DWORD>())!=providerProcess ||
                    providerProcess.at("creation_time")!=owner.at("creation_time") ||
                    CompareStringOrdinal(native(providerProcess.at("executable").get<std::string>()).c_str(),-1,native(owner.at("python").get<std::string>()).c_str(),-1,TRUE)!=CSTR_EQUAL ||
                    CD::fileFacts(ownerPath,operation).at("sha256")!=ownerSeal.at("sha256"))throw std::runtime_error("Retained video provider identity changed.");
                DWORD bytes{};bool ownsListener=false;
                if(GetExtendedTcpTable(nullptr,&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0)==ERROR_INSUFFICIENT_BUFFER){std::vector<std::byte> storage(bytes);
                    if(GetExtendedTcpTable(storage.data(),&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0)==NO_ERROR){const auto table=reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
                        for(DWORD i=0U;i<table->dwNumEntries;++i){const auto& row=table->table[i];if(row.dwOwningPid==owner.at("pid").get<DWORD>() && ntohs(static_cast<u_short>(row.dwLocalPort))==8188U && row.dwLocalAddr==htonl(INADDR_LOOPBACK))ownsListener=true;}}}
                if(!ownsListener)throw std::runtime_error("Exact retained video provider no longer owns its loopback listener.");
                const auto queue=CD::http(configuration_.value.comfyUi.endpoint+"/queue","GET","","",16U*1024U*1024U,operation),history=CD::http(configuration_.value.comfyUi.endpoint+"/history","GET","","",16U*1024U*1024U,operation);
                if(queue.status!=200U || history.status!=200U)throw std::runtime_error("Read-only video provider observation failed.");
                const auto q=CD::parse(queue.body);if(!q.at("queue_running").empty() || !q.at("queue_pending").empty())throw std::runtime_error("Video link click requires an idle provider.");
                return Json{{"queue",q},{"history",CD::parse(history.body)},{"process",providerProcess},{"owner_sha256",ownerSeal.at("sha256")}};
            };
            const auto providerBefore=observeProvider();event("native_video_provider_before",providerBefore);
            if(browserOpen){const auto prompt=launchJob.at("prompt_id").get<std::string>();const auto& history=providerBefore.at("history");
                if(!history.contains(prompt) || !history.at(prompt).at("status").value("completed",false) || history.at(prompt).at("prompt").at(1)!=prompt || history.at(prompt).at("prompt").at(2)!=launchJob.at("graph"))
                    throw std::runtime_error("Exact current provider history differs from the sealed video receipt prompt.");}
            const auto verifyConversation=[&] {
                const auto current=conversation();if(current.at("conversation_id")!=expected || current.at("tools_active").get<bool>() ||
                    current.at("user_message_evidence")!=before.at("user_message_evidence") || current.at("native_tool_results")!=before.at("native_tool_results") || selectedAssistant()!=assistant)
                    throw std::runtime_error("Selected native conversation or delivered link changed during qualification.");
            };
            const auto inventory=desktopCall("desktop_list",Json::object());Json lmWindow,hyperlink,lmProcess,popoverUrl;unsigned observedWindowIndex{};bool viewportSubmitted{};
            for(const auto& row:inventory.at("windows")) {
                CD::Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,row.at("pid").get<DWORD>())};std::array<wchar_t,32768> image{};DWORD size=static_cast<DWORD>(image.size());
                if(!process || !QueryFullProcessImageNameW(process.get(),0U,image.data(),&size) || CompareStringOrdinal(image.data(),static_cast<int>(size),lmExecutable_.c_str(),-1,TRUE)!=CSTR_EQUAL)continue;
                if(!scrollArgs.is_null() && (row.at("window_id")!=scrollArgs.at("window_id") || row.at("pid")!=scrollArgs.at("pid")))continue;
                if(!triggerArgs.is_null() && (row.at("window_id")!=triggerArgs.at("window_id") || row.at("pid")!=triggerArgs.at("pid")))continue;
                const auto rowProcess=processFacts(row.at("pid").get<DWORD>());auto elements=readAll(row);
                event("native_video_observed_window",{{"window",row},{"process",rowProcess},{"elements",elements}});
                const auto suffix="native_video_observed_window_"+std::to_string(observedWindowIndex++);capture(row,suffix.c_str());
                if(!triggerArgs.is_null()){
                    Json labels=Json::array();for(const auto& item:elements)if(item.at("control_type")==UIA_TextControlTypeId && item.at("name")==label && !item.value("name_truncated",false) && item.value("enabled",false))labels.push_back(item);
                    if(labels.size()!=1U || triggerArgs.at("x").get<int>()>=row.at("width").get<int>() || triggerArgs.at("y").get<int>()>=row.at("height").get<int>())
                        throw std::runtime_error("Observed renderer-trigger label or point is unavailable in the exact native window.");
                    verifyConversation();verifyVideo();const auto freshCapture=capture(row,"native_video_trigger_before");
                    if(freshCapture.at("sha256")!=observedCaptureSeal.at("sha256") || freshCapture.at("bytes")!=observedCaptureSeal.at("bytes") ||
                        CD::fileFacts(observedCapturePath,operation)!=observedCaptureSeal || CD::fileFacts(triggerPath,operation)!=triggerSeal ||
                        processFacts(row.at("pid").get<DWORD>())!=rowProcess || reinterpret_cast<std::uintptr_t>(GetForegroundWindow())!=row.at("window_id").get<std::uintptr_t>() || observeProvider()!=providerBefore)
                        throw std::runtime_error("Observed renderer-trigger pixels, argument seals, process, foreground or provider changed before click.");
                    const Json args{{"window_id",row.at("window_id")},{"pid",row.at("pid")},{"x",triggerArgs.at("x")},{"y",triggerArgs.at("y")},{"button","left"}};
                    event("native_video_renderer_trigger_click",{{"stage","before_dispatch"},{"window",row},{"process",rowProcess},{"label",labels.at(0)},{"anchor",anchor},{"capture",freshCapture},{"arguments",args}});
                    const auto triggerReceipt=desktopCall("desktop_click",args);event("native_video_renderer_trigger_click",{{"stage","returned"},{"receipt",triggerReceipt}});
                    if(!triggerReceipt.value("input_submitted",false))throw std::runtime_error("Guarded native renderer-trigger click was not submitted.");
                    std::this_thread::sleep_for(std::chrono::milliseconds{500});CD::check(operation);verifyConversation();verifyVideo();
                    const auto providerAfterTrigger=observeProvider();event("native_video_provider_after_trigger",providerAfterTrigger);
                    if(providerAfterTrigger!=providerBefore || processFacts(row.at("pid").get<DWORD>())!=rowProcess || reinterpret_cast<std::uintptr_t>(GetForegroundWindow())!=row.at("window_id").get<std::uintptr_t>() ||
                        CD::fileFacts(observedCapturePath,operation)!=observedCaptureSeal || CD::fileFacts(triggerPath,operation)!=triggerSeal)
                        throw std::runtime_error("Native conversation, artifact, provider, process, foreground or seals changed after renderer-trigger click.");
                    const auto afterElements=readAll(row);event("native_video_observed_window_after_renderer_trigger",{{"window",row},{"process",rowProcess},{"elements",afterElements}});capture(row,"native_video_renderer_popover");
                    verifyConversation();verifyVideo();if(observeProvider()!=providerBefore || processFacts(row.at("pid").get<DWORD>())!=rowProcess || reinterpret_cast<std::uintptr_t>(GetForegroundWindow())!=row.at("window_id").get<std::uintptr_t>() ||
                        CD::fileFacts(observedCapturePath,operation)!=observedCaptureSeal || CD::fileFacts(triggerPath,operation)!=triggerSeal)
                        throw std::runtime_error("Native identity, foreground or seals changed during post-trigger observation.");
                    return {{"ok",true},{"renderer_trigger_click_submitted",true},{"post_trigger_observation_captured",true},{"saved_anchor",anchor},{"provider_preserved",true},{"conversation_preserved",true},{"video_preserved",true},
                        {"browser_input_submitted",false},{"generation_submitted",false},{"send_dispatched",false},{"operator_approval_entered",false}};
                }
                if(browserOpen){if(reinterpret_cast<std::uintptr_t>(GetForegroundWindow())!=row.at("window_id").get<std::uintptr_t>())continue;
                    if(!lmWindow.is_null())throw std::runtime_error("Exact foreground native LM Studio window is ambiguous.");lmWindow=row;lmProcess=rowProcess;continue;}
                const auto scrollButtons=[&](const Json& controls){Json matches=Json::array();for(const auto& item:controls)
                    if(item.at("control_type")==UIA_ButtonControlTypeId && item.at("name")=="Scroll to bottom" && !item.value("name_truncated",false) &&
                        item.value("enabled",false) && !item.value("offscreen",true) && item.at("width").get<int>()>0 && item.at("height").get<int>()>0)matches.push_back(item);return matches;};
                const auto buttons=scrollButtons(elements);
                if(!observeOnly && !openPopover && scrollArgs.is_null() && buttons.size()>1U)throw std::runtime_error("Visible native Scroll to bottom button is ambiguous.");
                if(!observeOnly && !openPopover && (!scrollArgs.is_null() || buttons.size()==1U)){
                    if(viewportSubmitted)throw std::runtime_error("Only one native viewport input is permitted per qualification.");
                    Json args=scrollArgs,button;const auto scroll=!scrollArgs.is_null();
                    if(scroll){if(args.at("x").get<int>()>=row.at("width").get<int>() || args.at("y").get<int>()>=row.at("height").get<int>() || CD::fileFacts(scrollPath,operation)!=scrollSeal)
                        throw std::runtime_error("Observed scroll point is outside the current native window or arguments changed.");}
                    else {button=buttons.at(0);const auto refreshedButtons=scrollButtons(readAll(row));if(refreshedButtons.size()!=1U || refreshedButtons.at(0)!=button)
                        throw std::runtime_error("Exact visible native viewport button changed before click.");
                        args={{"window_id",row.at("window_id")},{"pid",row.at("pid")},{"x",button.at("x").get<int>()+button.at("width").get<int>()/2},
                            {"y",button.at("y").get<int>()+button.at("height").get<int>()/2},{"button","left"}};}
                    verifyConversation();verifyVideo();
                    if(processFacts(row.at("pid").get<DWORD>())!=rowProcess || reinterpret_cast<std::uintptr_t>(GetForegroundWindow())!=row.at("window_id").get<std::uintptr_t>() || observeProvider()!=providerBefore)
                        throw std::runtime_error("Exact native window, foreground or provider changed before viewport input.");
                    const auto inputEvent=scroll?"native_video_viewport_scroll":"native_video_viewport_click";
                    event(inputEvent,{{"stage","before_dispatch"},{"window",row},{"process",rowProcess},{"element",button},{"arguments",args}});
                    const auto viewportReceipt=desktopCall(scroll?"desktop_scroll":"desktop_click",args);event(inputEvent,{{"stage","returned"},{"receipt",viewportReceipt}});
                    if(!viewportReceipt.value("input_submitted",false))throw std::runtime_error("Guarded native viewport input was not submitted.");viewportSubmitted=true;
                    verifyConversation();verifyVideo();
                    const auto providerAfterViewport=observeProvider();event("native_video_provider_after_viewport",providerAfterViewport);
                    if(processFacts(row.at("pid").get<DWORD>())!=rowProcess || reinterpret_cast<std::uintptr_t>(GetForegroundWindow())!=row.at("window_id").get<std::uintptr_t>() || providerAfterViewport!=providerBefore || (scroll && CD::fileFacts(scrollPath,operation)!=scrollSeal))
                        throw std::runtime_error("Exact native window, foreground or provider changed after viewport input.");
                    std::this_thread::sleep_for(std::chrono::milliseconds{500});CD::check(operation);
                    elements=readAll(row);event("native_video_observed_window_after_viewport",{{"window",row},{"process",rowProcess},{"elements",elements}});
                    const auto afterSuffix=suffix+"_after_viewport";capture(row,afterSuffix.c_str());
                }
                if(openPopover){const auto controls=popoverControls(elements);if(!controls.is_null()){
                    if(!hyperlink.is_null())throw std::runtime_error("Visible native URL popover window is ambiguous.");lmWindow=row;hyperlink=controls.at("button");popoverUrl=controls.at("url");lmProcess=rowProcess;}}
                else for(const auto& item:elements)if(item.at("control_type")==UIA_HyperlinkControlTypeId && item.at("name")==label &&
                    !item.value("name_truncated",false) && item.value("enabled",false) && !item.value("offscreen",true) && item.at("width").get<int>()>0 && item.at("height").get<int>()>0){
                    if(!hyperlink.is_null())throw std::runtime_error("Visible native video hyperlink is ambiguous.");lmWindow=row;hyperlink=item;lmProcess=processFacts(row.at("pid").get<DWORD>());}
            }
            if(!triggerArgs.is_null())throw std::runtime_error("Exact observed native renderer-trigger window was not available; no input was sent.");
            if(observeOnly){verifyConversation();verifyVideo();const auto providerAfterObservation=observeProvider();event("native_video_provider_after_observation",providerAfterObservation);
                if(providerAfterObservation!=providerBefore)throw std::runtime_error("Provider changed during read-only native video-link observation.");
                return {{"ok",true},{"observation_only",true},{"observed_windows",observedWindowIndex},{"visible_saved_hyperlink",!hyperlink.is_null()},{"saved_anchor",anchor},
                    {"provider_preserved",true},{"conversation_preserved",true},{"video_preserved",true},{"input_submitted",false},{"generation_submitted",false},{"send_dispatched",false},{"operator_approval_entered",false}};}
            if(!scrollArgs.is_null() && !viewportSubmitted)throw std::runtime_error("Exact observed native scroll window was not available; no viewport input was sent.");
            if(browserOpen && lmWindow.is_null())throw std::runtime_error("Exact native LM Studio window is not foreground; no browser launch was requested.");
            if(!browserOpen && hyperlink.is_null())throw std::runtime_error(openPopover?"Actual saved video URL and unique Open in browser button are not visible in the native LM Studio accessibility tree.":"Actual saved video hyperlink is not visible in the native LM Studio accessibility tree.");
            capture(lmWindow,"native_video_link_before");
            Json clickArgs,launchReceipt;
            if(browserOpen){verifyConversation();verifyVideo();if(processFacts(lmWindow.at("pid").get<DWORD>())!=lmProcess || reinterpret_cast<std::uintptr_t>(GetForegroundWindow())!=lmWindow.at("window_id").get<std::uintptr_t>() || observeProvider()!=providerBefore)
                    throw std::runtime_error("Exact native process, foreground, conversation, artifact or provider changed before browser launch.");
                const Json args{{"url",url}};event("native_video_browser_open",{{"stage","before_dispatch"},{"arguments",args},{"anchor",anchor},{"window",lmWindow},{"process",lmProcess}});
                launchReceipt=desktopCall("browser_open",args);event("native_video_browser_open",{{"stage","returned"},{"receipt",launchReceipt}});
                if(!launchReceipt.value("launch_accepted",false))throw std::runtime_error("Existing native browser_open did not accept the sealed video URL launch.");
            }else{
            event(openPopover?"native_video_popover_open_controls":"native_video_hyperlink",{{"window",lmWindow},{"process",lmProcess},{"element",hyperlink},{"url_element",popoverUrl}});
            verifyConversation();verifyVideo();if(processFacts(lmWindow.at("pid").get<DWORD>())!=lmProcess)throw std::runtime_error("Exact native LM Studio process changed before click.");
            if(openPopover){const auto refreshed=popoverControls(readAll(lmWindow));if(refreshed.is_null() || refreshed.at("button")!=hyperlink || refreshed.at("url")!=popoverUrl)throw std::runtime_error("Exact visible URL popover controls changed before click.");
                verifyConversation();verifyVideo();if(processFacts(lmWindow.at("pid").get<DWORD>())!=lmProcess || reinterpret_cast<std::uintptr_t>(GetForegroundWindow())!=lmWindow.at("window_id").get<std::uintptr_t>() || observeProvider()!=providerBefore)
                    throw std::runtime_error("Exact native popover process, foreground, conversation, artifact or provider changed before click.");}
            else {Json refreshed=Json::array();for(const auto& item:readAll(lmWindow))if(item.at("control_type")==UIA_HyperlinkControlTypeId && item.at("name")==label && item.value("enabled",false) && !item.value("offscreen",true))refreshed.push_back(item);
                if(refreshed.size()!=1U || refreshed.at(0)!=hyperlink)throw std::runtime_error("Exact visible video hyperlink changed before click.");}
            const auto x=hyperlink.at("x").get<int>()+hyperlink.at("width").get<int>()/2,y=hyperlink.at("y").get<int>()+hyperlink.at("height").get<int>()/2;
            clickArgs={{"window_id",lmWindow.at("window_id")},{"pid",lmWindow.at("pid")},{"x",x},{"y",y},{"button","left"}};
            const auto clickEvent=openPopover?"native_video_popover_open_click":"native_video_link_click";
            event(clickEvent,{{"stage","before_dispatch"},{"arguments",clickArgs},{"anchor",anchor}});
            const auto receipt=desktopCall("desktop_click",clickArgs);event(clickEvent,{{"stage","returned"},{"receipt",receipt}});
            if(!receipt.value("input_submitted",false))throw std::runtime_error("Guarded native video link input was not submitted.");
            }
            Json opened,address,browserProcess;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{15};
            while(std::chrono::steady_clock::now()<deadline){verifyConversation();CD::check(operation);const auto foreground=reinterpret_cast<std::uintptr_t>(GetForegroundWindow());const auto windows=desktopCall("desktop_list",Json::object());
                for(const auto& row:windows.at("windows")){if(row.at("window_id").get<std::uintptr_t>()!=foreground || row.at("pid")==lmWindow.at("pid"))continue;
                    const auto facts=processFacts(row.at("pid").get<DWORD>());if(CompareStringOrdinal(native(facts.at("executable").get<std::string>()).c_str(),-1,lmExecutable_.c_str(),-1,TRUE)==CSTR_EQUAL)continue;
                    const auto controls=readAll(row);Json matches=Json::array();
                    for(const auto& item:controls){auto name=item.value("name",std::string{});std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                        if((name.find("address")!=std::string::npos || name.find("location")!=std::string::npos) && item.at("control_type")==UIA_EditControlTypeId && item.value("enabled",false) && !item.value("offscreen",true) && !item.value("text_truncated",false) &&
                        (item.value("text_source",std::string{})=="value_pattern" || item.value("text_source",std::string{})=="text_pattern") &&
                        (item.value("text",std::string{})==url || item.value("text",std::string{})==url.substr(7U)))matches.push_back(item);}
                    event(browserOpen?"native_video_postlaunch_foreground":"native_video_postclick_foreground",{{"window",row},{"process",facts},{"address_candidates",matches}});
                    if(matches.size()>1U)throw std::runtime_error("Observed postclick URL edit is ambiguous.");
                    if(matches.size()==1U){opened=row;address=matches.at(0);browserProcess=facts;break;}}
                if(!opened.is_null())break;std::this_thread::sleep_for(std::chrono::milliseconds{250});}
            const auto providerAfter=observeProvider();event("native_video_provider_after",providerAfter);verifyConversation();verifyVideo();
            if(providerBefore!=providerAfter)throw std::runtime_error("Provider history, queue or ownership changed during native link click.");
            if(opened.is_null())return {{"ok",false},{"state","unknown"},{"native_hyperlink_click_submitted",!browserOpen && !openPopover},{"native_popover_open_click_submitted",!browserOpen && openPopover},{"browser_open_launch_accepted",browserOpen},{"browser_open_receipt",launchReceipt},{"browser_address_verified",false},
                {"reason","No unique visible postdispatch foreground URL edit matched the literal saved video URL; no navigation or address typing was attempted."},{"provider_preserved",true},{"conversation_preserved",true},{"generation_submitted",false}};
            capture(opened,"native_video_opened_browser");event("native_video_observed_browser_address",{{"window",opened},{"process",browserProcess},{"address",address}});
            return {{"ok",true},{"native_hyperlink_click_submitted",!browserOpen && !openPopover},{"native_popover_open_click_submitted",!browserOpen && openPopover},{"browser_open_launch_accepted",browserOpen},{"browser_open_receipt",launchReceipt},{"browser_address_verified",true},{"artifact",artifact},{"saved_anchor",anchor},{"selected_assistant_sha256",assistantSha},
                {"lmstudio_window",lmWindow},{"hyperlink",openPopover?Json{}:hyperlink},{"popover_button",openPopover?hyperlink:Json{}},{"popover_url",popoverUrl},{"click",clickArgs},{"opened_window",opened},{"opened_process",browserProcess},{"address_control",address},
                {"address_comparison",address.at("text")==url?"exact_literal_url":"exact_literal_url_with_http_scheme_hidden"},{"provider_preserved",true},{"conversation_preserved",true},
                {"video_preserved",true},{"generation_submitted",false},{"send_dispatched",false},{"operator_approval_entered",false},{"browser_owned",false},{"browser_closed",false},{"playback_verified",false}};
        }catch(const CD::Failure& failure){throw std::runtime_error(failure.error.code+": "+failure.error.message);}
    }
    Json clearOwnedDraft(const Json& before,const bool inspectOnly=false) {
        const auto expected = required("expected-conversation");
        if (expected != "Forge-Conductor-Windows-Edition/1791597164351.conversation.json" ||
            before.at("conversation_id") != expected || before.at("tools_active").get<bool>())
            throw std::runtime_error("The exact inactive qualification conversation was not verified; no draft was cleared.");
        const auto source = take(issuer_->authorize(*scope_, {pathText(native(required("message"))), std::nullopt,
            FC::Domain::FileAccess::Read,false}, context()));
        const auto bytes = take(files_.read(source,65536U,context()));
        const std::string message{reinterpret_cast<const char*>(bytes.data()),bytes.size()};
        const auto seal = take(hasher_.sha256(std::as_bytes(std::span{message.data(),message.size()}))).value();
        if (seal != "2ce30e8050bde0a189f27d9e5da3c4a732fc56acaac4ac9af3df683767f13257")
            throw std::runtime_error("This fixed qualification action only clears its exact unsent cancellation draft.");
        const auto savedPath = native(before.at("conversation_path").get<std::string>());
        const auto verifySaved = [&] {
            const auto saved = readJson(savedPath);
            if (!inspectOnly && saved.value("clientInput",std::string{}) != message)
                throw std::runtime_error("Saved draft differs from the exact owned cancellation text; no draft was cleared.");
            for (const auto& item : saved.at("messages")) {
                const auto& selected = item.at("versions").at(item.at("currentlySelected").get<std::size_t>());
                if (selected.value("role",std::string{}) != "user") continue;
                for (const auto& part : selected.at("content"))
                    if (part.is_object() && part.value("type",std::string{}) == "text" && part.value("text",std::string{}) == message)
                        throw std::runtime_error("Cancellation text has become a saved user message; no draft was cleared.");
            }
        };
        verifySaved();
        if (!take(IW::WindowsLMStudioChatControl::idle(pathText(lmExecutable_),context())))
            throw std::runtime_error("LM Studio is not idle; no draft was cleared.");
        const auto initialized = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
            throw std::runtime_error("Cannot initialize native qualification UI automation.");
        struct Uninitialize { bool owned; ~Uninitialize(){if(owned)CoUninitialize();} } apartment{SUCCEEDED(initialized)};
        using Microsoft::WRL::ComPtr;
        ComPtr<IUIAutomation> automation;
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation))))
            throw std::runtime_error("Cannot inspect the native LM Studio input.");
        struct Windows { const Fs::path* image; std::vector<HWND> found; bool failed{}; } windows{&lmExecutable_,{},false};
        const auto collect = [](HWND window,LPARAM argument)->BOOL {
            auto& state=*reinterpret_cast<Windows*>(argument);
            if(!IsWindowVisible(window))return TRUE;
            DWORD pid{};GetWindowThreadProcessId(window,&pid);
            const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
            if(!process)return TRUE;
            std::array<wchar_t,32768> image{};DWORD length=static_cast<DWORD>(image.size());
            const auto matches=QueryFullProcessImageNameW(process,0,image.data(),&length) && !_wcsicmp(image.data(),state.image->c_str());
            CloseHandle(process);
            if(matches)try{state.found.push_back(window);}catch(...){state.failed=true;return FALSE;}
            return TRUE;
        };
        if(!EnumWindows(collect,reinterpret_cast<LPARAM>(&windows)) || windows.failed)
            throw std::runtime_error("Cannot enumerate the owned LM Studio windows.");
        VARIANT name{};name.vt=VT_BSTR;name.bstrVal=SysAllocString(L"Chat input");
        if(!name.bstrVal)throw std::runtime_error("Cannot allocate the fixed input selector.");
        ComPtr<IUIAutomationCondition> named,typed,condition;
        const auto nameResult=automation->CreatePropertyCondition(UIA_NamePropertyId,name,&named);VariantClear(&name);
        VARIANT type{};type.vt=VT_I4;type.lVal=UIA_EditControlTypeId;
        if(FAILED(nameResult) || FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&typed)) ||
            FAILED(automation->CreateAndCondition(named.Get(),typed.Get(),&condition)))
            throw std::runtime_error("Cannot build the fixed native input selector.");
        ComPtr<IUIAutomationElement> input;HWND ownedWindow{};DWORD ownedPid{};
        for(const auto window:windows.found) {
            ComPtr<IUIAutomationElement> root;ComPtr<IUIAutomationElementArray> matches;
            if(FAILED(automation->ElementFromHandle(window,&root)) || FAILED(root->FindAll(TreeScope_Descendants,condition.Get(),&matches)))continue;
            int count{};if(FAILED(matches->get_Length(&count)))throw std::runtime_error("Cannot count owned native inputs.");
            if(!count)continue;
            if(count!=1 || input)throw std::runtime_error("The owned native Chat input is ambiguous; no draft was cleared.");
            if(FAILED(matches->GetElement(0,&input)))throw std::runtime_error("Cannot retain the owned native input.");
            ownedWindow=window;GetWindowThreadProcessId(window,&ownedPid);
        }
        if(!input || !ownedWindow)throw std::runtime_error("The owned native Chat input was not found; no draft was cleared.");
        const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,ownedPid);
        if(!process)throw std::runtime_error("Cannot retain the exact owned native process; no draft was cleared.");
        struct CloseProcess {HANDLE handle;~CloseProcess(){CloseHandle(handle);}} processOwner{process};
        std::array<wchar_t,32768> image{};DWORD imageLength=static_cast<DWORD>(image.size());
        FILETIME created{},exited{},kernel{},user{};
        if(!QueryFullProcessImageNameW(process,0,image.data(),&imageLength) || _wcsicmp(image.data(),lmExecutable_.c_str()) ||
            !GetProcessTimes(process,&created,&exited,&kernel,&user) || WaitForSingleObject(process,0U)!=WAIT_TIMEOUT)
            throw std::runtime_error("The retained native process image/lifetime differs; no draft was cleared.");
        const auto creation=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32U)|created.dwLowDateTime;
        ComPtr<IUIAutomationValuePattern> value;
        if(FAILED(input->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(&value))))
            throw std::runtime_error("The owned input has no writable native ValuePattern; no draft was cleared.");
        const auto expectedText=take(IW::Detail::strictUtf8ToUtf16(message));
        const auto currentText=[&] {
            BSTR text{};const auto result=value->get_CurrentValue(&text);
            if(FAILED(result)){SysFreeString(text);throw std::runtime_error("Cannot read the owned native draft.");}
            const std::wstring resultText{text?text:L"",text?static_cast<std::size_t>(SysStringLen(text)):0U};SysFreeString(text);return resultText;
        };
        BOOL readOnly{},enabled{};int inputPid{};DWORD currentPid{};GetWindowThreadProcessId(ownedWindow,&currentPid);
        if(!IsWindow(ownedWindow) || currentPid!=ownedPid || FAILED(input->get_CurrentProcessId(&inputPid)) ||
            inputPid!=static_cast<int>(ownedPid) || FAILED(input->get_CurrentIsEnabled(&enabled)) || !enabled ||
            FAILED(value->get_CurrentIsReadOnly(&readOnly)) || readOnly || (!inspectOnly && currentText()!=expectedText))
            throw std::runtime_error("The exact owned writable input/draft identity changed; no draft was cleared.");
        if(inspectOnly) {
            ComPtr<IUIAutomationTextPattern> textPattern;ComPtr<IUIAutomationTextRange> textRange;
            if(FAILED(input->GetCurrentPatternAs(UIA_TextPatternId,IID_PPV_ARGS(&textPattern))) ||
                FAILED(textPattern->get_DocumentRange(&textRange)))
                throw std::runtime_error("The owned input TextPattern could not be inspected; no draft was changed.");
            BSTR text{};const auto result=textRange->GetText(65537,&text);
            if(FAILED(result)){SysFreeString(text);throw std::runtime_error("The owned input TextPattern text could not be read.");}
            const std::wstring textValue{text?text:L"",text?static_cast<std::size_t>(SysStringLen(text)):0U};SysFreeString(text);
            const auto valueText=currentText();const auto savedText=readJson(savedPath).value("clientInput",std::string{});
            const auto describe=[&](const std::wstring& observed) {
                const auto observedUtf8=take(IW::Detail::strictUtf16ToUtf8(observed));
                Json facts{{"empty",observed.empty()},{"equals_owned_draft",observed==expectedText},{"characters",observed.size()},
                    {"sha256",take(hasher_.sha256(std::as_bytes(std::span{observedUtf8.data(),observedUtf8.size()}))).value()}};
                if(observed.empty() || observed==expectedText)facts["text"]=observedUtf8;
                return facts;
            };
            const auto after=conversation();
            if(after.at("conversation_id")!=expected || after.at("tools_active").get<bool>() ||
                after.at("user_message_evidence")!=before.at("user_message_evidence"))
                throw std::runtime_error("Native conversation changed during read-only draft inspection.");
            return {{"ok",true},{"read_only",true},{"window_pid",ownedPid},{"process_creation_time",creation},
                {"value_pattern",describe(valueText)},{"text_pattern",describe(textValue)},
                {"saved_client_input",describe(take(IW::Detail::strictUtf8ToUtf16(savedText)))},
                {"send_dispatched",false},{"draft_changed",false},{"operator_approval_entered",false}};
        }
        const auto current=conversation();
        if(current.at("conversation_id")!=expected || current.at("tools_active").get<bool>() ||
            current.at("user_message_evidence")!=before.at("user_message_evidence") ||
            !take(IW::WindowsLMStudioChatControl::idle(pathText(lmExecutable_),context())))
            throw std::runtime_error("Native conversation or idle state changed; no draft was cleared.");
        verifySaved();
        GetWindowThreadProcessId(ownedWindow,&currentPid);
        if(currentText()!=expectedText || !IsWindow(ownedWindow) || currentPid!=ownedPid || WaitForSingleObject(process,0U)!=WAIT_TIMEOUT)
            throw std::runtime_error("The native draft/window/process changed before clearing.");
        event("native_owned_draft_clear",{{"stage","before_dispatch"},{"conversation_id",expected},{"draft_sha256",seal},{"bytes",message.size()},{"window_pid",ownedPid},{"process_creation_time",creation}});
        auto empty=SysAllocString(L"");if(!empty)throw std::runtime_error("Cannot allocate the native empty draft value.");
        const auto clearResult=value->SetValue(empty);SysFreeString(empty);
        if(FAILED(clearResult))throw std::runtime_error("Native ValuePattern rejected clearing the owned draft; acknowledgement is unverified.");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{5};
        do {
            if(currentText().empty() && readJson(savedPath).value("clientInput",std::string{}).empty()) {
                event("native_owned_draft_clear",{{"stage","confirmed"},{"conversation_id",expected},{"draft_sha256",seal}});
                return {{"ok",true},{"owned_draft_cleared",true},{"send_dispatched",false},{"operator_approval_entered",false},{"conversation",conversation()}};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }while(std::chrono::steady_clock::now()<deadline);
        throw std::runtime_error("Owned draft clearing was dispatched once but native/saved empty acknowledgement was not observed.");
    }
    Json viewPlayback() {
        try {
            auto operation = context(90U);
            const auto runtime = root_ / L"forge-home" / L"comfy-jobs";
            const auto ownerPath = runtime / L"runtime-owner.json";
            const auto owner = CD::readJson(ownerPath);
            const auto ownerSeal = CD::fileFacts(ownerPath, operation);
            if (configuration_.value.comfyUi.endpoint != "http://127.0.0.1:8188" ||
                owner.at("endpoint") != configuration_.value.comfyUi.endpoint)
                throw std::runtime_error("Playback requires the exact retained loopback provider.");
            const auto requestedJob = required("job"), job = identifier<FC::Domain::OperationId>(requestedJob).value();
            if (job != requestedJob) throw std::runtime_error("Playback job UUID must be canonical.");
            Fs::path receiptPath; std::string receiptProject; unsigned projects{};
            CD::regularParents(runtime);
            for (const auto& entry : Fs::directory_iterator{runtime}) {
                const auto projectName = utf8(entry.path().filename()); const auto parsed = FC::Domain::ProjectId::parse(projectName);
                if (!parsed || parsed.value().value() != projectName || !entry.is_directory()) continue;
                if (++projects > 256U) throw std::runtime_error("Playback receipt lookup exceeds the project inventory bound.");
                const auto candidate = entry.path() / native(job) / L"receipt.json";
                if (!Fs::is_regular_file(candidate)) continue;
                CD::regularParents(candidate);
                if (!receiptPath.empty()) throw std::runtime_error("Playback job exists in more than one isolated project.");
                receiptPath = candidate; receiptProject = projectName;
            }
            if (receiptPath.empty()) throw std::runtime_error("Requested playback job has no actual isolated receipt.");
            const auto receiptSeal = CD::fileFacts(receiptPath, operation), envelope = CD::readJson(receiptPath);
            const auto& receipt = envelope.at("payload");
            const auto hash = [&](const std::string& value) { return take(hasher_.sha256(std::as_bytes(std::span{value.data(),value.size()}))).value(); };
            if (hash(receipt.dump()) != envelope.at("sha256").get<std::string>() || receipt.value("schema_version",0U) != 1U ||
                receipt.value("kind",std::string{}) != "forge_comfy_job" || receipt.at("job_id") != job || receipt.at("project_id") != receiptProject ||
                receipt.value("operation",std::string{}) != "render" || !receipt.value("submission_acknowledged",false) ||
                receipt.value("publication_suppressed",true) || receipt.value("cancellation_requested",true) || receipt.value("remote_state",std::string{}) != "completed" ||
                (receipt.value("state",std::string{}) != "awaiting_preview_approval" && receipt.value("state",std::string{}) != "completed") ||
                hash(receipt.at("graph").dump()) != receipt.at("graph_sha256").get<std::string>() ||
                CD::fileFacts(receiptPath, operation).at("sha256") != receiptSeal.at("sha256"))
                throw std::runtime_error("Playback requires an unchanged versioned receipt with an acknowledged sealed completed prompt.");
            const auto promptId = identifier<FC::Domain::OperationId>(receipt.at("prompt_id").get<std::string>()).value();
            const auto stage = receipt.at("stage").get<std::string>();
            if (stage != "preview" && stage != "final") throw std::runtime_error("Playback receipt has no render stage.");
            Json videos = Json::array();
            for (const auto& artifact : receipt.at(stage == "preview" ? "preview_artifacts" : "artifacts"))
                if (artifact.value("media_type",std::string{}) == "video/mp4") videos.push_back(artifact);
            if (videos.size() != 1U) throw std::runtime_error("Playback job must contain exactly one published MP4 video.");
            const auto artifact = videos.at(0), descriptor = artifact.at("descriptor"), metadata = artifact.at("metadata");
            auto folder = descriptor.at("subfolder").get<std::string>(); std::replace(folder.begin(),folder.end(),'\\','/');
            const auto filename = descriptor.at("filename").get<std::string>();
            if (descriptor.at("type") != "output" || folder != "ForgeConductor/" + receiptProject + "/" + job + "/" + stage ||
                filename.empty() || filename.find_first_of("\\/") != std::string::npos)
                throw std::runtime_error("Playback descriptor is outside the exact project/job/render namespace.");
            const auto url = configuration_.value.comfyUi.endpoint + "/view?filename=" + CD::encode(descriptor.at("filename").get<std::string>()) +
                "&subfolder=" + CD::encode(descriptor.at("subfolder").get<std::string>()) + "&type=output";
            const auto videoSha = identifier<FC::Domain::Sha256Digest>(artifact.at("sha256").get<std::string>()).value();
            const auto videoBytes = artifact.at("bytes").get<std::uint64_t>();
            const auto width = metadata.at("width").get<unsigned>(), height = metadata.at("height").get<unsigned>();
            const auto duration = metadata.at("duration").get<double>();
            if (artifact.at("provider_view_url") != url || !metadata.value("decoded",false) || metadata.value("frame_count",0.0) < 2.0 ||
                !std::isfinite(duration) || duration <= 0.0 || !width || !height || width > 32768U || height > 32768U ||
                videoBytes < 1024U || videoBytes > 1024ULL * 1024ULL * 1024ULL)
                throw std::runtime_error("Playback receipt does not contain bounded measured decoded-video facts.");
            const auto source = take(issuer_->authorize(*scope_,{pathText(native(artifact.at("path").get<std::string>())),std::nullopt,FC::Domain::FileAccess::Read,false},operation));
            const auto published = native(source.canonicalPath().value()); CD::regularParents(published);
            CD::Handle sourceFile{CreateFileW(published.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
            if (!sourceFile) throw std::runtime_error("Cannot retain the exact published video for playback verification.");
            const auto sourceSeal = CD::fileFacts(sourceFile.get(),operation);
            if (sourceSeal.at("sha256") != videoSha || sourceSeal.at("bytes") != videoBytes)
                throw std::runtime_error("Published MP4 differs from its actual receipt content seal.");
            std::array<std::byte,1024> prefix{}; DWORD prefixBytes{}; LARGE_INTEGER zero{};
            if (!SetFilePointerEx(sourceFile.get(),zero,nullptr,FILE_BEGIN) || !ReadFile(sourceFile.get(),prefix.data(),static_cast<DWORD>(prefix.size()),&prefixBytes,nullptr) || prefixBytes != prefix.size())
                throw std::runtime_error("Cannot read the sealed published video's range prefix.");
            const auto prefixSha = take(hasher_.sha256(std::span<const std::byte>{prefix})).value();
            const auto foregroundBefore = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
            event("playback_receipt_binding",{{"job_id",job},{"project_id",receiptProject},{"revision",receipt.at("revision")},{"prompt_id",promptId},
                {"graph_sha256",receipt.at("graph_sha256")},{"receipt_path",utf8(receiptPath)},{"receipt_seal",receiptSeal},{"video_path",utf8(published)},
                {"video_sha256",videoSha},{"video_bytes",videoBytes},{"descriptor",descriptor},{"provider_view_url",url},
                {"measured_metadata",{{"decoded",true},{"width",width},{"height",height},{"duration",duration},{"frame_count",metadata.at("frame_count")}}},{"prefix_sha256",prefixSha},{"foreground_before",foregroundBefore}});

            const auto listener = [](unsigned port, DWORD pid) {
                DWORD bytes{};
                if (GetExtendedTcpTable(nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
                    return false;
                std::vector<std::byte> storage(bytes);
                if (GetExtendedTcpTable(storage.data(), &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return false;
                const auto table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
                for (DWORD index = 0U; index < table->dwNumEntries; ++index) {
                    const auto& row = table->table[index];
                    if (row.dwOwningPid == pid && ntohs(static_cast<u_short>(row.dwLocalPort)) == port &&
                        row.dwLocalAddr == htonl(INADDR_LOOPBACK)) return true;
                }
                return false;
            };
            CD::Handle provider{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, owner.at("pid").get<DWORD>())};
            const auto verifyProvider = [&] {
                if (!provider || WaitForSingleObject(provider.get(), 0U) != WAIT_TIMEOUT)
                    throw std::runtime_error("Exact provider is no longer running.");
                FILETIME created{}, exited{}, kernel{}, user{};
                std::array<wchar_t, 32768> image{}; DWORD length = static_cast<DWORD>(image.size());
                if (!GetProcessTimes(provider.get(), &created, &exited, &kernel, &user) ||
                    !QueryFullProcessImageNameW(provider.get(), 0U, image.data(), &length))
                    throw std::runtime_error("Cannot verify retained provider process identity.");
                const auto creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
                if (creation != owner.at("creation_time").get<std::uint64_t>() ||
                    CompareStringOrdinal(image.data(), static_cast<int>(length), native(owner.at("python").get<std::string>()).c_str(), -1, TRUE) != CSTR_EQUAL ||
                    !listener(8188U, owner.at("pid").get<DWORD>()) || CD::fileFacts(ownerPath, operation).at("sha256") != ownerSeal.at("sha256") ||
                    CD::fileFacts(receiptPath, operation).at("sha256") != receiptSeal.at("sha256"))
                    throw std::runtime_error("Provider image, creation identity, loopback listener, ownership ledger or actual receipt changed.");
            };
            const auto runtimeLogFacts = [&] {
                const auto path = runtime / L"runtime.log"; CD::regularParents(path);
                CD::Handle log{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ | FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
                    FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
                LARGE_INTEGER bytes{};
                if (!log || !GetFileSizeEx(log.get(),&bytes) || bytes.QuadPart < 0 || bytes.QuadPart > 64LL * 1024LL * 1024LL)
                    throw std::runtime_error("Shared provider log is unavailable or exceeds the 64 MiB observation bound.");
                auto facts = CD::fileFacts(log.get(),operation); facts["path"] = utf8(path); return facts;
            };
            const auto observeProvider = [&] {
                verifyProvider();
                const auto queue = CD::http(configuration_.value.comfyUi.endpoint + "/queue", "GET", "", "", 16U * 1024U * 1024U, operation);
                const auto history = CD::http(configuration_.value.comfyUi.endpoint + "/history", "GET", "", "", 16U * 1024U * 1024U, operation);
                if (queue.status != 200U || history.status != 200U) throw std::runtime_error("Read-only exact provider observation failed.");
                const auto parsedQueue = CD::parse(queue.body), parsedHistory = CD::parse(history.body);
                const auto& exactHistory = parsedHistory.at(promptId), prompt = exactHistory.at("prompt");
                const auto& descriptors = exactHistory.at("outputs").at(artifact.at("node_id").get<std::string>()).at(artifact.at("history_key").get<std::string>());
                if (!parsedQueue.at("queue_running").empty() || !parsedQueue.at("queue_pending").empty() || !prompt.is_array() || prompt.size() < 3U || prompt[1] != promptId ||
                    !CD::promptGraphMatches(receipt.at("graph"),prompt[2]) || !exactHistory.at("status").at("completed").get<bool>() ||
                    exactHistory.at("status").value("status_str",std::string{}) != "success" || !descriptors.is_array() ||
                    std::count(descriptors.begin(),descriptors.end(),descriptor) != 1)
                    throw std::runtime_error("Playback requires an idle provider and its exact completed owned descriptor.");
                return Json{{"queue",parsedQueue},{"history",parsedHistory},{"provider_pid",owner.at("pid")},
                    {"provider_creation_filetime",owner.at("creation_time")},{"ownership_ledger_sha256",ownerSeal.at("sha256")},
                    {"receipt_sha256",receiptSeal.at("sha256")},{"runtime_log",runtimeLogFacts()}};
            };
            const auto before = observeProvider(); event("playback_provider_before", before);
            const auto streamedPath = root_ / L"evidence" / native(runId_ + ".view.mp4");
            CD::Handle output{CreateFileW(streamedPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (!output) throw std::runtime_error("Cannot create private streamed playback evidence.");
            const auto transferred = CD::http(url, "GET", "", "", 64U * 1024U, operation, output.get(), videoBytes);
            output.reset();
            const auto streamedSeal = CD::fileFacts(streamedPath, operation);
            if (transferred.status != 200U || transferred.contentType != "video/mp4" ||
                streamedSeal.at("bytes") != videoBytes || streamedSeal.at("sha256") != videoSha)
                throw std::runtime_error("Actual view response did not stream the exact sealed MP4.");
            event("playback_view_stream", {{"path",utf8(streamedPath)},{"facts",streamedSeal},{"http_status",transferred.status},{"content_type",transferred.contentType}});

            Fs::path edge;
            for (const auto variable : {L"ProgramFiles(x86)", L"ProgramFiles", L"LOCALAPPDATA"}) {
                std::array<wchar_t, 32768> value{};
                const auto length = GetEnvironmentVariableW(variable, value.data(), static_cast<DWORD>(value.size()));
                if (!length || length >= value.size()) continue;
                const auto candidate = Fs::path{value.data()} / L"Microsoft" / L"Edge" / L"Application" / L"msedge.exe";
                if (Fs::is_regular_file(candidate)) { edge = candidate; break; }
            }
            if (edge.empty()) throw std::runtime_error("Installed Microsoft Edge was not found.");
            CD::regularParents(edge);
            const auto profile = root_ / L"private" / native("edge-video-playback-" + runId_);
            if (!Fs::create_directory(profile)) throw std::runtime_error("Playback needs a new isolated Edge profile.");
            auto browser = CD::startProcess(edge, {"--headless=new","--edge-skip-compat-layer-relaunch","--no-first-run",
                "--no-default-browser-check","--disable-background-networking","--remote-debugging-address=127.0.0.1",
                "--remote-debugging-port=0","--user-data-dir=" + utf8(profile),"about:blank"}, profile,
                root_ / L"evidence" / native(runId_ + ".edge-playback.log"), true);
            event("playback_owned_browser", {{"pid",browser.pid},{"creation_filetime",browser.creationTime},{"executable",utf8(edge)},
                {"image_facts",CD::fileFacts(edge,operation)},{"private_profile",utf8(profile)},{"transient_job",true}});
            const auto browserAlive = [&] {
                CD::check(operation);
                if (WaitForSingleObject(browser.process.get(), 0U) != WAIT_TIMEOUT) throw std::runtime_error("Owned playback Edge exited.");
            };
            std::string port;
            while (port.empty()) {
                browserAlive(); const auto discovery = profile / L"DevToolsActivePort";
                if (Fs::is_regular_file(discovery)) { CD::regularParents(discovery); std::ifstream stream{discovery}; std::getline(stream,port);
                    if (port.empty() || port.size() > 5U || port.find_first_not_of("0123456789") != std::string::npos) port.clear(); }
                if (port.empty()) std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
            const auto portNumber = static_cast<unsigned>(std::stoul(port));
            if (!portNumber || portNumber > 65535U || !listener(portNumber,browser.pid))
                throw std::runtime_error("Private Edge debugging listener is not owned by the retained browser.");
            const auto debugOrigin = "http://127.0.0.1:" + port;
            std::string socket;
            while (socket.empty()) {
                browserAlive();
                if (!listener(portNumber,browser.pid)) throw std::runtime_error("Owned Edge debugger identity changed.");
                const auto targets = CD::http(debugOrigin + "/json/list", "GET", "", "", 1024U * 1024U, operation);
                if (targets.status != 200U) throw std::runtime_error("Private Edge target discovery failed.");
                for (const auto& target : CD::parse(targets.body))
                    if (target.value("type",std::string{}) == "page" && target.value("url",std::string{}) == "about:blank") {
                        if (!socket.empty()) throw std::runtime_error("Private Edge exposed more than one blank playback target.");
                        socket = target.value("webSocketDebuggerUrl",std::string{});
                    }
                if (socket.empty()) std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
            if (!socket.starts_with("ws://127.0.0.1:" + port + "/devtools/page/"))
                throw std::runtime_error("Private Edge page socket is outside its verified debugging origin.");
            Json playback;
            {
                CD::Cdp cdp{socket,operation}; static_cast<void>(cdp.call("Page.enable",Json::object(),operation));
                const auto navigation = cdp.call("Page.navigate",{{"url",url}},operation); event("playback_navigation",navigation);
                if (navigation.contains("errorText")) throw std::runtime_error("Exact video navigation failed: " + navigation.at("errorText").get<std::string>());
                Json window;
                for (;;) {
                    browserAlive();
                    const auto state = cdp.call("Runtime.evaluate",{{"expression","({href:location.href,ready:!!document.querySelector('video')})"},{"returnByValue",true}},operation);
                    const auto value = state.value("result",Json::object()).value("value",Json::object());
                    if (value.value("href",std::string{}) == url && value.value("ready",false)) {
                        window = cdp.call("Runtime.evaluate",{{"expression","window"},{"returnByValue",false}},operation).at("result"); break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{50});
                }
                constexpr auto playbackFunction = R"js(async function(expected) {
                    if (location.href !== expected.url || location.origin !== 'http://127.0.0.1:8188') throw new Error('Playback origin/URL changed');
                    const video = document.querySelector('video'); if (!video) throw new Error('No built-in video element');
                    const events = []; const handlers = [];
                    for (const name of ['loadedmetadata','loadeddata','play','playing','pause','seeking','seeked','ended','error']) {
                        const handler = () => { if (events.length < 32) events.push({event:name,currentTime:video.currentTime,readyState:video.readyState}); };
                        video.addEventListener(name,handler); handlers.push([name,handler]);
                    }
                    const facts = () => ({currentSrc:video.currentSrc,duration:video.duration,videoWidth:video.videoWidth,videoHeight:video.videoHeight,
                        readyState:video.readyState,networkState:video.networkState,currentTime:video.currentTime,paused:video.paused,ended:video.ended,
                        decodedFrames:video.getVideoPlaybackQuality?.().totalVideoFrames ?? null,error:video.error ? {code:video.error.code,message:video.error.message} : null});
                    const waitFor = async (test,label,ms=10000) => { const deadline=performance.now()+ms;
                        while (!test()) { if (video.error) throw new Error('Media error '+video.error.code+': '+video.error.message);
                            if (performance.now()>=deadline) throw new Error(label+' timed out'); await new Promise(resolve=>setTimeout(resolve,25)); } };
                    const range = async () => { const response=await fetch(expected.url,{headers:{Range:'bytes=0-1023'},cache:'no-store'});
                        const bytes=await response.arrayBuffer(); const sha=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),value=>value.toString(16).padStart(2,'0')).join('');
                        const result={status:response.status,contentType:response.headers.get('content-type'),contentRange:response.headers.get('content-range'),bytes:bytes.byteLength,sha256:sha};
                        if (result.status!==206 || result.contentType!=='video/mp4' || result.contentRange!=='bytes 0-1023/'+expected.bytes || result.bytes!==1024 || sha!==expected.prefixSha) throw new Error('Exact sealed browser range response differs: '+JSON.stringify(result));
                        return result; };
                    const result={ok:false,events};
                    try {
                        await waitFor(()=>video.readyState>=2 && Number.isFinite(video.duration),'Metadata/readiness');
                        result.metadata=facts(); result.range_before=await range();
                        if (video.currentSrc!==expected.url || video.videoWidth!==expected.width || video.videoHeight!==expected.height || Math.abs(video.duration-expected.duration)>0.02) throw new Error('Decoded browser metadata differs from sealed FFprobe facts');
                        video.pause(); video.muted=true; video.currentTime=0;
                        await waitFor(()=>!video.seeking && video.currentTime<0.05,'Initial seek'); result.before_play=facts();
                        await video.play(); await waitFor(()=>video.currentTime>Math.min(0.15,video.duration*0.1),'Playback progression'); result.during_play=facts(); video.pause();
                        const target=video.duration*0.65; video.currentTime=target;
                        await waitFor(()=>!video.seeking && Math.abs(video.currentTime-target)<0.08,'Seek'); result.seek_target=target; result.after_seek=facts();
                        await video.play(); await waitFor(()=>video.currentTime>target+Math.min(0.1,video.duration*0.05),'Playback after seek'); result.after_seek_play=facts(); video.pause();
                        if (!(result.after_seek_play.decodedFrames>0)) throw new Error('Browser reported no decoded video frames');
                        result.range_after=await range(); result.final=facts(); result.ok=true;
                    } catch (error) { result.browser_error=String(error); result.final=facts(); }
                    finally { for(const [name,handler] of handlers) video.removeEventListener(name,handler); video.pause(); }
                    return result;
                })js";
                const auto evaluated = cdp.call("Runtime.callFunctionOn",{{"objectId",window.at("objectId")},{"functionDeclaration",playbackFunction},
                    {"arguments",Json::array({Json{{"value",Json{{"url",url},{"prefixSha",prefixSha},{"bytes",videoBytes},{"width",width},{"height",height},{"duration",duration}}}}})},
                    {"awaitPromise",true},{"returnByValue",true},{"userGesture",true}},operation);
                event("playback_browser_response",evaluated);
                if (evaluated.contains("exceptionDetails")) throw std::runtime_error("Private browser playback evaluation failed: " + evaluated.at("exceptionDetails").dump());
                playback = evaluated.at("result").at("value");
            }
            const auto after = observeProvider(); event("playback_provider_after",after);
            const auto foregroundAfter = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
            if (before != after || CD::fileFacts(sourceFile.get(),operation).at("sha256") != sourceSeal.at("sha256") || foregroundAfter != foregroundBefore)
                throw std::runtime_error("Provider history/queue/log, ownership, actual receipt, exact video or foreground changed during playback observation.");
            if (!TerminateJobObject(browser.job.get(),0U) || WaitForSingleObject(browser.process.get(),5000U) != WAIT_OBJECT_0)
                throw std::runtime_error("Cannot confirm cleanup of the exact transient playback browser.");
            playback["provider_identity_and_history_preserved"] = true; playback["owned_browser_cleanup_verified"] = true;
            playback["job_id"] = job; playback["prompt_id"] = promptId; playback["graph_sha256"] = receipt.at("graph_sha256");
            playback["receipt_path"] = utf8(receiptPath); playback["receipt_sha256"] = receiptSeal.at("sha256");
            playback["foreground_before"] = foregroundBefore; playback["foreground_after"] = foregroundAfter; playback["foreground_preserved"] = true;
            playback["video_sha256"] = videoSha; playback["video_bytes"] = videoBytes; playback["private_profile"] = utf8(profile);
            playback["browser_control"] = "Native CDP with a fixed qualification function and explicit user gesture; no LM Studio link click was exercised.";
            playback["provider_view_url"] = url; playback["native_chat_playback_observed"] = false;
            playback["operator_quality_accepted"] = false; playback["generation_submitted"] = false;
            return playback;
        } catch (const CD::Failure& failure) {
            throw std::runtime_error(failure.error.code + ": " + failure.error.message);
        }
    }
    Json desktopComfyInput() {
        try {
        const auto controls = native(required("arguments"));
        if (!controls.is_absolute() || !CD::contained(root_ / L"workspace", controls) || Fs::exists(controls))
            throw std::runtime_error("Desktop qualification requires a fresh control-file path within the isolated workspace.");
        CD::regularParents(controls);
        auto operation = context(300U);
        const auto runtime = root_ / L"forge-home" / L"comfy-jobs";
        const auto ownerPath = runtime / L"runtime-owner.json";
        const auto owner = CD::readJson(ownerPath), ownerSeal = CD::fileFacts(ownerPath,operation);
        if (configuration_.value.comfyUi.endpoint != "http://127.0.0.1:8188" || owner.at("endpoint") != configuration_.value.comfyUi.endpoint)
            throw std::runtime_error("Desktop qualification only observes this isolated runtime's owned loopback provider.");
        const auto listener = [](unsigned port, DWORD pid) {
            DWORD bytes{};
            if (GetExtendedTcpTable(nullptr,&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0) != ERROR_INSUFFICIENT_BUFFER) return false;
            std::vector<std::byte> storage(bytes);
            if (GetExtendedTcpTable(storage.data(),&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0) != NO_ERROR) return false;
            const auto table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
            for (DWORD index=0;index<table->dwNumEntries;++index) {
                const auto& row=table->table[index];
                if (row.dwOwningPid==pid && ntohs(static_cast<u_short>(row.dwLocalPort))==port && row.dwLocalAddr==htonl(INADDR_LOOPBACK)) return true;
            }
            return false;
        };
        CD::Handle provider{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,owner.at("pid").get<DWORD>())};
        const auto providerImage = native(owner.at("python").get<std::string>());
        const auto providerImageSeal = CD::fileFacts(providerImage,operation);
        const auto verifyProvider = [&] {
            CD::check(operation);
            FILETIME created{},exited{},kernel{},user{}; std::array<wchar_t,32768> image{}; DWORD length=static_cast<DWORD>(image.size());
            if (!provider || WaitForSingleObject(provider.get(),0)!=WAIT_TIMEOUT || !GetProcessTimes(provider.get(),&created,&exited,&kernel,&user) ||
                !QueryFullProcessImageNameW(provider.get(),0,image.data(),&length)) throw std::runtime_error("Exact provider is unavailable.");
            const auto creation=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32U)|created.dwLowDateTime;
            if (creation!=owner.at("creation_time").get<std::uint64_t>() ||
                CompareStringOrdinal(image.data(),static_cast<int>(length),providerImage.c_str(),-1,TRUE)!=CSTR_EQUAL ||
                !listener(8188U,owner.at("pid").get<DWORD>()) || CD::fileFacts(ownerPath,operation).at("sha256")!=ownerSeal.at("sha256") ||
                CD::fileFacts(providerImage,operation).at("sha256")!=providerImageSeal.at("sha256"))
                throw std::runtime_error("Provider executable, creation identity, listener or ownership ledger changed.");
        };
        const auto observeProvider = [&] {
            verifyProvider();
            const auto queue=CD::http(configuration_.value.comfyUi.endpoint+"/queue","GET","","",16U*1024U*1024U,operation);
            const auto history=CD::http(configuration_.value.comfyUi.endpoint+"/history","GET","","",16U*1024U*1024U,operation);
            if (queue.status!=200U || history.status!=200U) throw std::runtime_error("Provider queue/history observation failed.");
            const auto q=CD::parse(queue.body);
            if (!q.at("queue_running").empty() || !q.at("queue_pending").empty()) throw std::runtime_error("Desktop qualification requires an idle provider.");
            return Json{{"queue",q},{"history",CD::parse(history.body)},{"pid",owner.at("pid")},
                {"creation_time",owner.at("creation_time")},{"owner_sha256",ownerSeal.at("sha256")},{"executable_sha256",providerImageSeal.at("sha256")}};
        };
        const auto beforeProvider=observeProvider(); event("desktop_provider_before",beforeProvider);
        // This checks the same owner Execute grant consumed by guarded desktop input.
        take(issuer_->authorize(*scope_,{scope_->trustedRoots().front(),std::nullopt,FC::Domain::FileAccess::Execute,false},operation));
        Fs::path edge;
        for (const auto variable : {L"ProgramFiles(x86)",L"ProgramFiles",L"LOCALAPPDATA"}) {
            std::array<wchar_t,32768> value{}; const auto length=GetEnvironmentVariableW(variable,value.data(),static_cast<DWORD>(value.size()));
            if (!length || length>=value.size()) continue;
            const auto candidate=Fs::path{value.data()}/L"Microsoft"/L"Edge"/L"Application"/L"msedge.exe";
            if (Fs::is_regular_file(candidate)) { edge=candidate; break; }
        }
        if (edge.empty()) throw std::runtime_error("Installed Edge was not found.");
        CD::regularParents(edge);
        const auto profile=root_/L"private"/native("edge-desktop-input-"+runId_);
        if (!Fs::create_directory(profile)) throw std::runtime_error("A new owned Edge profile is required.");
        auto browser=CD::startProcess(edge,{"--edge-skip-compat-layer-relaunch","--no-first-run","--no-default-browser-check",
            "--disable-background-networking","--remote-debugging-address=127.0.0.1","--remote-debugging-port=0",
            "--user-data-dir="+utf8(profile),"--new-window","about:blank"},profile,root_/L"evidence"/native(runId_+".edge-desktop.log"),true);
        bool cleaned=false;
        const auto cleanup = [&] {
            if (cleaned) return;
            if (!TerminateJobObject(browser.job.get(),0U) || WaitForSingleObject(browser.process.get(),5000U)!=WAIT_OBJECT_0)
                throw std::runtime_error("Exact owned Edge job cleanup could not be confirmed.");
            browser.process.reset(); browser.job.reset();
            if (!CD::contained(root_/L"private",profile) || profile.filename()!=native("edge-desktop-input-"+runId_))
                throw std::runtime_error("Owned profile cleanup boundary changed.");
            CD::regularParents(profile); Fs::remove_all(profile);
            if (Fs::exists(profile)) throw std::runtime_error("Owned Edge profile remains after cleanup.");
            cleaned=true; event("desktop_owned_browser_cleanup",{{"pid",browser.pid},{"job_terminated",true},{"profile_removed",true}});
        };
        try {
            const auto browserAlive = [&] { CD::check(operation); if (WaitForSingleObject(browser.process.get(),0U)!=WAIT_TIMEOUT) throw std::runtime_error("Owned Edge exited."); };
            event("desktop_owned_browser",{{"pid",browser.pid},{"creation_time",browser.creationTime},{"private_profile",utf8(profile)},
                {"executable",utf8(edge)},{"executable_facts",CD::fileFacts(edge,operation)},{"transient_job",true}});
            std::string port;
            while (port.empty()) {
                browserAlive(); const auto discovery=profile/L"DevToolsActivePort";
                if (Fs::is_regular_file(discovery)) { CD::regularParents(discovery); std::ifstream stream{discovery}; std::getline(stream,port);
                    if (port.empty() || port.size()>5U || port.find_first_not_of("0123456789")!=std::string::npos) port.clear(); }
                if (port.empty()) std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
            const auto portNumber=static_cast<unsigned>(std::stoul(port));
            if (!portNumber || portNumber>65535U || !listener(portNumber,browser.pid)) throw std::runtime_error("Debugger listener is not owned by exact Edge.");
            const auto targets=CD::http("http://127.0.0.1:"+port+"/json/list","GET","","",1024U*1024U,operation);
            if (targets.status!=200U) throw std::runtime_error("Owned debugger discovery failed.");
            std::string socket;
            for (const auto& target : CD::parse(targets.body)) if (target.value("type",std::string{})=="page" && target.value("url",std::string{})=="about:blank") {
                if (!socket.empty()) throw std::runtime_error("More than one owned blank target."); socket=target.value("webSocketDebuggerUrl",std::string{});
            }
            if (!socket.starts_with("ws://127.0.0.1:"+port+"/devtools/page/")) throw std::runtime_error("Owned page debugger origin differs.");
            Json result;
            {
                CD::Cdp cdp{socket,operation}; static_cast<void>(cdp.call("Page.enable",Json::object(),operation));
                const auto navigation=cdp.call("Page.navigate",{{"url",configuration_.value.comfyUi.endpoint+"/"}},operation);
                if (navigation.contains("errorText")) throw std::runtime_error("Owned Comfy page navigation failed.");
                Json app;
                for (;;) {
                    browserAlive();
                    const auto ready=cdp.call("Runtime.evaluate",{{"expression","(() => { const app=window.comfyAPI?.app?.app; return app?.canvas?.canvas && (app.rootGraph || app.graph) && typeof app.loadGraphData==='function' && typeof app.graphToPrompt==='function' ? app : undefined; })()"},{"returnByValue",false}},operation);
                    if (ready.at("result").contains("objectId")) { app=ready.at("result"); break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds{50});
                }
                const auto fixedCall = [&](const char* function,const Json& args=Json::array()) {
                    browserAlive(); if (!listener(portNumber,browser.pid)) throw std::runtime_error("Owned debugger listener changed.");
                    const auto reply=cdp.call("Runtime.callFunctionOn",{{"objectId",app.at("objectId")},{"functionDeclaration",function},
                        {"arguments",args},{"awaitPromise",true},{"returnByValue",true}},operation);
                    if (reply.contains("exceptionDetails")) throw std::runtime_error("Fixed canvas observation failed: "+reply.at("exceptionDetails").dump());
                    return reply.at("result").at("value");
                };
                event("desktop_empty_private_graph",fixedCall(R"js(async function() {
                    if(location.origin!=='http://127.0.0.1:8188') throw new Error('Origin changed');
                    await this.loadGraphData({last_node_id:0,last_link_id:0,nodes:[],links:[],groups:[],config:{},extra:{ds:{scale:1,offset:[0,0]}},version:0.4},true,true);
                    const canvas=this.canvas.canvas, events=[];
                    window.__forgeDesktopInputEvents=events;
                    for(const name of ['wheel','pointerdown','pointermove','pointerup']) canvas.addEventListener(name,e=>{
                        if(events.length<128) events.push({type:e.type,isTrusted:e.isTrusted,clientX:e.clientX,clientY:e.clientY,
                            buttons:e.buttons,deltaX:e.deltaX??null,deltaY:e.deltaY??null});
                    },true);
                    return {private_empty_graph_loaded:true};
                })js"));
                constexpr auto observationFunction=R"js(async function() {
                    if(location.origin!=='http://127.0.0.1:8188') throw new Error('Origin changed');
                    const graph=this.rootGraph||this.graph, canvas=this.canvas.canvas, r=canvas.getBoundingClientRect(), serialized=graph.serialize();
                    if(serialized.nodes.length || serialized.groups.length || (Array.isArray(serialized.links)?serialized.links.length:Object.keys(serialized.links||{}).length)) throw new Error('Private graph is no longer empty');
                    if(serialized.extra) delete serialized.extra.ds;
                    const prompt=await this.graphToPrompt(); if(Object.keys(prompt.output).length) throw new Error('Empty canvas unexpectedly has API nodes');
                    return {graph:serialized,api:prompt.output,ds:{scale:this.canvas.ds.scale,offset:Array.from(this.canvas.ds.offset)},
                        canvas_rect:{x:r.x,y:r.y,width:r.width,height:r.height},viewport:{width:innerWidth,height:innerHeight,device_pixel_ratio:devicePixelRatio},
                        events:window.__forgeDesktopInputEvents||[]};
                })js";
                NW::WindowsDesktopArtifactService desktop{*issuer_,files_};
                const auto desktopCall=[&](std::string_view name,const Json& args) { return Json::parse(take(desktop.execute(name,args.dump(),*scope_,context(15U)))); };
                // The generic process launcher initially hides its window; show only this newly owned window, without forcing foreground.
                struct OwnedWindow { DWORD pid; HWND window{}; } owned{browser.pid};
                while (!owned.window) {
                    browserAlive();
                    EnumWindows([](HWND window,LPARAM data)->BOOL { auto& state=*reinterpret_cast<OwnedWindow*>(data); DWORD pid{};
                        GetWindowThreadProcessId(window,&pid); std::array<wchar_t,64> type{}; GetClassNameW(window,type.data(),static_cast<int>(type.size()));
                        if(pid==state.pid && std::wstring_view{type.data()}==L"Chrome_WidgetWin_1") { state.window=window; return FALSE; } return TRUE;
                    },reinterpret_cast<LPARAM>(&owned));
                    if (!owned.window) std::this_thread::sleep_for(std::chrono::milliseconds{50});
                }
                ShowWindowAsync(owned.window,SW_SHOWNORMAL);
                Json selected;
                while (selected.is_null()) {
                    browserAlive();
                    const auto inventory=desktopCall("desktop_list",Json::object());
                    for (const auto& row : inventory.at("windows"))
                        if (row.at("pid")==browser.pid && row.at("window_id").get<std::uintptr_t>()==reinterpret_cast<std::uintptr_t>(owned.window)) selected=row;
                    if (selected.is_null()) std::this_thread::sleep_for(std::chrono::milliseconds{50});
                }
                const auto base=fixedCall(observationFunction); event("desktop_canvas_before",base);
                RECT windowRect{},clientRect{}; POINT clientOrigin{};
                if (!GetWindowRect(owned.window,&windowRect) || !GetClientRect(owned.window,&clientRect) || !ClientToScreen(owned.window,&clientOrigin))
                    throw std::runtime_error("Owned window geometry unavailable.");
                const auto ratio=static_cast<double>(clientRect.right-clientRect.left)/base.at("viewport").at("width").get<double>();
                const auto pageX=clientOrigin.x-windowRect.left;
                const auto pageY=clientOrigin.y-windowRect.top+(clientRect.bottom-clientRect.top)-base.at("viewport").at("height").get<double>()*ratio;
                const auto& rect=base.at("canvas_rect");
                const Json canvasRect{{"x",pageX+rect.at("x").get<double>()*ratio},{"y",pageY+rect.at("y").get<double>()*ratio},
                    {"width",rect.at("width").get<double>()*ratio},{"height",rect.at("height").get<double>()*ratio}};
                const auto capture = [&](std::string_view label) {
                    auto args=Json{{"window_id",selected.at("window_id")},{"pid",browser.pid},
                        {"path",utf8(root_/L"workspace"/native(runId_+"."+std::string{label}+".png"))},{"preview_max_dimension",256U}};
                    const auto image=desktopCall("desktop_capture",args), facts=CD::fileFacts(native(image.at("path").get<std::string>()),operation);
                    auto compact=image; compact.erase("preview_png_base64"); compact.erase("preview_base64");
                    event(label,{{"capture",compact},{"seal",facts}}); return facts;
                };
                const auto firstCapture=capture("desktop_before_input");
                const Json ready{{"run_id",runId_},{"window",selected},{"canvas_window_rect",canvasRect},{"canvas",base},
                    {"capture",firstCapture},{"control_file",utf8(controls)},{"evidence_path",utf8(evidencePath_)},
                    {"mapping","Native client width / observed CSS viewport width; client bottom minus observed viewport height establishes page origin."}};
                event("desktop_input_review_gate",ready);
                std::cout << "DESKTOP_COMFY_READY " << ready.dump() << '\n' << std::flush;
                const auto reviewDeadline=std::chrono::steady_clock::now()+std::chrono::seconds{120};
                while (!Fs::exists(controls)) { browserAlive(); if (std::chrono::steady_clock::now()>=reviewDeadline) throw std::runtime_error("Observed screenshot control-file gate timed out without input.");
                    std::this_thread::sleep_for(std::chrono::milliseconds{100}); }
                const auto authorized=take(issuer_->authorize(*scope_,{pathText(controls),std::nullopt,FC::Domain::FileAccess::Read,false},operation));
                const auto bytes=take(files_.read(authorized,64U*1024U,operation));
                const auto control=Json::parse(std::string_view{reinterpret_cast<const char*>(bytes.data()),bytes.size()});
                if (control.at("run_id")!=runId_ || control.at("window_id")!=selected.at("window_id") || control.at("pid")!=browser.pid ||
                    control.at("capture_sha256")!=firstCapture.at("sha256")) throw std::runtime_error("Control file does not bind the exact observed owned canvas capture.");
                const auto point=[&](const Json& value,const char* x,const char* y) {
                    const auto px=value.at(x).get<int>(),py=value.at(y).get<int>();
                    if (px<canvasRect.at("x").get<double>()+16 || py<canvasRect.at("y").get<double>()+16 ||
                        px>=canvasRect.at("x").get<double>()+canvasRect.at("width").get<double>()-16 ||
                        py>=canvasRect.at("y").get<double>()+canvasRect.at("height").get<double>()-16)
                        throw std::runtime_error("Observed control point is outside the empty canvas interior.");
                };
                auto scroll=control.at("scroll"),drag=control.at("drag"); point(scroll,"x","y"); point(drag,"start_x","start_y"); point(drag,"end_x","end_y");
                const auto delta=scroll.at("delta").get<int>();
                if (!delta || delta<-1200 || delta>1200 || drag.value("duration_ms",500U)>1000U || drag.value("duration_ms",500U)<100U)
                    throw std::runtime_error("Qualification input exceeds its bounded wheel/drag contract.");
                const auto sameTransform=[](const Json& a,const Json& b) {
                    return std::abs(a.at("scale").get<double>()-b.at("scale").get<double>())<0.0001 &&
                        std::abs(a.at("offset").at(0).get<double>()-b.at("offset").at(0).get<double>())<0.05 &&
                        std::abs(a.at("offset").at(1).get<double>()-b.at("offset").at(1).get<double>())<0.05;
                };
                const auto input=[&](const char* name,Json args,const char* label) {
                    verifyProvider(); if (observeProvider()!=beforeProvider) throw std::runtime_error("Provider queue/history changed before input.");
                    RECT current{}; if (!GetWindowRect(owned.window,&current) || current.left!=windowRect.left || current.top!=windowRect.top ||
                        current.right!=windowRect.right || current.bottom!=windowRect.bottom) throw std::runtime_error("Observed target window moved or resized.");
                    const auto prior=fixedCall(observationFunction);
                    if (prior.at("graph")!=base.at("graph") || prior.at("api")!=base.at("api") || prior.at("canvas_rect")!=base.at("canvas_rect") || prior.at("viewport")!=base.at("viewport"))
                        throw std::runtime_error("Observed canvas graph or geometry changed before input.");
                    Json points=Json::array();
                    const auto cssPoint=[&](const char* x,const char* y) { points.push_back({{"x",(args.at(x).get<double>()-pageX)/ratio},{"y",(args.at(y).get<double>()-pageY)/ratio}}); };
                    if (std::string_view{name}=="desktop_scroll") cssPoint("x","y");
                    else { cssPoint("start_x","start_y"); cssPoint("end_x","end_y"); }
                    const auto hits=fixedCall(R"js(function(points) {
                        const canvas=this.canvas.canvas;
                        return points.map(point=>{const hit=document.elementFromPoint(point.x,point.y);return {point,canvas_is_topmost:hit===canvas};});
                    })js",Json::array({Json{{"value",points}}}));
                    for(const auto& hit:hits) if(!hit.at("canvas_is_topmost").get<bool>()) throw std::runtime_error("Observed input point is covered inside the browser page.");
                    event(std::string{label}+"_canvas_hit_test",hits);
                    capture(std::string{label}+"_before"); args["window_id"]=selected.at("window_id"); args["pid"]=browser.pid;
                    const auto receipt=desktopCall(name,args); event(label,{{"arguments",args},{"receipt",receipt}});
                    if (!receipt.value("input_submitted",false)) throw std::runtime_error("Native guarded input was not submitted.");
                    std::this_thread::sleep_for(std::chrono::milliseconds{250});
                    const auto after=fixedCall(observationFunction); event(std::string{label}+"_observed",after); capture(std::string{label}+"_after");
                    if (after.at("graph")!=base.at("graph") || after.at("api")!=base.at("api")) throw std::runtime_error("Canvas input changed graph/API content.");
                    return after;
                };
                const auto afterScroll=input("desktop_scroll",scroll,"desktop_scroll"); scroll["delta"]=-delta;
                const auto afterInverseScroll=input("desktop_scroll",scroll,"desktop_scroll_inverse");
                const auto afterDrag=input("desktop_drag",drag,"desktop_drag");
                const auto startX=drag.at("start_x"),startY=drag.at("start_y"); drag["start_x"]=drag.at("end_x"); drag["start_y"]=drag.at("end_y"); drag["end_x"]=startX; drag["end_y"]=startY;
                const auto final=input("desktop_drag",drag,"desktop_drag_inverse");
                const auto afterProvider=observeProvider(); event("desktop_provider_after",afterProvider);
                const auto& events=final.at("events"); unsigned wheels{},downs{},ups{},moves{};
                for (const auto& item : events) if (item.value("isTrusted",false)) {
                    const auto type=item.value("type",std::string{}); if(type=="wheel")++wheels; else if(type=="pointerdown")++downs; else if(type=="pointerup")++ups; else if(type=="pointermove" && item.value("buttons",0U))++moves;
                }
                const auto successful=!sameTransform(base.at("ds"),afterScroll.at("ds")) && sameTransform(base.at("ds"),afterInverseScroll.at("ds")) &&
                    !sameTransform(afterInverseScroll.at("ds"),afterDrag.at("ds")) && sameTransform(base.at("ds"),final.at("ds")) &&
                    wheels>=2U && downs>=2U && ups>=2U && moves>=2U && beforeProvider==afterProvider;
                result={{"ok",successful},{"scroll_motion_observed",!sameTransform(base.at("ds"),afterScroll.at("ds"))},
                    {"scroll_inverse_restored",sameTransform(base.at("ds"),afterInverseScroll.at("ds"))},
                    {"drag_motion_observed",!sameTransform(afterInverseScroll.at("ds"),afterDrag.at("ds"))},{"drag_inverse_restored",sameTransform(base.at("ds"),final.at("ds"))},
                    {"trusted_wheel_events",wheels},{"trusted_pointer_down_events",downs},{"trusted_pointer_up_events",ups},{"trusted_drag_move_events",moves},
                    {"graph_and_api_preserved",true},{"provider_identity_queue_history_preserved",beforeProvider==afterProvider},
                    {"generation_submitted",false},{"native_service","WindowsDesktopArtifactService"},{"window_id",selected.at("window_id")},{"pid",browser.pid},
                    {"controls",control},{"initial_transform",base.at("ds")},{"final_transform",final.at("ds")}};
            }
            cleanup(); result["owned_browser_cleanup_verified"]=true; result["owned_profile_cleanup_verified"]=true; return result;
        } catch (...) {
            const auto error=std::current_exception();
            try { cleanup(); } catch (const std::exception& cleanupError) { event("desktop_cleanup_error",{{"error",cleanupError.what()},{"private_profile",utf8(profile)}}); }
            catch (const CD::Failure& cleanupError) { event("desktop_cleanup_error",{{"code",cleanupError.error.code},{"error",cleanupError.error.message},{"private_profile",utf8(profile)}}); }
            std::rethrow_exception(error);
        }
        } catch (const CD::Failure& failure) {
            throw std::runtime_error(failure.error.code+": "+failure.error.message);
        }
    }
    Json frontend() {
        const auto custom = options_.option("workflow");
        const auto source = custom.empty() ? native(configuration_.value.comfyUi.installationPath) / L"ComfyUI" / L"user" / L"default" / L"workflows" / L"test-workflow.json" : native(custom);
        auto editor = readJson(source);
        if (!editor.contains("nodes")) throw std::runtime_error("Frontend qualification requires editor JSON with nodes.");
        Json patches = Json::array();
        if (custom.empty()) {
            patches.push_back({{"node_id", "4"}, {"input", "ckpt_name"}, {"value", "cyberrealistic_final.safetensors"}});
            patches.push_back({{"node_id", "6"}, {"input", "text"}, {"value", "Native Forge frontend serialization qualification."}});
            // Insert an actual LiteGraph reroute and a bypassed model node.
            const auto bypass = editor.at("last_node_id").get<unsigned>() + 1U, reroute = bypass + 1U;
            auto nextLink = editor.at("last_link_id").get<unsigned>() + 1U;
            auto& links = editor.at("links"); auto original = std::find_if(links.begin(), links.end(), [](const auto& link) { return link.is_array() && link.size() == 6U && link[1] == 4U && link[2] == 0U && link[3] == 3U; });
            if (original == links.end()) throw std::runtime_error("Installed editor fixture no longer has its expected model link.");
            const auto inputLink = original->at(0); (*original)[3] = bypass; (*original)[4] = 0U;
            const auto rerouteLink = nextLink++, samplerLink = nextLink++;
            links.push_back(Json::array({rerouteLink, bypass, 0U, reroute, 0U, "MODEL"}));
            links.push_back(Json::array({samplerLink, reroute, 0U, 3U, 0U, "MODEL"}));
            for (auto& node : editor.at("nodes")) if (node.at("id") == 3U) for (auto& input : node.at("inputs")) if (input.at("name") == "model") input["link"] = samplerLink;
            editor["nodes"].push_back({{"id", bypass}, {"type", "ModelSamplingSD3"}, {"pos", Json::array({100,100})}, {"size", Json::array({270,82})},
                {"flags", Json::object()}, {"order", 8U}, {"mode", 4U}, {"inputs", Json::array({{{"name","model"},{"type","MODEL"},{"link",inputLink}}})},
                {"outputs", Json::array({{{"name","MODEL"},{"type","MODEL"},{"links",Json::array({rerouteLink})}}})}, {"properties",{{"Node name for S&R","ModelSamplingSD3"}}}, {"widgets_values",Json::array({8.0})}});
            editor["nodes"].push_back({{"id", reroute}, {"type", "Reroute"}, {"pos", Json::array({300,100})}, {"size", Json::array({75,26})},
                {"flags", Json::object()}, {"order", 9U}, {"mode", 0U}, {"inputs",Json::array({{{"name",""},{"type",""},{"link",rerouteLink}}})},
                {"outputs",Json::array({{{"name",""},{"type","MODEL"},{"links",Json::array({samplerLink})}}})}, {"properties",{{"showOutputText",false},{"horizontal",false}}}});
            editor["last_node_id"] = reroute; editor["last_link_id"] = samplerLink;
        }
        auto result = invoke("comfy_workflow", {{"action","inspect"},{"workflow",editor},{"patches",patches}});
        if (result.value("format", std::string{}) != "ui_and_api" || result.value("queued", true) || !result.contains("ui_workflow"))
            throw std::runtime_error("Frontend did not preserve editor JSON and produce API JSON without submission.");
        if (custom.empty()) {
            const auto& api = result.at("workflow");
            if (api.at("4").at("inputs").at("ckpt_name") != "cyberrealistic_final.safetensors" ||
                api.at("6").at("inputs").at("text") != "Native Forge frontend serialization qualification." ||
                api.at("3").at("inputs").at("model") != Json::array({"4",0U}) || api.at("3").at("inputs").at("steps") != 20U ||
                api.at("3").at("inputs").at("seed") != 156680208700286ULL)
                throw std::runtime_error("Typed widget edits, reroute, bypass, or KSampler control widget serialization differs from its expected API contract.");
            result["qualified_cases"] = Json::array({"typed_string_widget", "ksampler_seed_control_widget", "reroute", "bypassed_model_node", "editor_and_api_retained", "no_submission"});
        }
        else if (!options_.option("arguments").empty()) {
            const auto assertions = readJson(native(options_.option("arguments")));
            const auto fixtureName = native(custom).filename().string();
            if (!assertions.is_object() || !assertions.contains(fixtureName))
                throw std::runtime_error("Frontend assertions do not name the requested fixture.");
            const auto& expected = assertions.at(fixtureName);
            const auto& api = result.at("workflow");
            if (!expected.contains("workflow") || !expected.at("workflow").is_object() || !api.is_object() || api.size() != expected.at("workflow").size())
                throw std::runtime_error("Frontend fixture did not retain the exact expected executable node count.");
            for (const auto& [id,node] : expected.at("workflow").items()) {
                if (!api.contains(id) || api.at(id).at("class_type") != node.at("class_type") || api.at(id).at("inputs") != node.at("inputs"))
                    throw std::runtime_error("Frontend fixture executable class or inputs differ at node " + id + ".");
            }
            if (expected.contains("required_editor_definition_id")) {
                const auto& retained = result.at("ui_workflow");
                const auto definitions = retained.value("definitions",Json::object()).value("subgraphs",Json::array());
                if (!definitions.is_array() || std::none_of(definitions.begin(),definitions.end(),[&](const auto& definition) {
                    return definition.is_object() && definition.value("id",std::string{}) == expected.at("required_editor_definition_id").get<std::string>();
                })) throw std::runtime_error("Frontend fixture did not retain its required editor subgraph definition.");
            }
            result["qualified_cases"] = expected.value("qualified_cases",Json::array({fixtureName,"exact_api_node_count","exact_class_and_inputs","editor_and_api_retained","no_submission"}));
            event("frontend_expected_contract",expected);
        }
        event("frontend_serialization", result); return result;
    }
    Json preview() {
        const auto name = options_.kind == "image" ? "sd1_image" : options_.kind == "t2v" ? "wan22_5b_t2v" : options_.kind == "i2v" ? "wan22_5b_i2v" : "";
        if (std::string_view{name}.empty()) throw std::runtime_error("preview requires image, t2v, or i2v.");
#ifdef FC_COMFY_SOURCE_RESOURCE_DIR
        const auto resources = native(options_.option("resources", FC_COMFY_SOURCE_RESOURCE_DIR));
#else
        const auto resources = native(required("resources"));
#endif
        const auto manifest = readJson(resources / L"starter_manifest.json");
        const auto& workflows = manifest.at("workflows");
        const auto selected = std::find_if(workflows.begin(), workflows.end(), [&](const auto& entry) { return entry.at("id") == name; });
        if (selected == workflows.end()) throw std::runtime_error("Starter manifest omits selected workflow.");
        auto draft = readJson(resources / native(selected->at("preview_file").get<std::string>()));
        auto final = readJson(resources / native(selected->at("final_file").get<std::string>()));
        const auto prompt = options_.option("prompt");
        if (!prompt.empty()) { const auto& binding = selected->at("parameters").at("prompt"); for (auto* graph : {&draft,&final}) (*graph)[binding.at("node_id").get<std::string>()]["inputs"][binding.at("input").get<std::string>()] = prompt; }
        if (options_.kind != "image") {
            const auto& contract = selected->at("duration_contract");
            const auto duration = [&](Json& graph, std::string_view key) {
                if (options_.option(key).empty()) return;
                const auto seconds = options_.number(key,5U,30U), stride = contract.at("temporal_stride").get<unsigned>(), offset = contract.at("frame_offset").get<unsigned>();
                const auto fps = graph.at(contract.at("fps_node").get<std::string>()).at("inputs").at(contract.at("fps_input").get<std::string>()).get<double>();
                const auto frames = static_cast<unsigned>(std::ceil((fps*seconds-offset)/stride))*stride+offset;
                graph[contract.at("latent_node").get<std::string>()]["inputs"][contract.at("frame_input").get<std::string>()] = frames;
            }; duration(draft,"draft-seconds"); duration(final,"seconds");
        }
        Json inputs = Json::array();
        if (options_.kind == "i2v") {
            auto source = Fs::canonical(native(required("input")));
            if (!Fs::is_regular_file(source) || Fs::file_size(source) > 1024ULL*1024ULL*1024ULL) throw std::runtime_error("Input must be a regular image smaller than 1 GiB.");
            auto inputName = native("input-" + take(uuids_.next()).value()); inputName += source.extension();
            const auto copied = root_ / L"workspace" / inputName;
            if (!Fs::copy_file(source,copied,Fs::copy_options::none)) throw std::runtime_error("Input copy was not published.");
            const auto& binding = selected->at("input_bindings").at(0);
            inputs.push_back({{"node_id",binding.at("node_id")},{"input",binding.at("input")},{"path",utf8(copied)}});
            event("input_copy",{{"source",utf8(source)},{"path",utf8(copied)},{"bytes",Fs::file_size(copied)}});
        }
        Json args{{"stage","preview"},{"media_kind",options_.kind=="image"?"image":"video"},{"preview_workflow",draft},{"final_workflow",final},
            {"inputs",inputs},{"output_directory",utf8(root_/L"workspace")},{"expected_outputs",selected->at("expected_outputs")},
            {"timeout_sec",configuration_.value.comfyUi.generationTimeoutSeconds}};
        event("render_request",args);
        const auto result = wait(invoke("comfy_run",args));
        if (result.value("state",std::string{}) == "awaiting_preview_approval") {
            evidence_["host_preview_inference_verified"] = true;
            evidence_["final_approval_status"] = "Requires actual saved LM Studio preview tool result followed by an operator approval reply. The harness never writes either.";
        }
        return result;
    }
    Arguments options_; Fs::path root_, evidencePath_, lmRoot_, lmExecutable_; std::string runId_;
    std::chrono::steady_clock::time_point started_; Json evidence_;
    IW::WindowsAtomicFileStore files_; IW::WindowsUuidGenerator uuids_; IW::SystemClock clock_; IW::BCryptSha256Hasher hasher_;
    Configuration configuration_; std::unique_ptr<IW::WindowsWorkspaceAuthority> issuer_,storageIssuer_;
    std::unique_ptr<FC::Contracts::WorkspaceAuthority> scope_,storageScope_;
    std::shared_ptr<NW::WindowsComfyUiBackend> backend_; std::unique_ptr<NW::WindowsComfyUiService> service_;
};
}

int wmain(int count,wchar_t** values) {
    try { Harness harness{arguments(count,values)}; return harness.run(); }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
