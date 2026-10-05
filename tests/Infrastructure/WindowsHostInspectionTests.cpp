#include "TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioResponsesTransport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsGitHubReadService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsSystemInspection.h"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>

namespace ForgeConductor::Tests {
namespace {
namespace Native = Infrastructure::Windows;
using Json = nlohmann::json;

void providerUsesLoadedInstanceAndExactIdentity()
{
    Domain::LocalModelConfig configured;
    configured.model = "qwen3.8-27b";
    configured.effectiveContextCapacity = 133120U;
    configured.nextResponseReserve = 4096U;
    configured.handoffReserve = 4096U;
    configured.estimationSafetyMargin = 2048U;
    const auto inventory = R"({"models":[
        {"type":"llm","key":"qwen/qwen3.8-27b","format":"gguf",
         "quantization":{"name":"Q8_0","bits_per_weight":8},"selected_variant":"qwen/qwen3.8-27b@q8_0",
         "max_context_length":262144,"loaded_instances":[{"id":"active-instance","config":{"context_length":262144,"parallel":4}}]},
        {"type":"llm","key":"other","loaded_instances":[]},
        {"type":"embedding","key":"embed","loaded_instances":[{"id":"embed-instance"}]}]})";
    auto value = Json::parse(take(Native::LMStudioResponsesTransport::projectModelInventory(inventory, configured)));
    require(value.at("loaded_instance_count") == 1U && value.at("downloaded_llm_count") == 2U,
        "Only actual loaded LLM instances may be reported as active.");
    const auto& loaded = value.at("loaded_models").at(0U);
    require(loaded.at("model_key") == "qwen/qwen3.8-27b" && loaded.at("instance_id") == "active-instance",
        "Provider identifiers must be preserved without inferring a filename from the key.");
    require(loaded.at("context_length") == 262144U && loaded.at("quantization").at("name") == "Q8_0",
        "Loaded context and quantization must come from actual provider inventory.");
    require(value.at("configured_context").at("effective_capacity") == 133120U &&
        value.at("configured_context").at("next_response_reserve") == 4096U,
        "Saved Forge limits must remain separate from measured provider context.");
    require(!value.at("configured_model_loaded").get<bool>() && value.at("configured_model_matches").empty(),
        "Similar display names/suffixes must not be treated as an exact configured model match.");
    for (const char* field : {"model_file", "model_revision", "runtime_version"})
        require(!loaded.at(field).at("known").get<bool>() && loaded.at(field).at("value").is_null() &&
            !loaded.at(field).at("reason").get<std::string>().empty(), "Unknown metadata must be explicitly labelled.");
    configured.model = "active-instance";
    value = Json::parse(take(Native::LMStudioResponsesTransport::projectModelInventory(inventory, configured)));
    require(value.at("configured_model_loaded").get<bool>() && value.at("configured_model_matches").at(0U) == "active-instance",
        "Exact loaded instance identity must be recognized.");
}

void providerRejectsMalformedAndOversizedInventory()
{
    Domain::LocalModelConfig configured;
    requireError(Native::LMStudioResponsesTransport::projectModelInventory(R"({"models":{}})", configured),
        Domain::ErrorCodes::MalformedMessage, "Unexpected model inventory shape must fail.");
    requireError(Native::LMStudioResponsesTransport::projectModelInventory("{", configured),
        Domain::ErrorCodes::MalformedMessage, "Malformed provider JSON must fail.");
    requireError(Native::LMStudioResponsesTransport::projectModelInventory(std::string(2U * 1024U * 1024U + 1U, ' '), configured),
        Domain::ErrorCodes::PayloadTooLarge, "Oversized provider JSON must fail before parsing.");
    const Json amplified{{"models", Json::array({Json{{"type", "llm"}, {"key", "model"},
        {"capabilities", std::string(128U * 1024U, 'x')},
        {"loaded_instances", Json::array({Json{{"id", "one"}}, Json{{"id", "two"}}})}}})}};
    const auto amplifiedNative = amplified.dump();
    require(amplifiedNative.size() < 2U * 1024U * 1024U,
        "The projected-bound fixture must pass the native HTTP size limit.");
    requireError(Native::LMStudioResponsesTransport::projectModelInventory(amplifiedNative, configured),
        Domain::ErrorCodes::PayloadTooLarge,
        "Repeated loaded model metadata must not overflow the MCP receipt even when the native response fits.");
    const auto value = Json::parse(take(Native::LMStudioResponsesTransport::projectModelInventory(R"({"models":[]})", configured)));
    require(value.at("loaded_models").empty() && !value.at("configured_model_loaded").get<bool>(),
        "An empty provider inventory must not fabricate a loaded model.");
}

void githubConstrainsRepositoryOperationsAndPagination()
{
    Contracts::GitHubReadRequest request{"owner/repository", "runs"};
    require(take(Native::WindowsGitHubReadService::requestPath(request)) == "/repos/owner/repository/actions/runs?per_page=30&page=1",
        "Runs routing must use the fixed repository API path.");
    request.operation = "run"; request.id = 37254719493ULL;
    require(take(Native::WindowsGitHubReadService::requestPath(request)) == "/repos/owner/repository/actions/runs/37254719493",
        "A run id must remain a 64-bit identifier.");
    request.operation = "artifacts"; request.page = 2U; request.perPage = 10U;
    require(take(Native::WindowsGitHubReadService::requestPath(request)) == "/repos/owner/repository/actions/runs/37254719493/artifacts?per_page=10&page=2",
        "Run artifacts must query the selected run, preserving pagination.");
    request.id.reset(); request.operation = "pull_requests";
    require(take(Native::WindowsGitHubReadService::requestPath(request)) == "/repos/owner/repository/pulls?state=all&per_page=10&page=2",
        "Pull request inspection must remain GET-only.");
    request.operation = "refs"; request.ref = "heads/feature/a?x=y";
    require(take(Native::WindowsGitHubReadService::requestPath(request)) == "/repos/owner/repository/git/ref/heads%2Ffeature%2Fa%3Fx%3Dy",
        "Ref segments must be escaped rather than becoming arbitrary query parameters.");
    for (const char* repository : {"https://github.com/owner/repo", "../repo", "owner/repo/extra", "owner/repo.git", "owner/repo?x=y"}) {
        request.repository = repository;
        requireError(Native::WindowsGitHubReadService::requestPath(request), Domain::ErrorCodes::InvalidRequest,
            "Caller URLs/path traversal/additional segments must fail.");
    }
    request.repository = "owner/repository"; request.ref = "heads/";
    requireError(Native::WindowsGitHubReadService::requestPath(request), Domain::ErrorCodes::InvalidRequest,
        "A ref with no name must fail.");
    request.ref = "heads/../../main";
    requireError(Native::WindowsGitHubReadService::requestPath(request), Domain::ErrorCodes::InvalidRequest,
        "Traversal refs must fail.");
    request.ref.reset(); request.operation = "delete_run";
    requireError(Native::WindowsGitHubReadService::requestPath(request), Domain::ErrorCodes::InvalidRequest,
        "Write operations must not be routable.");
    request.operation = "runs"; request.perPage = 101U;
    requireError(Native::WindowsGitHubReadService::requestPath(request), Domain::ErrorCodes::InvalidRequest,
        "A GitHub page must be bounded.");
    request.perPage = 30U; request.id = 1U;
    requireError(Native::WindowsGitHubReadService::requestPath(request), Domain::ErrorCodes::InvalidRequest,
        "Unused identifiers must not silently route to a different scope.");
}

void githubPreservesArtifactCountAndNativeResponse()
{
    Contracts::GitHubReadRequest request{"owner/repository", "artifacts"}; request.id = 37254719493ULL;
    const auto value = Json::parse(take(Native::WindowsGitHubReadService::projectResponse(request,
        R"({"total_count":2,"artifacts":[{"id":91,"name":"receipt"}]})", true, true)));
    require(value.at("total_count") == 2U && value.at("data").at("total_count") == 2U && value.at("data").at("artifacts").size() == 1U,
        "GitHub total_count must preserve API evidence independently from current page length.");
    require(value.at("http_method") == "GET" && value.at("authenticated") == true && value.at("has_next_page") == true && value.at("next_page") == 2U,
        "Read provenance and pagination must be explicit without exposing credentials.");
    const auto empty = Json::parse(take(Native::WindowsGitHubReadService::projectResponse(request,
        R"({"total_count":0,"artifacts":[]})", false, false)));
    require(empty.at("total_count") == 0U && empty.at("next_page").is_null(),
        "An actual zero-artifact response must remain distinguishable from inaccessible data.");
    const Json oversized{{"total_count", 1U}, {"artifacts", Json::array({Json{{"name", std::string(192U * 1024U, 'x')}}})}};
    requireError(Native::WindowsGitHubReadService::projectResponse(request, oversized.dump(), false, false),
        Domain::ErrorCodes::PayloadTooLarge, "Oversized native data must yield an actionable page-size error before MCP envelope overflow.");
    requireError(Native::WindowsGitHubReadService::projectResponse(request, "{", false, false),
        Domain::ErrorCodes::MalformedMessage, "Malformed GitHub JSON must fail.");
}

void desktopVersionFactsHaveRuntimeProvenance()
{
    TestContext context;
    const auto value = Json::parse(take(Native::WindowsSystemInspection::lmStudioDesktopVersion(context.active())));
    require(value.at("observations").size() <= 16U && value.at("server_api_version").is_null() && value.at("inference_engine_version").is_null(),
        "A desktop binary version must not fabricate server/engine versions.");
    if (value.at("known").get<bool>()) {
        require(value.at("value").is_string() && !value.at("value").get<std::string>().empty() && !value.at("observations").empty(),
            "A known desktop version requires actual process image observations.");
        for (const auto& observed : value.at("observations")) require(!observed.at("process_ids").empty() &&
            observed.at("executable_path").is_string() && observed.at("product_version") == value.at("value"),
            "Known version facts must map to observed running process images.");
    } else require(value.at("value").is_null() && value.at("reason").is_string(),
        "Missing/ambiguous running version facts must be labelled unknown.");
    context.cancellation.request_stop();
    requireError(Native::WindowsSystemInspection::lmStudioDesktopVersion(context.active()), Domain::ErrorCodes::Cancelled,
        "Cancelled desktop version inspection must not query processes.");
}

void nativeInspectionIsBoundedAndContainsCurrentProcess()
{
    TestContext context;
    const auto value = Json::parse(take(Native::WindowsSystemInspection::inspect(context.active())));
    require(value.at("process_scope") == "same_user_readable" && value.at("processes_available").get<bool>(),
        "Process inspection must identify its same-user readable scope.");
    const auto& processes = value.at("processes");
    require(processes.size() <= 256U && value.at("services").size() <= 256U, "Native output must be bounded.");
    require(std::any_of(processes.begin(), processes.end(), [](const Json& process) {
        return process.at("pid").get<DWORD>() == GetCurrentProcessId();
    }), "Native process inspection must include the calling user's current process.");
    for (const auto& process : processes) require(process.size() == 4U && !process.contains("command_line") && !process.contains("environment"),
        "Process inspection must not expose arguments or environments.");
    for (const auto& service : value.at("services")) require(service.size() == 4U && !service.contains("account") && !service.contains("binary_path"),
        "Service inspection must omit service account credentials and executable arguments.");
}

void inspectionHonorsCancellationAndRejectsCredentialInjection()
{
    TestContext context; context.cancellation.request_stop();
    requireError(Native::WindowsSystemInspection::inspect(context.active()), Domain::ErrorCodes::Cancelled,
        "Cancelled native inspection must not enumerate the host.");
    Native::WindowsGitHubReadService publicService;
    Contracts::GitHubReadRequest request{"owner/repository", "runs"};
    requireError(publicService.read(request, context.active()), Domain::ErrorCodes::Cancelled,
        "Cancelled GitHub inspection must not connect.");
    TestContext activeContext;
    requireError(Native::WindowsSystemInspection::inspect(activeContext.expired()), Domain::ErrorCodes::DeadlineExceeded,
        "Expired native inspection must not enumerate the host.");
    requireError(publicService.read(request, activeContext.expired()), Domain::ErrorCodes::DeadlineExceeded,
        "Expired GitHub inspection must not connect.");
    Native::WindowsGitHubReadService invalidToken{std::string{"canary\r\nInjected: value"}};
    const auto result = invalidToken.read(request, activeContext.active());
    requireError(result, Domain::ErrorCodes::InvalidRequest, "A configured credential must not inject headers.");
    require(result.error().message.find("canary") == std::string::npos,
        "Credential validation failures must never echo credential content.");
}
} // namespace
} // namespace ForgeConductor::Tests

int main()
{
    using namespace ForgeConductor::Tests;
    try {
        providerUsesLoadedInstanceAndExactIdentity();
        providerRejectsMalformedAndOversizedInventory();
        githubConstrainsRepositoryOperationsAndPagination();
        githubPreservesArtifactCountAndNativeResponse();
        desktopVersionFactsHaveRuntimeProvenance();
        nativeInspectionIsBoundedAndContainsCurrentProcess();
        inspectionHonorsCancellationAndRejectsCredentialInjection();
        std::cout << "7 host inspection tests passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
