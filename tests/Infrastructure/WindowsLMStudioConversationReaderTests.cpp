#include "TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioConversationReader.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioChatContinuity.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "Infrastructure/Windows/LMStudioChatContinuityControl.h"
#include "Infrastructure/Windows/LMStudioChatCheckpoint.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Fakes/ConfigurationStoreFake.h"
#include "Fakes/ProjectRepositoryFakes.h"
#include "Fakes/RecordingProjectMemoryService.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::WindowsLMStudioConversationReader;
using Json = nlohmann::json;

class ConversationFixture final {
public:
    ConversationFixture()
        : root_{std::filesystem::current_path() /
              ("conversation-reader-fixture-" + std::to_string(::GetCurrentProcessId()) +
               "-" + std::to_string(::GetTickCount64()))}
    {
        std::filesystem::create_directories(root_ / ".internal");
        std::filesystem::create_directories(root_ / "conversations" / "project");
        write(root_ / ".internal" / "conversation-config.json",
            Json{{"selectedConversation", "project/chat.conversation.json"}});
        const auto native = root_.generic_u8string();
        const std::string encoded{
            reinterpret_cast<const char*>(native.data()), native.size()};
        write(root_ / ".internal" / "projects-registry.json",
            Json{{"json", {{"projects", Json::array({
                Json::array({"test-project", Json{{"path", encoded}}})})}}}});
    }

    ~ConversationFixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    void save(const Json& conversation) const
    {
        write(root_ / "conversations" / "project" / "chat.conversation.json", conversation);
    }

    [[nodiscard]] Domain::PathText path() const
    {
        const auto encoded = root_.generic_u8string();
        return take(Domain::PathText::create(std::string{
            reinterpret_cast<const char*>(encoded.data()), encoded.size()}));
    }

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    static void write(const std::filesystem::path& path, const Json& value)
    {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        output << value.dump();
        require(static_cast<bool>(output), "conversation fixture write failed");
    }

private:
    std::filesystem::path root_;
};

Json generation(const std::uint64_t used, const std::uint64_t capacity,
                const std::string_view reason = "eosFound")
{
    return Json{{"type", "contentBlock"}, {"genInfo", {
        {"stats", {{"totalTokensCount", used}, {"stopReason", reason}}},
        {"loadModelConfig", {{"fields", Json::array({
            Json{{"key", "llm.load.contextLength"}, {"value", capacity}}})}}}}}};
}

Json version(const Json& steps)
{
    return Json{{"type", "multiStep"}, {"role", "assistant"}, {"steps", steps}};
}

Json message(const Json& versions, const std::uint64_t selected = 0U)
{
    return Json{{"versions", versions}, {"currentlySelected", selected}};
}

Json conversation(const Json& messages)
{
    return Json{{"tokenCount", 999999U}, {"messages", messages},
        {"plugins", Json::array({"mcp/forge-conductor",
            "mcp/forge-conductor-fallback", "mcp/forge-conductor-clu"})}};
}

void selectedProviderStatisticsAndTerminalTools()
{
    ConversationFixture fixture;
    const Json tool{{"type", "toolStatus"}, {"statusState", {
        {"status", {{"type", "toolCallSucceeded"}}}}}};
    fixture.save(conversation(Json::array({
        message(Json::array({
            version(Json::array({generation(48000U, 48384U, "contextLengthReached")})),
            version(Json::array({generation(18000U, 32768U), tool}))}), 1U)})));
    TestContext context;
    const auto observation = take(WindowsLMStudioConversationReader::read(
        fixture.path(), context.active()));
    require(observation.has_value(), "selected conversation was not observed");
    require(observation->conversationId == "project/chat.conversation.json",
        "conversation identifier changed");
    require(observation->projectIdentifier == "test-project",
        "project identifier did not come from the root's registry");
    require(observation->usedTokens == 18000U &&
        observation->contextCapacity == 32768U,
        "reader used an unselected version or replaced provider usage with a cached prompt count");
    require(!observation->cachedRenderedPromptTokens,
        "an unbound cached prompt count was admitted without a selected model identity");
    require(Json::parse(observation->generationEvidence) == Json{
        {"message_index", 0U}, {"selected_version", 1U}, {"step_index", 0U},
        {"genInfo", generation(18000U, 32768U)["genInfo"]}},
        "generation evidence did not identify the latest selected provider generation");
    require(!observation->overflow && !observation->toolsActive,
        "unselected overflow or completed tool blocked the pause");
    require(observation->plugins.size() == 3U, "plugin bindings were lost");
}

Json renderedPromptConversation(const Json& count)
{
    auto selected = generation(130969U, 262144U);
    selected["genInfo"]["identifier"] = "qwen/qwen3.8-27b";
    selected["genInfo"]["indexedModelIdentifier"] = "qwen/qwen3.8-27b";
    auto document = conversation(Json::array({
        message(Json::array({version(Json::array({selected}))}))}));
    document["tokenCount"] = count;
    document["lastUsedModel"] = Json{{"identifier", "qwen/qwen3.8-27b"},
        {"indexedModelIdentifier", "deployment-prefix:qwen/qwen3.8-27b"},
        {"instanceLoadTimeConfig", {{"fields", Json::array({
            Json{{"key", "llm.load.contextLength"}, {"value", 262144U}}})}}}};
    return document;
}

void cachedRenderedPromptRemainsSeparateFromProviderStatistics()
{
    ConversationFixture fixture;
    fixture.save(renderedPromptConversation(264415U));
    TestContext context;
    const auto observation = take(WindowsLMStudioConversationReader::read(
        fixture.path(), context.active()));
    require(observation && observation->cachedRenderedPromptTokens == 264415U,
        "the full rendered prompt count was lost after a smaller provider generation");
    require(observation->usedTokens == 130969U &&
        observation->contextCapacity == 262144U && !observation->overflow &&
        observation->stopReason == "eosFound",
        "a cached prompt larger than capacity changed actual provider usage or overflow");
    const auto evidence = Json::parse(observation->generationEvidence);
    require(evidence.at("genInfo").at("stats").at("totalTokensCount") == 130969U &&
        !evidence.at("genInfo").contains("tokenCount"),
        "the cached prompt count was presented as timestamped provider generation evidence");
}

void cachedRenderedPromptNumericAdmission()
{
    ConversationFixture fixture;
    TestContext context;
    const auto verify = [&](const Json& document,
                            const std::optional<std::uint64_t> expected) {
        fixture.save(document);
        const auto observation = take(WindowsLMStudioConversationReader::read(
            fixture.path(), context.active()));
        require(observation && observation->cachedRenderedPromptTokens == expected,
            "cached rendered prompt numeric admission differed from its integer contract");
        require(observation->usedTokens == 130969U &&
            observation->contextCapacity == 262144U && !observation->overflow,
            "an absent or invalid cached count changed provider statistics");
    };
    verify(renderedPromptConversation(0U), 0U);
    verify(renderedPromptConversation(std::int64_t{264415}), 264415U);
    verify(renderedPromptConversation((std::numeric_limits<std::uint64_t>::max)()),
        (std::numeric_limits<std::uint64_t>::max)());
    auto absent = renderedPromptConversation(264415U);
    absent.erase("tokenCount");
    verify(absent, std::nullopt);
    for (const auto& invalid : Json::array({nullptr, true, false, -1, 1.25,
             264415.0, "264415", Json::array({264415U}),
             Json{{"value", 264415U}}})) {
        verify(renderedPromptConversation(invalid), std::nullopt);
    }
}

void cachedRenderedPromptRequiresSelectedModelBinding()
{
    ConversationFixture fixture;
    TestContext context;
    const auto verify = [&](const Json& document, const bool admitted) {
        fixture.save(document);
        const auto observation = take(WindowsLMStudioConversationReader::read(
            fixture.path(), context.active()));
        require(observation && observation->usedTokens == 130969U &&
            observation->contextCapacity == 262144U && !observation->overflow,
            "cached prompt model admission changed selected provider usage");
        require(observation->cachedRenderedPromptTokens ==
                (admitted ? std::optional<std::uint64_t>{264415U} : std::nullopt),
            "cached rendered prompt was bound to a missing, different, or unselected model");
    };
    auto document = renderedPromptConversation(264415U);
    document.erase("lastUsedModel");
    verify(document, false);
    for (const auto& invalid : Json::array({nullptr, "qwen/qwen3.8-27b",
             Json::object(), Json{{"identifier", nullptr}},
             Json{{"identifier", 1}}, Json{{"identifier", ""}},
             Json{{"identifier", "qwen/qwen3.8-27b-other"}},
             Json{{"identifier", "QWEN/qwen3.8-27b"}}})) {
        document = renderedPromptConversation(264415U);
        if (invalid.is_object()) {
            document["lastUsedModel"] = invalid;
            document["lastUsedModel"]["instanceLoadTimeConfig"] =
                renderedPromptConversation(264415U)["lastUsedModel"]["instanceLoadTimeConfig"];
        } else {
            document["lastUsedModel"] = invalid;
        }
        verify(document, false);
    }
    for (const auto& invalid : Json::array({nullptr, 1, ""})) {
        document = renderedPromptConversation(264415U);
        document["messages"][0]["versions"][0]["steps"][0]["genInfo"]
            ["identifier"] = invalid;
        verify(document, false);
    }
    document = renderedPromptConversation(264415U);
    document["messages"][0]["versions"][0]["steps"][0]["genInfo"]
        .erase("identifier");
    verify(document, false);

    document = renderedPromptConversation(264415U);
    document["lastUsedModel"].erase("instanceLoadTimeConfig");
    verify(document, false);
    for (const auto& invalid : Json::array({nullptr, "262144",
             Json::object(), Json{{"fields", Json::array()}}})) {
        document = renderedPromptConversation(264415U);
        document["lastUsedModel"]["instanceLoadTimeConfig"] = invalid;
        verify(document, false);
    }
    for (const auto& invalid : Json::array({0U, -1, 262144.0, "262144", 131072U})) {
        document = renderedPromptConversation(264415U);
        document["lastUsedModel"]["instanceLoadTimeConfig"]["fields"][0]["value"] = invalid;
        verify(document, false);
    }
    for (const auto& malformed : Json::array({
             Json{{"key", 1}, {"value", 262144U}},
             Json{{"key", nullptr}, {"value", 262144U}},
             Json{{"value", 262144U}}})) {
        document = renderedPromptConversation(264415U);
        document["lastUsedModel"]["instanceLoadTimeConfig"]["fields"] =
            Json::array({malformed});
        verify(document, false);
        document["lastUsedModel"]["instanceLoadTimeConfig"]["fields"].push_back(
            Json{{"key", "llm.load.contextLength"}, {"value", 262144U}});
        verify(document, true);
    }

    auto unselected = generation(262144U, 262144U, "contextLengthReached");
    unselected["genInfo"]["identifier"] = "different-model";
    unselected["genInfo"]["indexedModelIdentifier"] = "different-model";
    auto selected = generation(130969U, 262144U);
    selected["genInfo"]["identifier"] = "qwen/qwen3.8-27b";
    selected["genInfo"]["indexedModelIdentifier"] = "qwen/qwen3.8-27b";
    document = renderedPromptConversation(264415U);
    document["messages"] = Json::array({message(Json::array({
        version(Json::array({unselected})), version(Json::array({selected}))}), 1U)});
    document["lastUsedModel"]["identifier"] = "different-model";
    verify(document, false);
    document["lastUsedModel"]["identifier"] = "qwen/qwen3.8-27b";
    verify(document, true);

    document["messages"] = Json::array({message(Json::array({
        version(Json::array({unselected, selected}))}))});
    document["lastUsedModel"]["identifier"] = "different-model";
    verify(document, false);
    document["lastUsedModel"]["identifier"] = "qwen/qwen3.8-27b";
    verify(document, true);
}

void cachedRenderedPromptRequiresValidProviderGeneration()
{
    ConversationFixture fixture;
    TestContext context;
    const auto verify = [&](const Json& document) {
        fixture.save(document);
        const auto observation = take(WindowsLMStudioConversationReader::read(
            fixture.path(), context.active()));
        require(observation && !observation->cachedRenderedPromptTokens &&
            observation->usedTokens == 0U &&
            observation->generationEvidence.empty(),
            "a cached prompt fabricated valid provider usage or generation evidence");
        require(observation->contextCapacity == 262144U,
            "missing provider usage discarded the independently loaded model capacity");
    };
    auto document = renderedPromptConversation(264415U);
    document["messages"] = Json::array();
    verify(document);
    document = renderedPromptConversation(264415U);
    document["messages"][0]["versions"][0]["steps"][0].erase("genInfo");
    verify(document);
    document = renderedPromptConversation(264415U);
    document["messages"][0]["versions"][0]["steps"][0]["genInfo"]
        .erase("stats");
    verify(document);
    document = renderedPromptConversation(264415U);
    document["messages"][0]["versions"][0]["steps"][0]["genInfo"]["stats"]
        .erase("totalTokensCount");
    verify(document);
    document = renderedPromptConversation(264415U);
    document["messages"][0]["versions"][0]["steps"][0]["genInfo"]["stats"]
        ["totalTokensCount"] = -1;
    verify(document);
    document = renderedPromptConversation(264415U);
    document["messages"][0]["versions"][0]["steps"][0]["genInfo"]
        .erase("loadModelConfig");
    verify(document);
    document = renderedPromptConversation(264415U);
    document["messages"][0]["versions"][0]["steps"][0]["genInfo"]
        ["loadModelConfig"]["fields"][0]["value"] = 0U;
    verify(document);
}

void overflowAndActiveTools()
{
    ConversationFixture fixture;
    fixture.save(conversation(Json::array({message(Json::array({
        version(Json::array({
            generation(48383U, 48384U, "contextLengthReached"),
            Json{{"type", "toolStatus"}, {"statusState", {
                {"status", {{"type", "callingTool"}}}}}}}))}))})));
    TestContext context;
    const auto observation = take(WindowsLMStudioConversationReader::read(
        fixture.path(), context.active()));
    require(observation && observation->overflow &&
        observation->usedTokens == 48383U &&
        observation->contextCapacity == 48384U,
        "actual provider context limit was not observed");
    require(observation->toolsActive, "an active Forge tool was treated as paused");
}

void partialFilesAndSelectionBoundary()
{
    ConversationFixture fixture;
    TestContext context;
    {
        std::ofstream partial{fixture.root() / "conversations" / "project" /
            "chat.conversation.json", std::ios::binary | std::ios::trunc};
        partial << "{\"messages\":[";
    }
    const auto partial = WindowsLMStudioConversationReader::read(
        fixture.path(), context.active());
    requireError(partial, Domain::ErrorCodes::MalformedMessage,
        "partial saved JSON was treated as a valid observation");
    require(partial.error().retryable, "partial JSON did not request a retry");

    ConversationFixture::write(fixture.root() / ".internal" /
        "conversation-config.json", Json{{"selectedConversation", "../outside.json"}});
    const auto escaped = WindowsLMStudioConversationReader::read(
        fixture.path(), context.active());
    requireError(escaped, Domain::ErrorCodes::PathOutsideAuthority,
        "selected file escaped the conversations repository");
}

void missingSelectionAndCancellation()
{
    ConversationFixture fixture;
    ConversationFixture::write(fixture.root() / ".internal" /
        "conversation-config.json", Json{{"selectedConversation", nullptr}});
    TestContext context;
    require(!take(WindowsLMStudioConversationReader::read(
        fixture.path(), context.active())).has_value(),
        "a missing selection produced a fabricated conversation");
    context.cancellation.request_stop();
    const auto cancelled = WindowsLMStudioConversationReader::read(
        fixture.path(), context.active());
    requireError(cancelled, Domain::ErrorCodes::Cancelled,
        "cancelled observation attempted to read the conversation");
}

class UnusedLegacyContinuity final : public Contracts::ILegacyContextContinuityService {
public:
    Domain::Result<Domain::LegacyContinuityPersistOutcome> checkpoint(
        const Domain::LegacyContinuityWriteRequest&, const Domain::ClientId&,
        Domain::LegacyHandoffSource, const Domain::OperationContext&) noexcept override
    { return unused<Domain::LegacyContinuityPersistOutcome>(); }
    Domain::Result<Domain::LegacyContinuityPersistOutcome> handoff(
        const Domain::LegacyContinuityWriteRequest&, const Domain::ClientId&,
        Domain::LegacyHandoffSource, const Domain::OperationContext&) noexcept override
    { return unused<Domain::LegacyContinuityPersistOutcome>(); }
    Domain::Result<Domain::LegacyContinuityPersistOutcome> automaticPersist(
        const Domain::LegacyContinuityAutomaticRequest&, const Domain::ClientId&,
        const Domain::OperationContext&) noexcept override
    { return unused<Domain::LegacyContinuityPersistOutcome>(); }
    Domain::Result<Domain::LegacyContinuityPersistOutcome> budgetHandoff(
        const Domain::ClientId&, std::string_view, const Domain::OperationContext&,
        const Domain::LegacyContinuityPatch&, std::optional<Domain::LegacyHandoffId>) noexcept override
    { return unused<Domain::LegacyContinuityPersistOutcome>(); }
    Domain::Result<Domain::LegacyContinuityGetOutcome> get(
        const Domain::LegacyContinuityGetRequest& request, const Domain::OperationContext&) noexcept override
    {
        if(record && request.handoffId && *request.handoffId==record->packet.id)
            return Domain::Result<Domain::LegacyContinuityGetOutcome>::success({record,true});
        for(const auto& retained:previousRecords) if(request.handoffId && *request.handoffId==retained.packet.id)
            return Domain::Result<Domain::LegacyContinuityGetOutcome>::success({retained,true});
        return unused<Domain::LegacyContinuityGetOutcome>();
    }
    Domain::Result<Domain::LegacyContinuityListOutcome> list(
        const Domain::LegacyContinuityListRequest&, const Domain::OperationContext&) noexcept override
    { return unused<Domain::LegacyContinuityListOutcome>(); }
    Domain::Result<Domain::LegacyContinuityProjectionRepairOutcome> repairProjections(
        const Domain::OperationContext&) noexcept override
    { return unused<Domain::LegacyContinuityProjectionRepairOutcome>(); }
    Domain::Result<Domain::LegacyContinuityResetOutcome> reset(
        const Domain::DestructiveConfirmation&, const Domain::OperationContext&) noexcept override
    { return unused<Domain::LegacyContinuityResetOutcome>(); }
    void shutdown() noexcept override {}
    std::optional<Domain::LegacyContinuityRecord> record;
    std::vector<Domain::LegacyContinuityRecord> previousRecords;
private:
    template <typename T> static Domain::Result<T> unused()
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Unexpected handoff during observation test."));
    }
};

class ContinuityObservationFixture final {
public:
    ContinuityObservationFixture()
    {
        const auto home = fileFixture.root() / "home";
        std::filesystem::create_directory(home);
        const auto homeText = home.generic_u8string();
        ConversationFixture::write(fileFixture.root() / "mcp.json", Json{{"mcpServers", {
            {"forge-conductor", {{"env", {{"FORGE_CONDUCTOR_HOME", std::string{
                reinterpret_cast<const char*>(homeText.data()), homeText.size()}}}}}},
            {"forge-conductor-fallback", Json::object()}, {"forge-conductor-clu", Json::object()}}}});
        const auto homePath = take(Domain::PathText::create(std::string{
            reinterpret_cast<const char*>(homeText.data()), homeText.size()}));
        configuration.reloadResult.set(Domain::Result<Domain::AppConfig>::success({}));
        projects.listRecentResult.set(Domain::Result<Domain::MemoryPage>::success(
            Domain::MemoryPage{project, {}, std::nullopt, false, 0U, 0U}));
        observer = std::make_unique<Infrastructure::Windows::WindowsLMStudioChatContinuity>(
            project, fileFixture.path(), homePath, fileFixture.path(), fileFixture.path(),
            Domain::LocalModelConfig{}, memory, legacyContinuity, projects, clock, uuid, configuration, true);
    }
    void partial() const
    {
        std::ofstream output{fileFixture.root() / "conversations" / "project" /
            "chat.conversation.json", std::ios::binary | std::ios::trunc};
        output << "{\"messages\":[";
        require(static_cast<bool>(output), "partial observation fixture write failed");
    }
    void complete() const
    {
        fileFixture.save(conversation(Json::array({message(Json::array({
            version(Json::array({generation(100U, 32768U)}))}))})));
    }
    Json await(const std::function<bool(const Json&)>& predicate) const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        do {
            auto status = Json::parse(observer->status());
            if (predicate(status)) return status;
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        } while (std::chrono::steady_clock::now() < deadline);
        throw TestFailure{"Continuity observation did not reach expected status: " + observer->status()};
    }
    ConversationFixture fileFixture;
    Domain::ProjectId project{parse<Domain::ProjectId>("5c06c108-26c9-4008-8123-2720fc8c9d97")};
    Infrastructure::Windows::SystemClock clock;
    Infrastructure::Windows::WindowsUuidGenerator uuid;
    Fakes::RecordingConfigurationStoreFake configuration;
    Fakes::RecordingProjectMemoryService projects;
    Fakes::LegacyMemoryServiceFake memory{8U, {"purge_legacy_memory", "all", "test-token"}};
    UnusedLegacyContinuity legacyContinuity;
    std::unique_ptr<Infrastructure::Windows::WindowsLMStudioChatContinuity> observer;
};

void noSelection(ContinuityObservationFixture& fixture, const std::optional<Json>& configuration)
{
    const auto path = fixture.fileFixture.root() / ".internal" / "conversation-config.json";
    if (configuration) ConversationFixture::write(path, *configuration);
    else require(std::filesystem::remove(path), "private conversation selection fixture was not removed");
}

Json awaitMeasured(ContinuityObservationFixture& fixture)
{
    return fixture.await([](const Json& status) {
        return status.contains("context_telemetry") && status["context_telemetry"].value("available", false);
    });
}

Json awaitNoSelection(ContinuityObservationFixture& fixture)
{
    return fixture.await([](const Json& status) {
        return status.contains("context_telemetry") &&
            !status["context_telemetry"].value("available", true) &&
            status["context_telemetry"].at("conversation_id").is_null();
    });
}

void noSelectionInvalidatesMeasuredTelemetry(const std::optional<Json>& configuration)
{
    ContinuityObservationFixture fixture;
    fixture.complete();fixture.observer->start();
    auto before = awaitMeasured(fixture);
    fixture.observer->shutdown();
    noSelection(fixture, configuration);
    fixture.observer->start();
    auto after = awaitNoSelection(fixture);
    fixture.observer->shutdown();
    const auto& telemetry = after.at("context_telemetry");
    for (const char* key : {"tokens_used", "context_capacity", "generation_reference", "headroom_tokens"})
        require(telemetry.at(key).is_null(), "no selection retained a previous generation measurement");
    require(!telemetry.at("overflow").get<bool>() && !telemetry.at("tools_active").get<bool>(),
        "no selection retained stale generation or active-tool state");
    require(telemetry.at("observed_at_unix_ms") > before.at("context_telemetry").at("observed_at_unix_ms"),
        "no-selection telemetry retained its old observation time");
    require(!telemetry.at("reason").get<std::string>().empty(), "unavailable no-selection telemetry has no reason");
    before.erase("context_telemetry");after.erase("context_telemetry");
    require(after == before, "no selection changed visible handoff availability, state, or binding status");
}

void noSelectionRecoversPreviousReadError()
{
    ContinuityObservationFixture fixture;
    fixture.partial();fixture.observer->start();
    fixture.await([](const Json& status) {
        return status.value("error", std::string{}).find("incomplete or invalid JSON") != std::string::npos;
    });
    fixture.observer->shutdown();
    noSelection(fixture, Json{{"selectedConversation", nullptr}});
    fixture.observer->start();
    const auto status = fixture.await([](const Json& observed) {
        return observed.contains("context_telemetry") &&
            !observed["context_telemetry"].value("available", true) && !observed.contains("error");
    });
    fixture.observer->shutdown();
    require(!status.contains("error"), "a successful null observation retained its previous read error");
}

void noSelectionPreservesOngoingPreferenceError()
{
    ContinuityObservationFixture fixture;
    fixture.complete();
    fixture.projects.listRecentResult.set(Domain::Result<Domain::MemoryPage>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, "Ongoing no-selection preference failure.")));
    fixture.observer->start();
    fixture.await([](const Json& status) {
        return status.value("error", std::string{}) == "Ongoing no-selection preference failure.";
    });
    fixture.observer->shutdown();
    noSelection(fixture, Json{{"selectedConversation", nullptr}});
    fixture.observer->start();
    const auto status = awaitNoSelection(fixture);
    fixture.observer->shutdown();
    require(status.value("error", std::string{}) == "Ongoing no-selection preference failure.",
        "no selection hid an ongoing preference failure");
}

void noSelectionRefreshesContinuityPreference()
{
    ContinuityObservationFixture fixture;
    fixture.complete();fixture.observer->start();
    const auto before = awaitMeasured(fixture);
    fixture.observer->shutdown();
    require(before.at("enabled") == true, "private observation fixture did not start with continuity enabled");
    const auto body = Json{{"provider_id", "lmstudio://http/127.0.0.1:1234/<automatic>"}, {"enabled", false}}.dump();
    Domain::ProjectMemoryRecord preference{
        parse<Domain::MemoryRecordId>("eb0a5ea3-c5cf-4f2a-a8be-4b484de62a6f"), fixture.project, 1U,
        "automatic_continuity_preference", "Private continuity preference", "Disable private continuity", body,
        {}, 1.0, 1.0, "test", std::nullopt, std::nullopt, {}, {}, {}, std::nullopt,
        parse<Domain::Sha256Digest>(std::string(64U, '0')), false};
    fixture.projects.listRecentResult.set(Domain::Result<Domain::MemoryPage>::success(
        Domain::MemoryPage{fixture.project, {{std::move(preference), 1.0}}, std::nullopt, false, body.size(), 256U * 1024U}));
    noSelection(fixture, Json{{"selectedConversation", nullptr}});
    fixture.observer->start();
    const auto status = fixture.await([](const Json& observed) {
        return observed.contains("context_telemetry") &&
            !observed["context_telemetry"].value("available", true) && observed.at("enabled") == false;
    });
    fixture.observer->shutdown();
    require(status.at("enabled") == false, "no selection retained a stale continuity preference");
}

void noSelectionPreservesWorkspaceControlError()
{
    ContinuityObservationFixture fixture;
    fixture.complete();
    const auto traceDirectory = fixture.fileFixture.root() / "home" / "continuity";
    {
        std::ofstream blocker{traceDirectory};blocker << "Private failed workspace binding trace.";
        require(static_cast<bool>(blocker), "private workspace binding blocker write failed");
    }
    const auto rebound = parse<Domain::ProjectId>("083f1a53-cda8-438e-9e04-5aaefb275a44");
    fixture.observer->bindWorkspace(rebound, fixture.fileFixture.path());fixture.observer->start();
    const auto before = fixture.await([&](const Json& status) {
        return status.contains("error") && status.value("project_id", std::string{}) == rebound.value();
    });
    fixture.observer->shutdown();
    require(std::filesystem::remove(traceDirectory), "private workspace binding blocker was not removed");
    noSelection(fixture, Json{{"selectedConversation", nullptr}});
    fixture.observer->start();
    const auto after = awaitNoSelection(fixture);
    fixture.observer->shutdown();
    for (const char* key : {"error", "state", "available", "project_id", "binding_source"})
        require(after.at(key) == before.at(key), "no selection changed an unresolved operational failure or binding state");
}

void noSelectionPreservesPendingWorkspaceBinding()
{
    ContinuityObservationFixture fixture;
    fixture.complete();fixture.observer->start();
    const auto before = awaitMeasured(fixture);fixture.observer->shutdown();
    const auto pending = parse<Domain::ProjectId>("aebbe194-23c9-4407-bd7a-228e26f31caa");
    fixture.observer->bindWorkspace(pending, fixture.fileFixture.path());
    noSelection(fixture, Json{{"selectedConversation", nullptr}});
    fixture.observer->start();const auto empty = awaitNoSelection(fixture);fixture.observer->shutdown();
    require(!empty.contains("project_id") && empty.at("state") == before.at("state"),
        "no selection applied or abandoned the pending authorized workspace binding");
    ConversationFixture::write(fixture.fileFixture.root() / ".internal" / "conversation-config.json",
        Json{{"selectedConversation", "project/chat.conversation.json"}});
    fixture.observer->start();
    const auto bound = fixture.await([&](const Json& status) {
        return status.value("project_id", std::string{}) == pending.value() &&
            status.contains("context_telemetry") && status["context_telemetry"].value("available", false);
    });
    fixture.observer->shutdown();
    require(bound.at("binding_source") == "authorized_mcp_workspace",
        "reselecting the chat lost the pending authorized workspace binding");
}

void selectedEmptyConversationRetainsIdentityWithoutUsage()
{
    ContinuityObservationFixture fixture;
    fixture.complete();fixture.observer->start();awaitMeasured(fixture);fixture.observer->shutdown();
    fixture.fileFixture.save(conversation(Json::array()));fixture.observer->start();
    const auto status = fixture.await([](const Json& observed) {
        return observed.contains("context_telemetry") &&
            !observed["context_telemetry"].value("available", true) &&
            observed["context_telemetry"].at("conversation_id") == "project/chat.conversation.json";
    });
    fixture.observer->shutdown();
    require(status.at("context_telemetry").at("tokens_used").is_null(),
        "a selected empty chat fabricated provider usage");
}

void successfulNullObservationStatusesAndRecovery()
{
    std::vector<std::string> failures;
    const auto run = [&](const char* name, const std::function<void()>& exercise) {
        try {exercise();std::cout << "[CASE PASS] continuity-null." << name << '\n';}
        catch (const std::exception& error) {
            failures.push_back(std::string{name} + ": " + error.what());
            std::cerr << "[CASE FAIL] continuity-null." << failures.back() << '\n';
        }
    };
    run("null_selector", [] {noSelectionInvalidatesMeasuredTelemetry(Json{{"selectedConversation", nullptr}});});
    run("missing_selector", [] {noSelectionInvalidatesMeasuredTelemetry(Json::object());});
    run("empty_selector", [] {noSelectionInvalidatesMeasuredTelemetry(Json{{"selectedConversation", ""}});});
    run("missing_config", [] {noSelectionInvalidatesMeasuredTelemetry(std::nullopt);});
    run("read_error_recovery", noSelectionRecoversPreviousReadError);
    run("ongoing_preference_failure", noSelectionPreservesOngoingPreferenceError);
    run("preference_refresh", noSelectionRefreshesContinuityPreference);
    run("operational_error_preserved", noSelectionPreservesWorkspaceControlError);
    run("pending_binding_preserved", noSelectionPreservesPendingWorkspaceBinding);
    run("selected_empty_chat_distinction", selectedEmptyConversationRetainsIdentityWithoutUsage);
    require(failures.empty(), "Successful-null continuity cases failed; inspect the per-case evidence above.");
}

void observerReconstructionRetainsWaitingPacketWithoutResending()
{
    using namespace Infrastructure::Windows;
    ContinuityObservationFixture fixture;
    {
        const auto encoded=(fixture.fileFixture.root()/"home").generic_u8string();
        const auto home=take(Domain::PathText::create(std::string{reinterpret_cast<const char*>(encoded.data()),encoded.size()}));
        const Json suppliedScope{{"current_caller","private-observer-reconstruction"}};
        TestContext operation;
        Detail::LMStudioChatCheckpoint checkpoint{home,fixture.project,suppliedScope,operation.active()};
        require(checkpoint.scope().is_object() && checkpoint.scope()==suppliedScope,
            "checkpoint list initialization wrapped the freshly supplied authority-evidence scope");
    }
    std::size_t sentMessages{};
    auto controls=std::make_shared<Detail::LMStudioChatControlActions>();
    controls->activate=[](const auto&,const auto&) {return Domain::Result<void>::success();};
    controls->idle=[](const auto&,const auto&) {return Domain::Result<bool>::success(true);};
    controls->pause=[](const auto&,std::string_view,const auto&) {return Domain::Result<bool>::success(true);};
    controls->send=[&](const Domain::PathText&,std::string_view text,bool newChat,
        const Domain::OperationContext& operation,std::optional<std::string_view> expected,
        const std::function<void(std::string_view)>&,const LMStudioChatEffectObserver& receipt) {
        require(!newChat && expected==std::optional<std::string_view>{"project/chat.conversation.json"},
            "private packet request addressed the wrong chat or created a successor");
        const auto observed=take(WindowsLMStudioConversationReader::read(fixture.fileFixture.path(),operation));
        require(observed.has_value(),"private packet request had no selected chat");
        const auto boundary=observed->userMessages.size();
        if(receipt) {
            auto recorded=receipt({LMStudioChatEffect::Send,LMStudioChatEffectStage::BeforeDispatch,observed->conversationId,boundary});
            if(!recorded) return recorded;
        }
        std::ifstream input{fixture.fileFixture.root()/"conversations"/"project"/"chat.conversation.json",std::ios::binary};
        auto saved=Json::parse(input);
        saved["messages"].push_back(message(Json::array({Json{{"type","singleStep"},{"role","user"},
            {"content",Json::array({Json{{"type","text"},{"text",text}}})}}})));
        fixture.fileFixture.save(saved);++sentMessages;
        if(receipt) return receipt({LMStudioChatEffect::Send,LMStudioChatEffectStage::Confirmed,observed->conversationId,boundary});
        return Domain::Result<void>::success();
    };
    const auto create=[&] {
        const auto encoded=(fixture.fileFixture.root()/"home").generic_u8string();
        auto home=take(Domain::PathText::create(std::string{reinterpret_cast<const char*>(encoded.data()),encoded.size()}));
        return Detail::LMStudioChatContinuityAccess::create(controls,fixture.project,fixture.fileFixture.path(),home,
            fixture.fileFixture.path(),fixture.fileFixture.path(),Domain::LocalModelConfig{},fixture.memory,
            fixture.legacyContinuity,fixture.projects,fixture.clock,fixture.uuid,fixture.configuration,true);
    };
    fixture.observer.reset();fixture.observer=create();
    fixture.fileFixture.save(conversation(Json::array({message(Json::array({version(Json::array({generation(31000U,32768U)}))}))})));
    fixture.observer->start();
    const auto waiting=fixture.await([](const Json& status) {return status.value("state",std::string{})=="waiting_for_model_packet";});
    fixture.observer->shutdown();
    require(sentMessages==1U,"private observer did not dispatch exactly one confirmed packet request");
    TestContext readContext;
    const auto persisted=take(WindowsLMStudioConversationReader::read(fixture.fileFixture.path(),readContext.active()));
    require(persisted && persisted->userMessages.size()==1U && persisted->userMessages.front().starts_with("Auto Continuity:"),
        "private confirmed packet request was not persisted as native user evidence");
    fixture.observer.reset();fixture.observer=create();fixture.observer->start();
    fixture.await([&](const Json& status) {
        return status.value("state",std::string{})=="waiting_for_model_packet" &&
            status.contains("context_telemetry") && status["context_telemetry"]["observed_at_unix_ms"]>
                waiting["context_telemetry"]["observed_at_unix_ms"];
    });
    fixture.observer->shutdown();
    require(sentMessages==1U,"reconstructing the observer lost its verified waiting phase and sent the packet request again");
}

class VisibleHandoffFixture final {
public:
    using Effect=Infrastructure::Windows::LMStudioChatEffect;
    using Stage=Infrastructure::Windows::LMStudioChatEffectStage;
    enum class Fault {None,UndispatchedRequest,AmbiguousRequest,BeforeDispatchStorage,AfterDispatchStorage,UncertainNew,StableNewUndelivered,AmbiguousDelivery};
    VisibleHandoffFixture() {
        controls=std::make_shared<Infrastructure::Windows::Detail::LMStudioChatControlActions>();
        controls->activate=[](const auto&,const auto&) {return Domain::Result<void>::success();};
        controls->idle=[](const auto&,const auto&) {return Domain::Result<bool>::success(true);};
        controls->pause=[](const auto&,std::string_view,const auto&) {return Domain::Result<bool>::success(true);};
        controls->send=[this](const Domain::PathText&,std::string_view text,bool newChat,
            const Domain::OperationContext& operation,std::optional<std::string_view> expected,
            const std::function<void(std::string_view)>& successor,
            const Infrastructure::Windows::LMStudioChatEffectObserver& receipt) {
            require(receipt && expected==selected,"private durable control lost its receipt or exact target");
            if(!newChat && fault==Fault::UndispatchedRequest) return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,"The selected LM Studio chat exceeds the 32 MiB integration-field update bound; no field was changed."));
            const auto acknowledge=[&](Effect effect,Stage stage,const std::string& id,std::size_t boundary) {
                return receipt({effect,stage,id,boundary});
            };
            if(newChat) {
                auto recorded=acknowledge(Effect::NewChat,Stage::BeforeDispatch,selected,0U);
                if(!recorded) return recorded;
                ++creations;
                select("project/successor.conversation.json",conversation(Json::array()));
                if(fault==Fault::UncertainNew) return ambiguous();
                recorded=acknowledge(Effect::NewChat,Stage::Confirmed,selected,0U);
                if(!recorded) return recorded;
                if(successor) successor(selected);
                if(fault==Fault::StableNewUndelivered) return ambiguous();
            }
            const auto chat=take(WindowsLMStudioConversationReader::read(fixture.fileFixture.path(),operation));
            require(chat && chat->conversationId==selected,"private durable Send observed another target");
            const auto boundary=chat->userMessages.size();
            if(fault==Fault::BeforeDispatchStorage) denyCheckpointPublication();
            auto recorded=acknowledge(Effect::Send,Stage::BeforeDispatch,selected,boundary);
            if(!recorded) return recorded;
            ++sends;lastText=text;
            if(fault==Fault::AmbiguousRequest || fault==Fault::AmbiguousDelivery) return ambiguous();
            append(message(Json::array({Json{{"type","singleStep"},{"role","user"},
                {"content",Json::array({Json{{"type","text"},{"text",std::string{text}}}})}}})));
            if(fault==Fault::AfterDispatchStorage) denyCheckpointPublication();
            return acknowledge(Effect::Send,Stage::Confirmed,selected,boundary);
        };
        reconstruct();
    }
    ~VisibleHandoffFixture() {if(fixture.observer) fixture.observer->shutdown();publicationBlock.reset();}
    std::unique_ptr<Infrastructure::Windows::WindowsLMStudioChatContinuity> create(bool confirmed=true) {
        return create(fixture.projects,fixture.configuration,confirmed);
    }
    std::unique_ptr<Infrastructure::Windows::WindowsLMStudioChatContinuity> create(
        Contracts::IProjectMemoryService& projects,Contracts::IConfigurationStore& configuration,bool confirmed=true) {
        const auto encoded=(fixture.fileFixture.root()/"home").generic_u8string();
        auto home=take(Domain::PathText::create(std::string{reinterpret_cast<const char*>(encoded.data()),encoded.size()}));
        return Infrastructure::Windows::Detail::LMStudioChatContinuityAccess::create(controls,fixture.project,
            fixture.fileFixture.path(),home,fixture.fileFixture.path(),fixture.fileFixture.path(),Domain::LocalModelConfig{},
            fixture.memory,fixture.legacyContinuity,projects,fixture.clock,fixture.uuid,configuration,confirmed);
    }
    void reconstruct(bool confirmed=true) {
        fixture.observer.reset();fixture.observer=create(confirmed);
    }
    Json run(const std::function<bool(const Json&)>& predicate) {
        fixture.observer->start();auto status=fixture.await(predicate);fixture.observer->shutdown();return status;
    }
    Json pending() {return run([](const Json& status) {return status.value("state",std::string{})=="recovery_pending";});}
    void waiting() {
        fixture.fileFixture.save(conversation(Json::array({message(Json::array({version(Json::array({generation(31000U,32768U)}))}))})));
        run([](const Json& status) {return status.value("state",std::string{})=="waiting_for_model_packet";});
        require(sends==1U && creations==0U,"private fixture did not acknowledge one initial packet request");
    }
    void undispatched() {
        fault=Fault::UndispatchedRequest;
        fixture.fileFixture.save(conversation(Json::array({message(Json::array({version(Json::array({generation(31000U,32768U)}))}))})));
        run([&](const Json& status) {
            const auto error=status.value("error",std::string{});
            if(error.find("32 MiB")==std::string::npos) return false;
            try {
                const auto saved=checkpoint();const auto& state=saved.at("state");
                return saved.at("scope").at("project_id")==fixture.project.value() && state.at("phase")==1U &&
                    state.at("effect").is_null() && !state.at("packet_request_acknowledged").get<bool>() &&
                    state.at("dispatch_error")==error && state.at("operational_error")==error;
            } catch(...) {return false;}
        });
        require(sends==0U && creations==0U && checkpoint()["state"]["effect"].is_null(),"private request failure dispatched an effect");
        fault=Fault::None;
    }
    Domain::PathText home() const {
        const auto text=(fixture.fileFixture.root()/"home").generic_u8string();
        return take(Domain::PathText::create(std::string{reinterpret_cast<const char*>(text.data()),text.size()}));
    }
    Json upgradeRoutes(bool globallyBound=false) {
        std::wstring module(32768U,L'\0');const auto size=::GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
        require(size>0U && size<module.size(),"private current module path is unavailable");module.resize(size);
        const auto binary=take(Domain::PathText::create(take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(module))));
        const auto bytes=take(Infrastructure::Windows::LMStudioConfigurationCodec::mergeForgeServers(
            Infrastructure::Windows::LMStudioConfigurationCodec::empty(),binary,home(),
            parse<Domain::DeploymentId>("0b6eadbe-c35a-4d41-8c6e-6d8c40b68dc2"),
            globallyBound?std::nullopt:std::optional<Domain::ProjectId>{fixture.project},
            globallyBound?std::nullopt:std::optional<Domain::PathText>{fixture.fileFixture.path()}));
        auto routes=Json::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data())+bytes.size());
        ConversationFixture::write(fixture.fileFixture.root()/"mcp.json",routes);return routes;
    }
    Json nativeHandoff() const {
        TestContext operation;const auto chat=take(WindowsLMStudioConversationReader::read(fixture.fileFixture.path(),operation.active()));
        require(chat.has_value(),"private native handoff chat is missing");
        std::optional<Json> latest;
        for(const auto& result:chat->nativeToolResults) if(result.name=="session_handoff")
            for(const auto& body:result.textBodies) latest=Json::parse(body);
        if(latest) return *latest;
        throw TestFailure{"Private native handoff result is missing."};
    }
    void queueHandoff(const Json& result) {fixture.observer->recordTool("session_handoff",true,result.dump());}
    std::filesystem::path checkpointPath() const {
        return fixture.fileFixture.root()/"home"/"continuity"/("lmstudio-visible-"+fixture.project.value()+".checkpoint");
    }
    void awaitWriterOwnership() const {
        const auto path=checkpointPath().parent_path()/"lmstudio-visible-writer.lock";
        require(std::filesystem::is_regular_file(path),"private initialized writer lock is missing");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{10};
        do {
            Infrastructure::Windows::Detail::UniqueHandle probe{::CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,
                0U,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
            const auto error=probe ? ERROR_SUCCESS : ::GetLastError();
            if(!probe && error==ERROR_SHARING_VIOLATION) return;
            require(static_cast<bool>(probe),"private writer ownership probe failed unexpectedly: "+std::to_string(error));
            probe.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        } while(std::chrono::steady_clock::now()<deadline);
        throw TestFailure{"First observer did not acquire the private native writer lease."};
    }
    Json checkpoint() const {
        std::ifstream input{checkpointPath(),std::ios::binary};
        std::vector<char> bytes{std::istreambuf_iterator<char>{input},{}};
        require(!bytes.empty(),"private durable checkpoint is missing");
        return take(Infrastructure::Windows::Detail::LMStudioChatCheckpoint::open(std::as_bytes(std::span{bytes})));
    }
    void writeCheckpoint(const Json& value) {
        const auto stored=take(Infrastructure::Windows::Detail::LMStudioChatCheckpoint::seal(value));
        std::ofstream output{checkpointPath(),std::ios::binary|std::ios::trunc};
        output.write(reinterpret_cast<const char*>(stored.data()),static_cast<std::streamsize>(stored.size()));
        require(static_cast<bool>(output),"private checkpoint rewrite failed");
    }
    void append(const Json& value) {
        const auto path=fixture.fileFixture.root()/"conversations"/std::filesystem::path{selected};
        std::ifstream input{path,std::ios::binary};auto saved=Json::parse(input);input.close();
        saved["messages"].push_back(value);ConversationFixture::write(path,saved);
    }
    void select(const std::string& id,const Json& value) {
        selected=id;ConversationFixture::write(fixture.fileFixture.root()/"conversations"/std::filesystem::path{id},value);
        ConversationFixture::write(fixture.fileFixture.root()/".internal"/"conversation-config.json",Json{{"selectedConversation",id}});
    }
    static Json tool(std::string_view name,const Json& payload,unsigned call) {
        const auto id="durable-native-"+std::to_string(call);
        return Json{{"type","contentBlock"},{"content",Json::array({
            Json{{"type","toolCallRequest"},{"callId",call},{"name",name},{"toolCallRequestId",id},{"pluginIdentifier","mcp/forge-conductor"}},
            Json{{"type","toolCallResult"},{"callId",call},{"toolCallRequestId",id},
                {"content",Json::array({Json{{"type","text"},{"text",payload.dump()}}}).dump()}}})}};
    }
    void saveModelPacket(std::string_view packetId="durable-native-packet",std::uint64_t sequence=1U) {
        Domain::LegacyHandoffPacket packet{parse<Domain::LegacyHandoffId>(packetId)};
        packet.resumeReady=true;packet.goal="Recover the exact original task after the visible connector restart";
        packet.narrative="The private observer has measured a real selected provider generation, sent exactly one packet request, and retained the original project folder and Forge integrations. The next step must preserve native tool evidence, pending actions and explicit constraints while resuming the same durable packet after reconstruction.";
        packet.resumeSeed=packet.narrative+" Read this packet with context_get and run agent_list afterward.";
        packet.keyFiles={fixture.fileFixture.path().value()};packet.decisions={"Do not repeat uncertain New chat or Send effects"};
        packet.nextActions={"Read the retained packet with context_get","Run agent_list after recovery"};
        Json body{{"meta",{{"id",packet.id.value()},{"source","model"},{"resume_ready",true}}},
            {"task",{{"goal",packet.goal},{"status",packet.status},{"next_actions",packet.nextActions},{"blockers",packet.blockers}}},
            {"working_set",{{"key_files",packet.keyFiles},{"decisions",packet.decisions}}},
            {"resume",{{"seed",packet.resumeSeed}}},{"narrative",packet.narrative},{"agents",Json::array()}};
        if(fixture.legacyContinuity.record) fixture.legacyContinuity.previousRecords.push_back(*fixture.legacyContinuity.record);
        fixture.legacyContinuity.record=Domain::LegacyContinuityRecord{packet,sequence,{}};
        TestContext operation;
        const auto savedPointer=take(fixture.memory.set({"continuity/project/"+fixture.project.value(),packet.id.value(),{}},operation.active()));
        require(savedPointer.stored && savedPointer.note.body==packet.id.value(),"private model packet pointer was not saved exactly");
        append(message(Json::array({version(Json::array({tool("session_handoff",Json{{"ok",true},
            {"handoff_id",packet.id.value()},{"resume_seed",packet.resumeSeed},{"packet",body}},static_cast<unsigned>(sequence))}))})));
    }
    void nativeResume() {
        append(message(Json::array({version(Json::array({
            tool("context_get",Json{{"ok",true},{"found",true},{"handoff_id","durable-native-packet"}},2U),
            tool("agent_list",Json{{"ok",true}},3U),generation(3000U,32768U)}))})));
    }
    Json fullResumeReceipt() const {
        require(fixture.legacyContinuity.record.has_value(),"private retained packet is missing");
        return Json{{"ok",true},{"found",true},{"handoff_id",fixture.legacyContinuity.record->packet.id.value()},
            {"resume_seed",fixture.legacyContinuity.record->packet.resumeSeed},{"packet",checkpoint().at("state").at("handed")}};
    }
    Json nativeFullResume(bool following=true,unsigned callId=20U) {
        const auto result=fullResumeReceipt();
        auto tools=Json::array({tool("context_get",result,callId)});
        if(following) tools.push_back(tool("agent_list",Json{{"ok",true}},callId+1U));
        tools.push_back(generation(3000U,32768U));append(message(Json::array({version(tools)})));return result;
    }
    void denyCheckpointPublication() {
        publicationBlock.reset(::CreateFileW(checkpointPath().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
            OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        require(static_cast<bool>(publicationBlock),"private storage failure could not lock the old checkpoint");
    }
    static Domain::Result<void> ambiguous() {return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::AcknowledgementTimeout,"Private control dispatch acknowledgement is uncertain.",true));}
    ContinuityObservationFixture fixture;
    std::shared_ptr<Infrastructure::Windows::Detail::LMStudioChatControlActions> controls;
    std::atomic<std::size_t> sends{},creations{};
    std::string selected{"project/chat.conversation.json"},lastText;
    Fault fault{Fault::None};
    Infrastructure::Windows::Detail::UniqueHandle publicationBlock;
};

Json visibleContinuityTraceEvent(const VisibleHandoffFixture& fixture, const std::string_view event,
    const std::size_t expectedMatches=1U)
{
    const auto path=fixture.fixture.fileFixture.root()/"home"/"continuity"/
        ("lmstudio-chat-trace-"+std::to_string(::GetCurrentProcessId())+".jsonl");
    std::ifstream input{path,std::ios::binary};
    require(static_cast<bool>(input),"private visible continuity trace is missing");
    Json found;std::size_t matches{};std::string line;
    while(std::getline(input,line)) {
        const auto value=Json::parse(line);
        if(value.value("event",std::string{})==event) {found=value;++matches;}
    }
    require(matches==expectedMatches,"private continuity trace did not retain exactly "+
        std::to_string(expectedMatches)+" "+std::string{event}+" events");
    return found;
}

void cachedPromptPressureRequestsPacketWithSeparateProviderEvidence()
{
    VisibleHandoffFixture f;
    f.fixture.fileFixture.save(renderedPromptConversation(264415U));
    TestContext operation;
    const auto initial=take(WindowsLMStudioConversationReader::read(f.fixture.fileFixture.path(),operation.active()));
    require(initial && !initial->generationEvidence.empty(),"private pressure fixture has no selected provider generation");
    const auto initialGeneration=Json::parse(initial->generationEvidence);
    const auto status=f.run([](const Json& observed) {
        return observed.value("state",std::string{})=="waiting_for_model_packet";
    });
    require(f.sends==1U && f.creations==0U,"cached prompt pressure did not request exactly one model packet without creating a chat");
    const auto& telemetry=status.at("context_telemetry");
    require(telemetry.at("tokens_used")==130969U && telemetry.at("context_capacity")==262144U &&
        telemetry.at("headroom_tokens")==120935U && !telemetry.at("overflow").get<bool>(),
        "cached prompt pressure replaced measured provider usage, headroom, or physical overflow");
    require(telemetry.at("cached_rendered_prompt_tokens")==264415U && telemetry.at("pressure_tokens")==264415U &&
        telemetry.at("pressure_source")=="cached_rendered_prompt" && telemetry.at("pressure_headroom_tokens")==0U,
        "cached full prompt projection did not independently explain rollover pressure");
    const auto note=telemetry.at("cached_prompt_note").get<std::string>();
    require(note.find("before")!=std::string::npos && note.find("after")!=std::string::npos &&
        note.find("not current KV usage")!=std::string::npos,
        "cached prompt telemetry omitted its refresh boundary or current-KV limitation");
    require(f.lastText.find("Provider measured 130969 tokens of 262144")!=std::string::npos &&
        f.lastText.find("cached full rendered prompt")!=std::string::npos &&
        f.lastText.find("264415")!=std::string::npos && f.lastText.find("not current KV usage")!=std::string::npos,
        "the model packet request conflated cached projected tokens with actual provider measurement");
    for(const auto event:{"context_pressure_detected","context_pressure_pause"}) {
        const auto trace=visibleContinuityTraceEvent(f,event);
        require(trace.at("provider_used")==130969U && trace.at("cached_rendered_prompt_tokens")==264415U &&
            trace.at("pressure_tokens")==264415U && trace.at("pressure_source")=="cached_rendered_prompt" &&
            !trace.at("overflow").get<bool>(),"pressure trace did not retain distinct cached and provider evidence");
        require(trace.at("generation_evidence")==initialGeneration,
            "pressure trace did not retain the exact selected provider generation and model evidence");
    }
}

void cachedPromptPressureUsesMaximumWithoutPromotingInvalidCache()
{
    for(const Json count:{Json(0U),Json(130968U),Json(130969U),Json(180000U),Json(nullptr),Json(-1),Json("264415")}) {
        VisibleHandoffFixture f;f.fixture.fileFixture.save(renderedPromptConversation(count));
        const auto status=f.run([](const Json& observed) {
            return observed.contains("context_telemetry") && observed["context_telemetry"].value("available",false);
        });
        const auto& telemetry=status.at("context_telemetry");
        const bool larger=count==Json(180000U);
        const auto pressure=larger?180000U:130969U;
        require(f.sends==0U && f.creations==0U && status.at("state")=="observing",
            "a safe or invalid cached projection dispatched a model request");
        require(telemetry.at("tokens_used")==130969U && telemetry.at("headroom_tokens")==120935U &&
            telemetry.at("pressure_tokens")==pressure && telemetry.at("pressure_headroom_tokens")==
                (larger?71904U:120935U) && telemetry.at("pressure_source")==
                (larger?"cached_rendered_prompt":"latest_provider_generation"),
            "cached projection reduced actual usage or failed to preserve the larger safe pressure signal");
        if(count.is_number_unsigned()) require(telemetry.at("cached_rendered_prompt_tokens")==count,
            "an admitted safe cache disappeared from telemetry");
        else require(telemetry.at("cached_rendered_prompt_tokens").is_null(),
            "an invalid cache became pressure telemetry");
    }
    VisibleHandoffFixture f;auto document=renderedPromptConversation(264415U);
    document["lastUsedModel"]["identifier"]="different-model";f.fixture.fileFixture.save(document);
    const auto status=f.run([](const Json& observed) {return observed.contains("context_telemetry") &&
        observed["context_telemetry"].value("available",false);});
    require(f.sends==0U && status["context_telemetry"]["cached_rendered_prompt_tokens"].is_null() &&
        status["context_telemetry"]["pressure_tokens"]==130969U,
        "a mismatched model cache triggered visible continuity pressure");
}

void cachedPromptPressurePreservesProviderPressureAndOverflow()
{
    for(const bool overflow:{false,true}) {
        VisibleHandoffFixture f;auto document=renderedPromptConversation(1000U);
        auto& stats=document["messages"][0]["versions"][0]["steps"][0]["genInfo"]["stats"];
        stats["totalTokensCount"]=overflow?130969U:240000U;
        stats["stopReason"]=overflow?"contextLengthReached":"eosFound";
        f.fixture.fileFixture.save(document);
        const auto status=f.run([](const Json& observed) {return observed.value("state",std::string{})=="waiting_for_model_packet";});
        const auto& telemetry=status.at("context_telemetry");
        require(f.sends==1U && f.creations==0U && telemetry.at("tokens_used")==stats.at("totalTokensCount") &&
            telemetry.at("cached_rendered_prompt_tokens")==1000U && telemetry.at("overflow")==overflow &&
            telemetry.at("pressure_source")==(overflow?"provider_overflow":"latest_provider_generation"),
            "a smaller cache suppressed actual provider pressure or changed the physical overflow source");
        require(telemetry.at("pressure_tokens")==static_cast<unsigned>(overflow?262144U:240000U) &&
            telemetry.at("pressure_headroom_tokens")==static_cast<unsigned>(overflow?0U:11904U),
            "provider overflow pressure telemetry contradicted the resolved emergency budget");
        for(const auto event:{"context_pressure_detected","context_pressure_pause"}) {
            const auto trace=visibleContinuityTraceEvent(f,event);
            require(trace.at("provider_used")==stats.at("totalTokensCount") &&
                trace.at("cached_rendered_prompt_tokens")==1000U &&
                trace.at("pressure_tokens")==static_cast<unsigned>(overflow?262144U:240000U) &&
                trace.at("remaining")==static_cast<unsigned>(overflow?0U:11904U) &&
                trace.at("pressure_action")==(overflow?"emergency":"rollover"),
                "physical overflow trace lost its actual provider usage or emergency pressure budget");
        }
    }
}

void cachedPromptPressureRereadsProjectionAtConfirmedPause()
{
    for(const auto change:{"lower","absent","model_mismatch","capacity_mismatch","no_provider_evidence","no_capacity","still_high","new_generation"}) {
        VisibleHandoffFixture f;f.fixture.fileFixture.save(renderedPromptConversation(264415U));
        TestContext operation;
        const auto initial=take(WindowsLMStudioConversationReader::read(f.fixture.fileFixture.path(),operation.active()));
        require(initial && !initial->generationEvidence.empty(),"private pause fixture has no initial selected provider generation");
        const auto initialGeneration=Json::parse(initial->generationEvidence);
        const bool requestsPacket=std::string_view{change}=="still_high" || std::string_view{change}=="new_generation";
        Json pauseGeneration;
        std::atomic<std::size_t> pauses{};
        f.controls->pause=[&](const auto&,std::string_view,const auto&) {
            auto fresh=renderedPromptConversation(requestsPacket?240000U:0U);
            if(std::string_view{change}=="new_generation") {
                auto newer=fresh["messages"][0]["versions"][0];
                newer["steps"][0]["genInfo"]["stats"]["totalTokensCount"]=130970U;
                newer["steps"][0]["genInfo"]["native_unknown_field"]=Json{{"preserved",true}};
                fresh["messages"][0]["versions"].push_back(std::move(newer));
                fresh["messages"][0]["currentlySelected"]=1U;
            }
            if(std::string_view{change}=="absent") fresh.erase("tokenCount");
            if(std::string_view{change}=="model_mismatch") {
                fresh["tokenCount"]=264415U;fresh["lastUsedModel"]["identifier"]="different-model";
            }
            if(std::string_view{change}=="capacity_mismatch") {
                fresh["tokenCount"]=264415U;
                fresh["lastUsedModel"]["instanceLoadTimeConfig"]["fields"][0]["value"]=131072U;
            }
            if(std::string_view{change}=="no_provider_evidence") {
                auto& stats=fresh["messages"][0]["versions"][0]["steps"][0]["genInfo"]["stats"];
                stats.erase("totalTokensCount");stats["stopReason"]="contextLengthReached";
            }
            if(std::string_view{change}=="no_capacity") {
                fresh["messages"][0]["versions"][0]["steps"][0]["genInfo"].erase("loadModelConfig");
                fresh["lastUsedModel"].erase("instanceLoadTimeConfig");
            }
            f.fixture.fileFixture.save(fresh);
            if(requestsPacket) {
                TestContext pausedOperation;
                const auto paused=take(WindowsLMStudioConversationReader::read(f.fixture.fileFixture.path(),pausedOperation.active()));
                require(paused && !paused->generationEvidence.empty(),"private pause fixture lost its selected provider generation");
                pauseGeneration=Json::parse(paused->generationEvidence);
            }
            ++pauses;return Domain::Result<bool>::success(true);
        };
        if(requestsPacket) {
            f.run([](const Json& status) {return status.value("state",std::string{})=="waiting_for_model_packet";});
            require(pauses==1U && f.sends==1U && f.creations==0U &&
                f.lastText.find("240000")!=std::string::npos && f.lastText.find("264415")==std::string::npos,
                "the confirmed-pause packet request used the stale pre-pause projection");
            const auto trace=visibleContinuityTraceEvent(f,"context_pressure_pause");
            require(trace.at("cached_rendered_prompt_tokens")==240000U && trace.at("pressure_tokens")==240000U,
                "the pause trace did not use the fresh admitted projection");
            const auto detected=visibleContinuityTraceEvent(f,"context_pressure_detected");
            require(detected.at("generation_evidence")==initialGeneration &&
                trace.at("generation_evidence")==pauseGeneration,
                "pressure trace conflated initial detection with the freshly selected generation at pause");
            if(std::string_view{change}=="new_generation") require(pauseGeneration!=initialGeneration &&
                pauseGeneration.at("selected_version")==1U &&
                pauseGeneration.at("genInfo").at("native_unknown_field").at("preserved")==true &&
                trace.at("provider_used")==130970U,
                "the new selected pause generation was reduced to counts or lost unknown native fields");
        } else {
            const bool lostProvider=std::string_view{change}=="no_provider_evidence" || std::string_view{change}=="no_capacity";
            const auto status=f.run([&](const Json& observed) {
                return pauses.load()>0U && observed.value("state",std::string{})=="observing" &&
                    observed.contains("context_telemetry") && observed["context_telemetry"].contains("pressure_tokens") &&
                    (lostProvider?(!observed["context_telemetry"].value("available",true) &&
                        observed["context_telemetry"]["pressure_tokens"].is_null()):
                        observed["context_telemetry"]["pressure_tokens"]==130969U);
            });
            require(pauses==1U && f.sends==0U && f.creations==0U &&
                !status.contains("error") && (lostProvider?status["context_telemetry"]["pressure_source"].is_null():
                    status["context_telemetry"]["pressure_source"]=="latest_provider_generation"),
                "a withdrawn projection or lost provider observation dispatched a packet or became an operational error after pause");
        }
    }
}

void cachedPromptPressureTraceDeduplicatesAndReentersAfterNormal()
{
    VisibleHandoffFixture f;auto document=renderedPromptConversation(264415U);
    document["messages"][0]["versions"][0]["steps"].push_back(Json{{"type","toolStatus"},
        {"statusState",{{"status",{{"type","callingTool"}}}}}});
    f.fixture.fileFixture.save(document);std::atomic<std::size_t> pauses{};
    f.controls->pause=[&](const auto&,std::string_view,const auto&) {++pauses;return Domain::Result<bool>::success(true);};
    const auto first=f.run([](const Json& observed) {return observed.contains("context_telemetry") &&
        observed["context_telemetry"].value("tools_active",false);});
    visibleContinuityTraceEvent(f,"context_pressure_detected");
    const auto same=f.run([&](const Json& observed) {return observed.contains("context_telemetry") &&
        observed["context_telemetry"]["observed_at_unix_ms"]>first["context_telemetry"]["observed_at_unix_ms"];});
    visibleContinuityTraceEvent(f,"context_pressure_detected");
    TestContext operation;
    const auto initial=take(WindowsLMStudioConversationReader::read(f.fixture.fileFixture.path(),operation.active()));
    require(initial.has_value(),"private cached-pressure trace fixture lost its provider observation");
    document["tokenCount"]=0U;f.fixture.fileFixture.save(document);
    f.run([&](const Json& observed) {return observed.contains("context_telemetry") &&
        observed["context_telemetry"]["observed_at_unix_ms"]>same["context_telemetry"]["observed_at_unix_ms"] &&
        observed["context_telemetry"]["pressure_source"]=="latest_provider_generation";});
    visibleContinuityTraceEvent(f,"context_pressure_detected");
    document["tokenCount"]=264415U;f.fixture.fileFixture.save(document);
    const auto changed=take(WindowsLMStudioConversationReader::read(f.fixture.fileFixture.path(),operation.active()));
    require(changed && changed->generationEvidence==initial->generationEvidence,
        "pressure trace re-entry accidentally changed the selected provider generation");
    const auto reentered=f.run([](const Json& observed) {return observed.contains("context_telemetry") &&
        observed["context_telemetry"]["cached_rendered_prompt_tokens"]==264415U &&
        observed["context_telemetry"]["pressure_source"]=="cached_rendered_prompt";});
    const auto trace=visibleContinuityTraceEvent(f,"context_pressure_detected",2U);
    require(trace.at("cached_rendered_prompt_tokens")==264415U && trace.at("provider_used")==130969U,
        "a return to the identical cached count after Normal did not emit fresh pressure evidence");
    f.run([&](const Json& observed) {return observed.contains("context_telemetry") &&
        observed["context_telemetry"]["observed_at_unix_ms"]>reentered["context_telemetry"]["observed_at_unix_ms"];});
    visibleContinuityTraceEvent(f,"context_pressure_detected",2U);
    document["tokenCount"]=300000U;f.fixture.fileFixture.save(document);
    f.run([](const Json& observed) {return observed.contains("context_telemetry") &&
        observed["context_telemetry"]["cached_rendered_prompt_tokens"]==300000U;});
    const auto higher=visibleContinuityTraceEvent(f,"context_pressure_detected",3U);
    require(higher.at("cached_rendered_prompt_tokens")==300000U && higher.at("pressure_tokens")==300000U &&
        pauses==0U && f.sends==0U && f.creations==0U,
        "a cache-only pressure increase was untraced or crossed the active-tool control boundary");
}

void cachedPromptPressureWaitsForActiveTools()
{
    VisibleHandoffFixture f;auto document=renderedPromptConversation(264415U);
    document["messages"][0]["versions"][0]["steps"].push_back(Json{{"type","toolStatus"},
        {"statusState",{{"status",{{"type","callingTool"}}}}}});
    f.fixture.fileFixture.save(document);std::atomic<std::size_t> pauses{};
    f.controls->pause=[&](const auto&,std::string_view,const auto&) {++pauses;return Domain::Result<bool>::success(true);};
    const auto status=f.run([](const Json& observed) {return observed.contains("context_telemetry") &&
        observed["context_telemetry"].value("tools_active",false);});
    require(pauses==0U && f.sends==0U && f.creations==0U && status.at("state")=="observing",
        "cached prompt pressure paused or requested a packet while a native tool was active");
}

void cachedPromptPressureControlsSuccessorBudgetClear()
{
    for(const auto cached:{1000U,264415U}) {
        VisibleHandoffFixture f;f.waiting();f.saveModelPacket();
        f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        const auto measurement=renderedPromptConversation(cached);
        f.append(message(Json::array({version(Json::array({
            VisibleHandoffFixture::tool("context_get",Json{{"ok",true},{"found",true},{"handoff_id","durable-native-packet"}},2U),
            VisibleHandoffFixture::tool("agent_list",Json{{"ok",true}},3U),
            measurement["messages"][0]["versions"][0]["steps"][0]}))})));
        const auto path=f.fixture.fileFixture.root()/"conversations"/std::filesystem::path{f.selected};
        std::ifstream input{path,std::ios::binary};auto saved=Json::parse(input);input.close();
        saved["lastUsedModel"]=measurement["lastUsedModel"];saved["tokenCount"]=cached;
        ConversationFixture::write(path,saved);
        f.run([](const Json& status) {return status.value("state",std::string{})=="resumed";});
        const auto trace=visibleContinuityTraceEvent(f,"following_forge_tool");
        require(trace.at("provider_used")==130969U && trace.at("context_budget_cleared")==(cached==1000U),
            "successor budget clearance ignored a larger cached full prompt projection");
        require(f.sends==2U && f.creations==1U,"successor budget observation replayed a visible handoff effect");
    }
}

void completedSuccessorRechecksChangedCacheWithoutNewGeneration()
{
    VisibleHandoffFixture f;f.waiting();f.saveModelPacket();
    f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
    const auto measurement=renderedPromptConversation(1000U);
    f.append(message(Json::array({version(Json::array({
        VisibleHandoffFixture::tool("context_get",Json{{"ok",true},{"found",true},{"handoff_id","durable-native-packet"}},2U),
        VisibleHandoffFixture::tool("agent_list",Json{{"ok",true}},3U),
        measurement["messages"][0]["versions"][0]["steps"][0]}))})));
    const auto path=f.fixture.fileFixture.root()/"conversations"/std::filesystem::path{f.selected};
    std::ifstream input{path,std::ios::binary};auto saved=Json::parse(input);input.close();
    saved["lastUsedModel"]=measurement["lastUsedModel"];saved["tokenCount"]=1000U;
    ConversationFixture::write(path,saved);
    const auto completed=f.run([](const Json& status) {return status.value("state",std::string{})=="resumed";});
    TestContext operation;const auto before=take(WindowsLMStudioConversationReader::read(f.fixture.fileFixture.path(),operation.active()));
    require(before.has_value() && f.sends==2U && f.creations==1U,"private successor did not complete once before the cache update");
    saved["tokenCount"]=264415U;ConversationFixture::write(path,saved);
    const auto changed=take(WindowsLMStudioConversationReader::read(f.fixture.fileFixture.path(),operation.active()));
    require(changed && changed->generationEvidence==before->generationEvidence && changed->usedTokens==before->usedTokens,
        "the cache-only pressure fixture accidentally changed its selected provider generation");
    const auto waiting=f.run([](const Json& status) {return status.value("state",std::string{})=="waiting_for_model_packet";});
    require(f.sends==3U && f.creations==1U && waiting["context_telemetry"]["pressure_source"]=="cached_rendered_prompt" &&
        waiting["context_telemetry"]["generation_reference"]==completed["context_telemetry"]["generation_reference"],
        "a completed successor ignored cache-only pressure or repeated New chat instead of requesting a fresh model packet");
    const auto nextState=f.checkpoint().at("state");
    require(!nextState.at("delivery_acknowledged").get<bool>() && !nextState.at("context_recovered").get<bool>(),
        "a second rollover inherited delivery/recovery acknowledgements from the completed packet");
}

void cachedPromptTelemetryClearsWhenSelectionDisappears()
{
    VisibleHandoffFixture f;f.fixture.fileFixture.save(renderedPromptConversation(180000U));
    f.run([](const Json& status) {return status.contains("context_telemetry") && status["context_telemetry"].value("available",false);});
    noSelection(f.fixture,Json{{"selectedConversation",nullptr}});
    const auto status=f.run([](const Json& observed) {return observed.contains("context_telemetry") &&
        !observed["context_telemetry"].value("available",true) && observed["context_telemetry"]["conversation_id"].is_null();});
    const auto& telemetry=status.at("context_telemetry");
    for(const char* key:{"tokens_used","cached_rendered_prompt_tokens","pressure_tokens","pressure_source","pressure_headroom_tokens"})
        require(telemetry.at(key).is_null(),"no selected chat retained stale measured or projected pressure telemetry");
    require(f.sends==0U && f.creations==0U,"clearing selection dispatched a continuity effect");
}

void durableVisibleHandoffRecoveryCases()
{
    std::vector<std::string> failures;
    const auto run=[&](const char* name,const std::function<void()>& exercise) {
        try {exercise();std::cout<<"[CASE PASS] continuity-durable."<<name<<'\n';}
        catch(const std::exception& error) {failures.push_back(std::string{name}+": "+error.what());std::cerr<<"[CASE FAIL] continuity-durable."<<failures.back()<<'\n';}
    };
    run("uncertain_send_never_replayed",[] {
        VisibleHandoffFixture f;f.fault=VisibleHandoffFixture::Fault::AmbiguousRequest;
        f.fixture.fileFixture.save(conversation(Json::array({message(Json::array({version(Json::array({generation(31000U,32768U)}))}))})));
        f.pending();require(f.sends==1U,"uncertain request was not dispatched once");
        f.fault=VisibleHandoffFixture::Fault::None;f.reconstruct();f.pending();
        require(f.sends==1U,"reconstructed observer repeated an uncertain Send");
        f.append(message(Json::array({Json{{"type","singleStep"},{"role","user"},
            {"content",Json::array({Json{{"type","text"},{"text",f.lastText}}})}}})));
        f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="waiting_for_model_packet";});
        require(f.sends==1U && f.checkpoint()["state"]["effect"]["stage"]=="confirmed","exact native evidence did not reconcile the uncertain request without replay");
    });
    run("pre_dispatch_storage_failure",[] {
        VisibleHandoffFixture f;f.fault=VisibleHandoffFixture::Fault::BeforeDispatchStorage;
        f.fixture.fileFixture.save(conversation(Json::array({message(Json::array({version(Json::array({generation(31000U,32768U)}))}))})));
        f.pending();require(f.sends==0U && f.creations==0U,"storage failure did not stop the pending native effect");
        f.publicationBlock.reset();
        f.fault=VisibleHandoffFixture::Fault::None;f.reconstruct();f.run([&](const Json& status) {
            if(status.value("state",std::string{})!="waiting_for_model_packet" || f.sends.load()!=1U) return false;
            const auto state=f.checkpoint().at("state");
            return state.at("packet_request_acknowledged")==true && state.at("effect").is_object() &&
                state.at("effect").at("stage")=="confirmed";
        });
        require(f.sends==1U,"a definitely undispatched request did not recover after storage became writable");
        const auto recovered=f.checkpoint().at("state");
        require(recovered.at("packet_request_acknowledged")==true && recovered.at("effect").at("stage")=="confirmed",
            "the recovered packet request lacked a durable confirmed effect receipt");
    });
    run("post_dispatch_storage_failure",[] {
        VisibleHandoffFixture f;f.fault=VisibleHandoffFixture::Fault::AfterDispatchStorage;
        f.fixture.fileFixture.save(conversation(Json::array({message(Json::array({version(Json::array({generation(31000U,32768U)}))}))})));
        f.pending();require(f.sends==1U,"private post-dispatch failure did not dispatch one effect");
        f.publicationBlock.reset();require(f.checkpoint()["state"]["effect"]["stage"]=="uncertain","failed post-write replaced the last durable uncertain receipt");
        f.fault=VisibleHandoffFixture::Fault::None;f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="waiting_for_model_packet";});
        require(f.sends==1U,"post-write recovery repeated an acknowledged native message");
    });
    run("overlapping_writer",[] {
        Fakes::RecordingConfigurationStoreFake contenderConfiguration;
        Fakes::RecordingProjectMemoryService contenderProjects;
        VisibleHandoffFixture f;f.waiting();
        contenderConfiguration.reloadResult.set(f.fixture.configuration.reloadResult.get());
        contenderProjects.listRecentResult.set(f.fixture.projects.listRecentResult.get());
        f.fixture.observer->start();f.awaitWriterOwnership();
        auto first=std::move(f.fixture.observer);
        f.fixture.observer=f.create(contenderProjects,contenderConfiguration);f.pending();first->shutdown();
        require(f.sends==1U,"overlapping observers both dispatched into the same LM Studio home");
        f.run([](const Json& status) {return status.value("state",std::string{})=="waiting_for_model_packet" && !status.contains("error");});
        require(f.sends==1U,"the previously denied observer replayed a request after writer reacquisition");
    });
    run("saved_scope_never_grants_authority",[] {
        VisibleHandoffFixture f;f.waiting();f.reconstruct(false);f.fixture.observer->start();
        std::this_thread::sleep_for(std::chrono::milliseconds{600});f.fixture.observer->shutdown();
        require(Json::parse(f.fixture.observer->status())["state"]=="awaiting_bound_workspace" && f.sends==1U,
            "persisted checkpoint granted workspace/control authority");
        f.fixture.observer->bindWorkspace(f.fixture.project,f.fixture.fileFixture.path());
        f.run([](const Json& status) {return status.value("state",std::string{})=="waiting_for_model_packet";});
        require(f.sends==1U,"fresh authorized binding lost the retained waiting phase");
    });
    run("null_selection_retained",[] {
        VisibleHandoffFixture f;f.waiting();noSelection(f.fixture,Json{{"selectedConversation",nullptr}});f.reconstruct();
        const auto status=f.pending();require(!status["context_telemetry"]["available"].get<bool>() && f.sends==1U,
            "null selection discarded the handoff or repeated a request");
    });
    run("third_chat_rejected",[] {
        VisibleHandoffFixture f;f.waiting();f.select("project/third.conversation.json",conversation(Json::array()));f.reconstruct();f.pending();
        require(f.sends==1U && f.creations==0U,"an unrelated selected chat was adopted as the saved predecessor");
    });
    for(const auto kind:{"schema_drift","source_drift","malformed_state","scope_drift","tampered_blob","oversized_phase"}) run(kind,[kind] {
        VisibleHandoffFixture f;f.waiting();auto document=f.checkpoint();
        if(std::string_view{kind}=="schema_drift") document["schema_version"]=2U;
        else if(std::string_view{kind}=="source_drift") document["source_contract"]="different-source-contract";
        else if(std::string_view{kind}=="malformed_state") document["state"].erase("predecessor");
        else if(std::string_view{kind}=="scope_drift") document["scope"]["project_root"]="C:/untrusted-replacement";
        else if(std::string_view{kind}=="oversized_phase") document["state"]["phase"]=std::uint64_t{4294967296ULL};
        if(std::string_view{kind}=="tampered_blob") {
            std::fstream output{f.checkpointPath(),std::ios::binary|std::ios::in|std::ios::out};
            output.seekg(-1,std::ios::end);char last{};output.get(last);output.seekp(-1,std::ios::end);
            output.put(static_cast<char>(static_cast<unsigned char>(last)^1U));require(static_cast<bool>(output),"private checkpoint tamper fixture failed");
        } else f.writeCheckpoint(document);
        f.reconstruct();f.pending();require(f.sends==1U && f.creations==0U,"invalid checkpoint reset Observe and replayed control");
    });
    run("provider_drift",[] {
        VisibleHandoffFixture f;f.waiting();Domain::AppConfig configuration;configuration.localModel.port=1235U;
        f.fixture.configuration.reloadResult.set(Domain::Result<Domain::AppConfig>::success(configuration));f.reconstruct();f.pending();
        require(f.sends==1U,"provider drift replayed a saved packet request");
    });
    run("route_drift",[] {
        VisibleHandoffFixture f;f.waiting();
        std::ifstream input{f.fixture.fileFixture.root()/"mcp.json",std::ios::binary};auto route=Json::parse(input);input.close();
        route["mcpServers"]["forge-conductor"]["args"]=Json::array({"serve","--role","fallback"});
        ConversationFixture::write(f.fixture.fileFixture.root()/"mcp.json",route);f.reconstruct();f.pending();
        require(f.sends==1U,"changed primary route replayed a saved packet request");
    });
    run("checkpoint_byte_bounds",[] {
        const auto oversized=Infrastructure::Windows::Detail::LMStudioChatCheckpoint::seal(
            Json{{"oversized",std::string(Infrastructure::Windows::Detail::LMStudioChatCheckpoint::MaximumPlainBytes,'x')}});
        requireError(oversized,Domain::ErrorCodes::PayloadTooLarge,"oversized checkpoint plaintext was accepted");
        std::vector<std::byte> stored(Infrastructure::Windows::Detail::LMStudioChatCheckpoint::MaximumStoredBytes+1U);
        requireError(Infrastructure::Windows::Detail::LMStudioChatCheckpoint::open(stored),Domain::ErrorCodes::IntegrityFailure,
            "oversized stored checkpoint was decoded");
    });
    run("stable_successor_undispatched_delivery",[] {
        VisibleHandoffFixture f;f.waiting();f.saveModelPacket();f.fault=VisibleHandoffFixture::Fault::StableNewUndelivered;
        f.run([](const Json& status) {return status.contains("error") && status.value("state",std::string{})=="recovery_pending";});
        require(f.creations==1U && f.sends==1U,"private stable creation dispatched a delivery unexpectedly");
        f.fault=VisibleHandoffFixture::Fault::None;f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        require(f.creations==1U && f.sends==2U,"stable recorded successor was recreated or not delivered exactly once");
        f.nativeResume();f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="resumed";});
        f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="resumed" && status.value("available",false) && !status.contains("handoff_recovery");});
        require(f.creations==1U && f.sends==2U && f.checkpoint()["state"]["phase"]==4U,"completed native recovery was replayed or lost across reconstruction");
    });
    run("packet_revision_drift",[] {
        VisibleHandoffFixture f;f.waiting();f.saveModelPacket();f.fault=VisibleHandoffFixture::Fault::StableNewUndelivered;
        f.run([](const Json& status) {return status.contains("error") && status.value("state",std::string{})=="recovery_pending";});
        ++f.fixture.legacyContinuity.record->writeSequence;f.fault=VisibleHandoffFixture::Fault::None;f.reconstruct();f.pending();
        require(f.creations==1U && f.sends==1U,"a changed stored packet revision was silently delivered into the successor");
    });
    run("uncertain_new_chat_never_replayed",[] {
        VisibleHandoffFixture f;f.waiting();f.saveModelPacket();f.fault=VisibleHandoffFixture::Fault::UncertainNew;
        f.run([](const Json& status) {return status.contains("error") && status.value("state",std::string{})=="recovery_pending";});
        f.fault=VisibleHandoffFixture::Fault::None;f.reconstruct();f.pending();
        require(f.creations==1U && f.sends==1U,"an empty unconfirmed successor caused New chat or Send replay");
        f.nativeResume();f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="resumed";});
        require(f.creations==1U && f.sends==1U,"actual native packet recovery repeated an uncertain creation/delivery");
    });
    run("uncertain_delivery_never_replayed",[] {
        VisibleHandoffFixture f;f.waiting();f.saveModelPacket();f.fault=VisibleHandoffFixture::Fault::AmbiguousDelivery;
        f.run([](const Json& status) {return status.contains("error") && status.value("state",std::string{})=="recovery_pending";});
        f.fault=VisibleHandoffFixture::Fault::None;f.reconstruct();f.pending();
        require(f.creations==1U && f.sends==2U,"reconstruction repeated the uncertain successor Send");
    });
    require(failures.empty(),"Durable visible handoff cases failed; inspect the per-case evidence above.");
}

void explicitVisibleRouteRecoveryCases()
{
    using Checkpoint=Infrastructure::Windows::Detail::LMStudioChatCheckpoint;
    const auto bytes=[](const std::filesystem::path& path) {
        const auto extended=std::filesystem::path{L"\\\\?\\"+std::filesystem::absolute(path).wstring()};
        std::ifstream input{extended,std::ios::binary};return std::vector<char>{std::istreambuf_iterator<char>{input},{}};
    };
    const auto currentScope=[](VisibleHandoffFixture& f,const Json& old) {
        auto scope=old.at("scope");Json active=Json::object();
        std::ifstream input{f.fixture.fileFixture.root()/"mcp.json"};const auto routes=Json::parse(input).at("mcpServers");
        for(const char* key:{"forge-conductor","forge-conductor-fallback","forge-conductor-clu"}) active[key]=routes.at(key);
        Infrastructure::Windows::BCryptSha256Hasher hasher;const auto serialized=active.dump();
        scope["routing_sha256"]=take(hasher.sha256(std::as_bytes(std::span{serialized.data(),serialized.size()}))).value();return scope;
    };
    const auto refused=[&](VisibleHandoffFixture& f) {
        const auto before=bytes(f.checkpointPath());std::optional<std::int64_t> firstObservation;
        f.run([&](const Json& status) {
            if(status.value("state",std::string{})!="recovery_pending" || !status.contains("context_telemetry")) return false;
            const auto observed=status.at("context_telemetry").at("observed_at_unix_ms").get<std::int64_t>();
            if(!firstObservation) {firstObservation=observed;return false;}
            // A later sequential worker tick proves the first admission returned
            // before shutdown can cancel that attempt's operation context.
            return observed>*firstObservation;
        });
        require(f.creations==0U && f.sends==0U,"explicit recovery replayed or dispatched a refused handoff");
        require(bytes(f.checkpointPath())==before,"refused explicit recovery changed the encrypted checkpoint");
    };
    std::vector<std::string> failures;
    const auto run=[&](const char* name,const std::function<void()>& exercise) {
        try {exercise();std::cout<<"[CASE PASS] continuity-route-recovery."<<name<<'\n';}
        catch(const std::exception& error) {failures.push_back(std::string{name}+": "+error.what());std::cerr<<"[CASE FAIL] continuity-route-recovery."<<failures.back()<<'\n';}
    };
    run("explicit_native_packet_archive_and_resume",[&] {
        VisibleHandoffFixture f;f.undispatched();const auto old=f.checkpoint();const auto raw=bytes(f.checkpointPath());
        f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);
        const auto recovered=f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        require(f.creations==1U && f.sends==1U,"explicit handoff did not create and deliver exactly one successor");
        require(f.lastText.starts_with("Resume this Forge project"),"explicit recovery resent the failed old packet request");
        const auto& recovery=recovered.at("route_recovery");
        const auto archive=std::filesystem::path{recovery.at("archive_path").get<std::string>()};
        require(bytes(archive)==raw,"explicit recovery archive did not retain the original encrypted bytes");
        Infrastructure::Windows::BCryptSha256Hasher hasher;
        require(recovery.at("archive_sha256")==take(hasher.sha256(std::as_bytes(std::span{raw}))).value(),"explicit recovery archive hash differs from its original encrypted bytes");
        require(recovery.at("previous_scope")==old.at("scope") && recovery.at("previous_revision")==old.at("revision"),"explicit recovery lost original scope or revision");
        const auto updated=f.checkpoint();require(updated.at("scope")==currentScope(f,old) && updated.at("revision")>old.at("revision"),"explicit recovery did not publish the validated new scope");
        require(!updated["state"]["packet_request_acknowledged"].get<bool>(),"explicit recovery fabricated acknowledgement of the old Send");
        f.nativeResume();f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="resumed";});
        require(f.creations==1U && f.sends==1U && bytes(archive)==raw,"completed explicit recovery replayed effects or altered its archive");
    });
    run("completed_cycle_failed_new_request_has_no_delivery_acknowledgements",[&] {
        VisibleHandoffFixture f;f.waiting();const auto predecessor=f.selected;f.saveModelPacket();
        f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        f.nativeResume();f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="resumed";});
        const auto completed=f.checkpoint().at("state");
        require(completed.at("delivery_acknowledged")==true && completed.at("context_recovered")==true,
            "private first cycle did not complete with both real acknowledgements");
        f.selected=predecessor;ConversationFixture::write(f.fixture.fileFixture.root()/".internal"/"conversation-config.json",Json{{"selectedConversation",predecessor}});
        f.fault=VisibleHandoffFixture::Fault::UndispatchedRequest;f.reconstruct();
        f.run([&](const Json& status) {
            const auto error=status.value("error",std::string{});
            if(error.find("32 MiB")==std::string::npos) return false;
            const auto state=f.checkpoint().at("state");return state.at("phase")==1U &&
                state.at("dispatch_error")==error && state.at("operational_error")==error;
        });
        const auto state=f.checkpoint().at("state");
        require(!state.at("delivery_acknowledged").get<bool>() && !state.at("context_recovered").get<bool>() &&
            !state.at("packet_request_acknowledged").get<bool>() && state.at("effect").is_null() &&
            state.at("packet_id")=="" && state.at("packet_write_sequence")==0U &&
            state.at("handed").is_null() && state.at("handed_message")=="" &&
            state.at("previous_packet")=="durable-native-packet" && state.at("previous_sequence")==1U,
            "a failed second cycle retained a delivery acknowledgement or lost the prior packet boundary");
        require(f.creations==1U && f.sends==2U,"a failed second packet request repeated New chat or Send");
    });
    const auto legacyUndispatched=[&](VisibleHandoffFixture& f) {
        f.fixture.fileFixture.save(conversation(Json::array()));f.saveModelPacket("prior-completed-packet",1U);
        f.undispatched();auto saved=f.checkpoint();
        require(saved.at("state").at("previous_packet")=="prior-completed-packet" && saved.at("state").at("previous_sequence")==1U,
            "private legacy new cycle did not retain its prior model packet boundary");
        saved["state"]["delivery_acknowledged"]=true;saved["state"]["context_recovered"]=true;
        f.writeCheckpoint(saved);
    };
    run("legacy_empty_failed_cycle_requires_fresh_native_packet_and_preserves_archive",[&] {
        VisibleHandoffFixture f;legacyUndispatched(f);const auto old=f.checkpoint();const auto raw=bytes(f.checkpointPath());
        f.upgradeRoutes();f.saveModelPacket("fresh-second-cycle-packet",2U);const auto native=f.nativeHandoff();
        const auto send=f.controls->send;std::atomic<bool> newCycleVerified{};
        f.controls->send=[&](const Domain::PathText& executable,std::string_view text,bool newChat,
            const Domain::OperationContext& operation,std::optional<std::string_view> expected,
            const std::function<void(std::string_view)>& successor,
            const Infrastructure::Windows::LMStudioChatEffectObserver& receipt) {
            require(newChat,"legacy route recovery retried the failed old packet request");
            const auto state=f.checkpoint().at("state");
            require(state.at("phase")==2U && state.at("packet_id")=="fresh-second-cycle-packet" &&
                state.at("packet_write_sequence")==2U && !state.at("delivery_acknowledged").get<bool>() &&
                !state.at("context_recovered").get<bool>() && !state.at("packet_request_acknowledged").get<bool>() &&
                state.at("effect").is_null(),"explicit legacy recovery did not clear only the prior cycle flags before new dispatch");
            newCycleVerified=true;return send(executable,text,newChat,operation,expected,successor,receipt);
        };
        f.reconstruct();f.queueHandoff(native);
        const auto status=f.run([](const Json& value) {return value.value("state",std::string{})=="resuming";});
        require(newCycleVerified && f.creations==1U && f.sends==1U,"fresh native legacy recovery did not deliver exactly once");
        const auto& recovery=status.at("route_recovery");
        require(bytes(std::filesystem::path{recovery.at("archive_path").get<std::string>()})==raw &&
            recovery.at("previous_scope")==old.at("scope") && recovery.at("previous_revision")==old.at("revision"),
            "explicit recovery rewrote its legacy encrypted state or lost its source scope/revision");
        require(!f.checkpoint()["state"]["packet_request_acknowledged"].get<bool>(),
            "legacy recovery fabricated acknowledgement of the failed request");
    });
    for(const char* change:{"mixed_delivery","mixed_context","request_ack","repair_ack","repair_request","repair_attempt",
        "successor","created_successor","handed_body","handed_message","packet_id","write_sequence",
        "uncertain_effect","confirmed_effect","different_failure","missing_failure","rejected_packet","rejected_sequence","invalid_requests"}) {
        run((std::string{"legacy_empty_cycle_refuses_"}+change).c_str(),[&,change] {
            VisibleHandoffFixture f;legacyUndispatched(f);auto saved=f.checkpoint();auto& state=saved["state"];
            const std::string_view field{change};
            if(field=="mixed_delivery") state["delivery_acknowledged"]=false;
            else if(field=="mixed_context") state["context_recovered"]=false;
            else if(field=="request_ack") state["packet_request_acknowledged"]=true;
            else if(field=="repair_ack") state["repair_acknowledged"]=true;
            else if(field=="repair_request") state["repair_request"]="retained repair request";
            else if(field=="repair_attempt") state["repair_attempts"]=1U;
            else if(field=="successor") state["successor"]="project/other.conversation.json";
            else if(field=="created_successor") state["created_successor"]="project/other.conversation.json";
            else if(field=="handed_body") state["handed"]=Json::object();
            else if(field=="handed_message") state["handed_message"]="retained delivered packet text";
            else if(field=="packet_id") state["packet_id"]="prior-completed-packet";
            else if(field=="write_sequence") state["packet_write_sequence"]=1U;
            else if(field=="different_failure") state["operational_error"]="different real operational error";
            else if(field=="missing_failure") state["dispatch_error"]="";
            else if(field=="rejected_packet") state["last_rejected_packet"]="rejected-old-packet";
            else if(field=="rejected_sequence") state["last_rejected_sequence"]=1U;
            else if(field=="invalid_requests") state["processed_invalid_requests"]=Json::array({"old-invalid-request"});
            else state["effect"]={{"kind","send"},{"stage",field=="confirmed_effect"?"confirmed":"uncertain"},{"purpose","request"},
                {"conversation_id",f.selected},{"previous_user_messages",0U}};
            f.writeCheckpoint(saved);f.upgradeRoutes();f.saveModelPacket("fresh-second-cycle-packet",2U);
            const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);refused(f);
        });
    }
    run("legacy_empty_cycle_without_fresh_callback_does_not_migrate",[&] {
        VisibleHandoffFixture f;legacyUndispatched(f);f.upgradeRoutes();f.saveModelPacket("fresh-second-cycle-packet",2U);
        f.reconstruct();refused(f);
    });
    run("legacy_empty_cycle_stale_callback_scope_does_not_migrate",[&] {
        VisibleHandoffFixture f;legacyUndispatched(f);f.saveModelPacket("fresh-second-cycle-packet",2U);const auto native=f.nativeHandoff();
        f.reconstruct();f.queueHandoff(native);f.upgradeRoutes();refused(f);
    });
    run("legacy_empty_cycle_missing_native_result_does_not_migrate",[&] {
        VisibleHandoffFixture f;legacyUndispatched(f);f.upgradeRoutes();f.saveModelPacket("fresh-second-cycle-packet",2U);const auto native=f.nativeHandoff();
        f.fixture.fileFixture.save(conversation(Json::array()));f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("legacy_empty_cycle_unchanged_write_sequence_does_not_migrate",[&] {
        VisibleHandoffFixture f;legacyUndispatched(f);f.upgradeRoutes();f.saveModelPacket("fresh-second-cycle-packet",1U);const auto native=f.nativeHandoff();
        f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("no_automatic_route_migration",[&] {
        VisibleHandoffFixture f;f.undispatched();f.upgradeRoutes();f.saveModelPacket();f.reconstruct();refused(f);
    });
    run("intent_from_previous_routes_is_refused",[&] {
        VisibleHandoffFixture f;f.undispatched();f.saveModelPacket();const auto native=f.nativeHandoff();
        f.reconstruct();f.queueHandoff(native);f.upgradeRoutes();refused(f);
    });
    run("fresh_callback_rebinds_prior_intent",[&] {
        VisibleHandoffFixture f;f.undispatched();f.saveModelPacket();const auto native=f.nativeHandoff();
        f.reconstruct();f.queueHandoff(native);f.upgradeRoutes();refused(f);
        f.queueHandoff(native);f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        require(f.creations==1U && f.sends==1U,"a fresh current-scope callback did not replace the stale intent exactly once");
        require(!f.checkpoint()["state"]["packet_request_acknowledged"].get<bool>(),"fresh callback rebind acknowledged the failed old request");
    });
    run("global_routes_with_confirmed_workspace",[&] {
        VisibleHandoffFixture f;f.undispatched();f.upgradeRoutes(true);f.saveModelPacket();const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);
        f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        require(f.creations==1U && f.sends==1U,"valid global routes lost the separately confirmed live workspace");
    });
    run("global_routes_with_confirmed_workspace_and_cwd",[&] {
        VisibleHandoffFixture f;f.undispatched();auto routes=f.upgradeRoutes(true);
        for(const char* key:{"forge-conductor","forge-conductor-fallback","forge-conductor-clu"}) routes["mcpServers"][key]["cwd"]=f.fixture.fileFixture.path().value();
        ConversationFixture::write(f.fixture.fileFixture.root()/"mcp.json",routes);f.saveModelPacket();const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);
        f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        require(f.creations==1U && f.sends==1U,"authorized global routes with a matching cwd were not retained");
    });
    run("receipt_without_native_result",[&] {
        VisibleHandoffFixture f;f.undispatched();f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();
        f.fixture.fileFixture.save(conversation(Json::array()));f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("native_without_matching_receipt",[&] {
        VisibleHandoffFixture f;f.undispatched();f.upgradeRoutes();f.saveModelPacket();auto native=f.nativeHandoff();native["resume_seed"]="different receipt";
        f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("fallback_native_result_is_not_primary",[&] {
        VisibleHandoffFixture f;f.undispatched();f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();
        const auto path=f.fixture.fileFixture.root()/"conversations"/std::filesystem::path{f.selected};
        std::ifstream input{path};auto text=Json::parse(input).dump();input.close();
        auto at=text.find("mcp/forge-conductor\"");require(at!=std::string::npos,"private PRIMARY native request is missing");
        text.replace(at,std::string{"mcp/forge-conductor"}.size(),"mcp/forge-conductor-fallback");ConversationFixture::write(path,Json::parse(text));
        f.reconstruct();f.queueHandoff(native);refused(f);
    });
    for(const auto acknowledged:{false,true}) run(acknowledged?"confirmed_effect_not_migrated":"uncertain_effect_not_migrated",[&,acknowledged] {
        VisibleHandoffFixture f;f.undispatched();auto old=f.checkpoint();
        old["state"]["effect"]={{"kind","send"},{"stage",acknowledged?"confirmed":"uncertain"},{"purpose","request"},
            {"conversation_id",f.selected},{"previous_user_messages",0U}};
        old["state"]["packet_request_acknowledged"]=acknowledged;f.writeCheckpoint(old);
        f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);refused(f);
    });
    for(const char* field:{"provider","project_id","project_root","home","lmstudio_root","executable"}) run(field,[&,field] {
        VisibleHandoffFixture f;f.undispatched();auto old=f.checkpoint();
        if(std::string_view{field}=="provider") old["scope"][field]["model"]="foreign-provider";else old["scope"][field]="foreign-scope";
        f.writeCheckpoint(old);f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);refused(f);
    });
    for(const char* field:{"schema_version","source_contract","phase"}) run(field,[&,field] {
        VisibleHandoffFixture f;f.undispatched();auto old=f.checkpoint();
        if(std::string_view{field}=="schema_version") old[field]=2U;
        else if(std::string_view{field}=="source_contract") old[field]="foreign-source-contract";
        else old["state"][field]=std::uint64_t{4294967296ULL};
        f.writeCheckpoint(old);f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("third_chat_not_adopted",[&] {
        VisibleHandoffFixture f;f.undispatched();f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();
        f.select("project/third.conversation.json",conversation(Json::array({message(Json::array({version(Json::array({VisibleHandoffFixture::tool("session_handoff",native,4U)}))}))})));
        f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("null_selection_retains_checkpoint",[&] {
        VisibleHandoffFixture f;f.undispatched();f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();
        noSelection(f.fixture,Json{{"selectedConversation",nullptr}});f.reconstruct();f.queueHandoff(native);refused(f);
    });
    for(const char* field:{"command","deployment","cwd","role"}) run(field,[&,field] {
        VisibleHandoffFixture f;f.undispatched();auto routes=f.upgradeRoutes();auto& role=routes["mcpServers"]["forge-conductor-clu"];
        if(std::string_view{field}=="command") role["command"]="C:\\wrong.exe";
        else if(std::string_view{field}=="deployment") role["env"]["FORGE_DEPLOYMENT_ID"]="d05633b0-2289-4eee-afda-bcd2e6d8278c";
        else if(std::string_view{field}=="cwd") role["cwd"]="C:\\foreign-project";
        else role["env"]["FORGE_MCP_ROLE"]="primary";
        ConversationFixture::write(f.fixture.fileFixture.root()/"mcp.json",routes);f.saveModelPacket();const auto native=f.nativeHandoff();
        f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("standard_packet_semantics",[&] {
        VisibleHandoffFixture f;f.undispatched();f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();
        f.fixture.legacyContinuity.record->packet.narrative="placeholder";f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("stale_packet_sequence",[&] {
        VisibleHandoffFixture f;f.undispatched();auto old=f.checkpoint();old["state"]["previous_sequence"]=1U;f.writeCheckpoint(old);
        f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("fresh_configuration_drift_before_publication",[&] {
        VisibleHandoffFixture f;f.undispatched();f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();
        f.controls->idle=[&](const auto&,const auto&) {auto changed=Domain::AppConfig{};changed.localModel.model="foreign-model";
            f.fixture.configuration.reloadResult.set(Domain::Result<Domain::AppConfig>::success(changed));return Domain::Result<bool>::success(true);};
        f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("exclusive_writer_contention_and_retry",[&] {
        VisibleHandoffFixture f;f.undispatched();const auto old=f.checkpoint();f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();TestContext op;
        {Checkpoint writer{f.home(),f.fixture.project,currentScope(f,old),op.active()};f.reconstruct();f.queueHandoff(native);refused(f);}
        f.reconstruct();f.queueHandoff(native);f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        require(f.creations==1U && f.sends==1U,"explicit recovery did not acquire the released writer exactly once");
    });
    run("preexisting_archive_collision",[&] {
        VisibleHandoffFixture f;f.undispatched();const auto old=f.checkpoint();f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();
        TestContext op;std::filesystem::path archive;
        {Checkpoint store{f.home(),f.fixture.project,currentScope(f,old),op.active()};const auto snapshot=take(store.inspectRouteRecovery(op.active()));
            archive=f.checkpointPath().parent_path()/("lmstudio-visible-"+f.fixture.project.value()+".route-upgrade-"+old.at("revision").dump()+"-"+snapshot.sha256+".archive");}
        {std::ofstream output{std::filesystem::path{L"\\\\?\\"+std::filesystem::absolute(archive).wstring()},std::ios::binary};output<<"foreign archive";require(static_cast<bool>(output),"private archive collision fixture failed");}
        const auto collision=bytes(archive);f.reconstruct();f.queueHandoff(native);refused(f);require(bytes(archive)==collision,"archive collision was overwritten");
    });
    run("archive_creation_failure_preserves_original",[&] {
        VisibleHandoffFixture f;f.undispatched();const auto old=f.checkpoint();const auto raw=bytes(f.checkpointPath());f.upgradeRoutes();TestContext op;
        Checkpoint store{f.home(),f.fixture.project,currentScope(f,old),op.active()};const auto snapshot=take(store.inspectRouteRecovery(op.active()));
        const auto archive=f.checkpointPath().parent_path()/("lmstudio-visible-"+f.fixture.project.value()+".route-upgrade-"+old.at("revision").dump()+"-"+snapshot.sha256+".archive");
        const auto extended=std::filesystem::path{L"\\\\?\\"+std::filesystem::absolute(archive).wstring()};
        require(std::filesystem::create_directory(extended),"private archive-directory blocker could not be created");
        const auto blocked=store.recoverRoute(snapshot,old.at("state"),[] {return Domain::Result<void>::success();},op.active());
        require(!blocked && bytes(f.checkpointPath())==raw && std::filesystem::is_directory(extended),"failed archive creation replaced the original checkpoint or blocker");
    });
    run("changed_checkpoint_and_snapshot_are_not_overwritten",[&] {
        VisibleHandoffFixture f;f.undispatched();const auto old=f.checkpoint();const auto raw=bytes(f.checkpointPath());f.upgradeRoutes();TestContext op;
        Checkpoint store{f.home(),f.fixture.project,currentScope(f,old),op.active()};const auto snapshot=take(store.inspectRouteRecovery(op.active()));
        auto altered=snapshot;altered.document["revision"]=old.at("revision").get<std::uint64_t>()+1U;
        requireError(store.recoverRoute(altered,old.at("state"),[] {return Domain::Result<void>::success();},op.active()),
            Domain::ErrorCodes::IntegrityFailure,"modified recovery snapshot was accepted");
        require(bytes(f.checkpointPath())==raw,"modified recovery snapshot changed the original file");
        auto changed=old;changed["revision"]=old.at("revision").get<std::uint64_t>()+1U;
        const auto rejected=store.recoverRoute(snapshot,old.at("state"),[&] {f.writeCheckpoint(changed);return Domain::Result<void>::success();},op.active());
        requireError(rejected,Domain::ErrorCodes::IntegrityFailure,"changed checkpoint was overwritten after archive");
        require(f.checkpoint()==changed,"concurrent checkpoint revision was silently replaced");
    });
    run("publication_failure_and_retained_archive_retry",[&] {
        VisibleHandoffFixture f;f.undispatched();const auto old=f.checkpoint();const auto raw=bytes(f.checkpointPath());f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();
        f.denyCheckpointPublication();f.reconstruct();f.queueHandoff(native);refused(f);f.publicationBlock.reset();
        f.reconstruct();f.queueHandoff(native);f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        require(f.creations==1U && f.sends==1U && f.checkpoint()["scope"]==currentScope(f,old),"explicit recovery could not safely retry after write failure");
        std::size_t archives{};for(const auto& entry:std::filesystem::directory_iterator{f.checkpointPath().parent_path()}) if(entry.path().extension()==".archive") {
            ++archives;require(bytes(entry.path())==raw,"failed publication lost original archived bytes");}
        require(archives==1U,"explicit recovery rewrote or duplicated the retained archive on retry");
    });
    run("cancel_deadline_and_fresh_authority_failure",[&] {
        VisibleHandoffFixture f;f.undispatched();const auto old=f.checkpoint();const auto raw=bytes(f.checkpointPath());f.upgradeRoutes();TestContext op;
        Checkpoint store{f.home(),f.fixture.project,currentScope(f,old),op.active()};const auto snapshot=take(store.inspectRouteRecovery(op.active()));
        const auto accepted=[] {return Domain::Result<void>::success();};op.cancellation.request_stop();
        requireError(store.recoverRoute(snapshot,old.at("state"),accepted,op.active()),Domain::ErrorCodes::Cancelled,"cancelled explicit recovery published state");
        TestContext expired;expired.now=std::chrono::steady_clock::now()-std::chrono::seconds{1};
        requireError(store.recoverRoute(snapshot,old.at("state"),accepted,expired.expired()),Domain::ErrorCodes::DeadlineExceeded,"expired explicit recovery published state");
        TestContext fresh;auto denied=store.recoverRoute(snapshot,old.at("state"),[] {return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Unauthorized,"Private current authority drift"));},fresh.active());
        requireError(denied,Domain::ErrorCodes::Unauthorized,"fresh recovery authority drift was ignored");require(bytes(f.checkpointPath())==raw,"failed current authority changed original checkpoint");
    });
    run("planned_creating_reconstruction_without_old_request",[&] {
        VisibleHandoffFixture f;f.undispatched();const auto old=f.checkpoint();f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();
        TestContext op;auto state=old.at("state");state["phase"]=2U;state["packet_id"]="durable-native-packet";state["handed"]=native.at("packet");
        state["handed_message"]="Resume this Forge project from the exact retained packet.";state["packet_write_sequence"]=1U;
        {Checkpoint store{f.home(),f.fixture.project,currentScope(f,old),op.active()};const auto snapshot=take(store.inspectRouteRecovery(op.active()));
            const auto archive=take(store.recoverRoute(snapshot,state,[] {return Domain::Result<void>::success();},op.active()));require(!archive.value().empty(),"private planned packet archive is missing");}
        f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        require(f.creations==1U && f.sends==1U && f.lastText==state.at("handed_message").get<std::string>(),"planned Creating reconstruction retried the old unacknowledged packet request");
    });
    for(const bool sourceDrift:{false,true}) run(sourceDrift?"planned_creating_source_metadata_drift":"planned_creating_resume_ready_metadata_drift",[&,sourceDrift] {
        VisibleHandoffFixture f;f.undispatched();const auto old=f.checkpoint();f.upgradeRoutes();f.saveModelPacket();const auto native=f.nativeHandoff();
        TestContext op;auto state=old.at("state");state["phase"]=2U;state["packet_id"]="durable-native-packet";state["handed"]=native.at("packet");
        state["handed_message"]="Resume this Forge project from the exact retained packet.";state["packet_write_sequence"]=1U;
        {Checkpoint store{f.home(),f.fixture.project,currentScope(f,old),op.active()};const auto snapshot=take(store.inspectRouteRecovery(op.active()));
            const auto archive=take(store.recoverRoute(snapshot,state,[] {return Domain::Result<void>::success();},op.active()));require(!archive.value().empty(),"private planned packet archive is missing");}
        if(sourceDrift) f.fixture.legacyContinuity.record->packet.source=Domain::LegacyHandoffSource::Automatic;
        else f.fixture.legacyContinuity.record->packet.resumeReady=false;
        require(f.fixture.legacyContinuity.record->writeSequence==1U && f.checkpoint()["state"]["handed"]==native.at("packet"),
            "private metadata-only drift changed its packet body or write sequence");
        f.reconstruct();refused(f);
        require(Json::parse(f.fixture.observer->status()).at("handoff_recovery").at("reason").get<std::string>().find("complete model handoff contract")!=std::string::npos,
            "planned packet metadata drift did not reach the complete-model admission guard");
    });
    const auto planned=[&](VisibleHandoffFixture& f) {
        f.undispatched();f.saveModelPacket();auto saved=f.checkpoint();const auto native=f.nativeHandoff();
        saved["state"]["phase"]=2U;saved["state"]["packet_id"]="durable-native-packet";
        saved["state"]["handed"]=native.at("packet");saved["state"]["handed_message"]="Resume this Forge project from the retained planned packet.";
        saved["state"]["packet_write_sequence"]=1U;saved["state"]["operational_error"]="";saved["state"]["dispatch_error"]="";
        f.writeCheckpoint(saved);
    };
    run("planned_creating_route_upgrade_requires_new_complete_packet",[&] {
        VisibleHandoffFixture f;planned(f);const auto raw=bytes(f.checkpointPath());
        f.upgradeRoutes();f.saveModelPacket("fresh-planned-packet",2U);const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);
        const auto status=f.run([](const Json& value) {return value.value("state",std::string{})=="resuming";});
        require(f.creations==1U && f.sends==1U && status.at("packet_id")=="fresh-planned-packet","planned route recovery did not use exactly the newly saved packet");
        require(bytes(std::filesystem::path{status.at("route_recovery").at("archive_path").get<std::string>()})==raw,"planned route recovery changed the original encrypted archive");
        require(!f.checkpoint()["state"]["packet_request_acknowledged"].get<bool>(),"planned recovery fabricated an old request acknowledgement");
    });
    run("planned_creating_paired_old_acknowledgements_are_not_normalized",[&] {
        VisibleHandoffFixture f;planned(f);auto saved=f.checkpoint();
        saved["state"]["delivery_acknowledged"]=true;saved["state"]["context_recovered"]=true;f.writeCheckpoint(saved);
        f.upgradeRoutes();f.saveModelPacket("fresh-planned-packet",2U);const auto native=f.nativeHandoff();
        f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("planned_creating_old_callback_and_sequence_refused",[&] {
        VisibleHandoffFixture f;planned(f);f.upgradeRoutes();const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);refused(f);
    });
    run("planned_creating_prior_metadata_must_remain_complete",[&] {
        VisibleHandoffFixture f;planned(f);f.upgradeRoutes();f.saveModelPacket("fresh-planned-packet",2U);
        f.fixture.legacyContinuity.previousRecords.front().packet.resumeReady=false;
        const auto native=f.nativeHandoff();f.reconstruct();f.queueHandoff(native);refused(f);
    });
    for(const auto phase:{2U,3U,4U}) run(phase==2U?"uncertain_new_native_resume_route_upgrade":phase==3U?"resuming_native_route_upgrade":"completed_native_route_upgrade",[&,phase] {
        VisibleHandoffFixture f;planned(f);auto saved=f.checkpoint();
        saved["state"]["phase"]=phase;saved["state"]["effect"]={{"kind","new_chat"},{"stage",phase==2U?"uncertain":"confirmed"},
            {"purpose","delivery"},{"conversation_id",f.selected},{"previous_user_messages",0U}};
        if(phase!=2U) {saved["state"]["created_successor"]="project/native-resumed.conversation.json";saved["state"]["successor"]="project/native-resumed.conversation.json";
            saved["state"]["delivery_acknowledged"]=true;saved["state"]["context_recovered"]=phase==4U;}
        f.writeCheckpoint(saved);const auto raw=bytes(f.checkpointPath());f.upgradeRoutes();
        f.select("project/native-resumed.conversation.json",conversation(Json::array()));const auto receipt=f.fullResumeReceipt();
        f.reconstruct();f.fixture.observer->recordTool("context_get",true,receipt.dump());f.nativeFullResume();
        const auto status=f.run([](const Json& value) {return value.value("state",std::string{})=="resumed";});
        require(f.creations==0U && f.sends==0U,"native successor route recovery replayed a UI effect");
        require(status.at("route_recovery").at("recovery_kind")=="native_successor_resume" &&
            bytes(std::filesystem::path{status.at("route_recovery").at("archive_path").get<std::string>()})==raw,"native successor recovery lost the original scope archive");
        const auto state=f.checkpoint().at("state");require(state.at("phase")==4U && state.at("context_recovered")==true &&
            state.at("delivery_acknowledged")==true && state.at("effect").at("stage")=="confirmed" &&
            state.at("packet_request_acknowledged")==false,"native resume recovery did not preserve exact confirmed versus old request acknowledgement state");
    });
    const auto completeSuccessor=[&](VisibleHandoffFixture& f) {
        planned(f);auto saved=f.checkpoint();saved["state"]["phase"]=4U;
        saved["state"]["created_successor"]="project/native-resumed.conversation.json";
        saved["state"]["successor"]="project/native-resumed.conversation.json";
        saved["state"]["context_recovered"]=true;saved["state"]["delivery_acknowledged"]=true;
        saved["state"]["effect"]={{"kind","new_chat"},{"stage","confirmed"},
            {"purpose","delivery"},{"conversation_id",f.selected},{"previous_user_messages",0U}};
        f.writeCheckpoint(saved);return saved;
    };
    const auto appendResumeReceipt=[](VisibleHandoffFixture& f,const Json& receipt) {
        f.append(message(Json::array({version(Json::array({
            VisibleHandoffFixture::tool("context_get",receipt,20U),
            VisibleHandoffFixture::tool("get_forge_status",Json{{"ok",true}},21U),
            generation(3000U,32768U)}))})));
    };
    for(const bool cleared:{false,true}) run(cleared?"native_resume_guard_annotation_true":"native_resume_guard_annotation_false",[&,cleared] {
        VisibleHandoffFixture f;const auto saved=completeSuccessor(f);const auto raw=bytes(f.checkpointPath());
        f.upgradeRoutes();f.select("project/native-resumed.conversation.json",conversation(Json::array()));
        const auto callback=f.fullResumeReceipt();auto native=callback;native["context_budget_cleared"]=cleared;
        f.reconstruct();f.fixture.observer->recordTool("context_get",true,callback.dump());appendResumeReceipt(f,native);
        const auto status=f.run([](const Json& value) {return value.value("state",std::string{})=="resumed";});
        const auto& recovery=status.at("route_recovery");const auto updated=f.checkpoint();
        require(f.creations==0U && f.sends==0U && recovery.at("automatic_replay")==false,
            "native guard annotation recovery dispatched or replayed a UI effect");
        require(recovery.at("native_request_id")=="durable-native-20" && recovery.at("recovery_kind")=="native_successor_resume" &&
            bytes(std::filesystem::path{recovery.at("archive_path").get<std::string>()})==raw &&
            updated.at("scope")==currentScope(f,saved) && updated.at("revision")==saved.at("revision").get<std::uint64_t>()+1U,
            "native guard annotation recovery lost its exact request, old encrypted archive or new scope revision");
        require(updated.at("state").at("handed")==saved.at("state").at("handed") &&
            updated.at("state").at("packet_write_sequence")==saved.at("state").at("packet_write_sequence") &&
            updated.at("state").at("packet_request_acknowledged")==false &&
            updated.at("state").at("phase")==4U && updated.at("state").at("context_recovered")==true &&
            updated.at("state").at("delivery_acknowledged")==true && updated.at("state").at("effect").at("stage")=="confirmed",
            "native guard annotation recovery changed the retained packet or fabricated an old request acknowledgement");
    });
    for(const char* fault:{"native_resume_guard_annotation_number","native_resume_guard_annotation_string",
        "native_resume_guard_annotation_null","native_resume_unrelated_metadata","native_resume_missing_callback_field",
        "native_resume_changed_callback_field","native_resume_changed_packet_body","native_resume_changed_owned_annotation"}) run(fault,[&,fault] {
        VisibleHandoffFixture f;completeSuccessor(f);
        f.upgradeRoutes();f.select("project/native-resumed.conversation.json",conversation(Json::array()));
        auto callback=f.fullResumeReceipt();
        if(std::string_view{fault}=="native_resume_changed_owned_annotation") callback["context_budget_cleared"]=false;
        auto native=callback;native["context_budget_cleared"]=true;
        if(std::string_view{fault}=="native_resume_guard_annotation_number") native["context_budget_cleared"]=0U;
        if(std::string_view{fault}=="native_resume_guard_annotation_string") native["context_budget_cleared"]="false";
        if(std::string_view{fault}=="native_resume_guard_annotation_null") native["context_budget_cleared"]=nullptr;
        if(std::string_view{fault}=="native_resume_unrelated_metadata") native["unrelated_transport_annotation"]=true;
        if(std::string_view{fault}=="native_resume_missing_callback_field") native.erase("resume_seed");
        if(std::string_view{fault}=="native_resume_changed_callback_field") native["resume_seed"]="different native receipt";
        if(std::string_view{fault}=="native_resume_changed_packet_body") native["packet"]["resume"]["seed"]="different native packet";
        f.reconstruct();f.fixture.observer->recordTool("context_get",true,callback.dump());appendResumeReceipt(f,native);refused(f);
    });
    for(const char* fault:{"no_callback","empty_chat","no_following","wrong_receipt","wrong_successor","wrong_pointer","metadata_drift","fallback_context","stale_scope","callback_read_failure","callback_selection_changed"}) run(fault,[&,fault] {
        VisibleHandoffFixture f;planned(f);auto saved=f.checkpoint();saved["state"]["effect"]={{"kind","new_chat"},{"stage","uncertain"},
            {"purpose","delivery"},{"conversation_id",f.selected},{"previous_user_messages",0U}};
        if(std::string_view{fault}=="wrong_successor") saved["state"]["created_successor"]="project/other.conversation.json";
        f.writeCheckpoint(saved);
        if(std::string_view{fault}=="stale_scope") {
            f.select("project/native-resumed.conversation.json",conversation(Json::array()));const auto receipt=f.nativeFullResume();
            f.reconstruct();f.fixture.observer->recordTool("context_get",true,receipt.dump());f.upgradeRoutes();refused(f);return;
        }
        f.upgradeRoutes();f.select("project/native-resumed.conversation.json",conversation(Json::array()));
        auto receipt=f.fullResumeReceipt();
        if(std::string_view{fault}=="wrong_receipt") receipt["packet"]["resume"]["seed"]="different callback";
        if(std::string_view{fault}=="wrong_pointer") {TestContext operation;const auto pointer=take(f.fixture.memory.set({"continuity/project/"+f.fixture.project.value(),"foreign-packet",{}},operation.active()));require(pointer.stored,"private wrong pointer was not saved");}
        if(std::string_view{fault}=="metadata_drift") f.fixture.legacyContinuity.record->packet.source=Domain::LegacyHandoffSource::Automatic;
        f.reconstruct();
        if(std::string_view{fault}=="callback_read_failure") ConversationFixture::write(
            f.fixture.fileFixture.root()/".internal"/"conversation-config.json",Json{{"selectedConversation",nullptr}});
        if(std::string_view{fault}!="no_callback") f.fixture.observer->recordTool("context_get",true,receipt.dump());
        if(std::string_view{fault}=="callback_read_failure") f.select("project/native-resumed.conversation.json",conversation(Json::array()));
        if(std::string_view{fault}=="callback_selection_changed") f.select("project/changed-after-callback.conversation.json",conversation(Json::array()));
        if(std::string_view{fault}!="empty_chat") f.nativeFullResume(std::string_view{fault}!="no_following");
        if(std::string_view{fault}=="fallback_context") {
            const auto path=f.fixture.fileFixture.root()/"conversations"/std::filesystem::path{f.selected};std::ifstream input{path};auto text=Json::parse(input).dump();input.close();
            const auto at=text.find("mcp/forge-conductor\"");require(at!=std::string::npos,"private native context connector is missing");
            text.replace(at,std::string{"mcp/forge-conductor"}.size(),"mcp/forge-conductor-fallback");ConversationFixture::write(path,Json::parse(text));
        }
        refused(f);
    });
    for(const bool appendFresh:{false,true}) run(appendFresh?"fresh_native_resume_after_identical_history":"historical_identical_native_resume_cannot_satisfy_fresh_callback",[&,appendFresh] {
        VisibleHandoffFixture f;planned(f);auto saved=f.checkpoint();saved["state"]["phase"]=4U;
        saved["state"]["created_successor"]="project/native-resumed.conversation.json";
        saved["state"]["successor"]="project/native-resumed.conversation.json";
        saved["state"]["context_recovered"]=true;saved["state"]["delivery_acknowledged"]=true;
        f.writeCheckpoint(saved);const auto raw=bytes(f.checkpointPath());
        f.select("project/native-resumed.conversation.json",conversation(Json::array()));const auto receipt=f.nativeFullResume();
        f.upgradeRoutes();f.reconstruct();f.fixture.observer->recordTool("context_get",true,receipt.dump());
        if(!appendFresh) {refused(f);return;}
        f.nativeFullResume(false,30U);
        refused(f);
        require(bytes(f.checkpointPath())==raw && f.creations==0U && f.sends==0U,
            "Fresh context_get reused an old following tool to migrate the retained route.");
        f.append(message(Json::array({version(Json::array({VisibleHandoffFixture::tool("agent_list",Json{{"ok",true}},31U),generation(3000U,32768U)}))})));
        const auto status=f.run([](const Json& value) {return value.value("state",std::string{})=="resumed";});
        require(f.creations==0U && f.sends==0U && status.at("route_recovery").at("native_request_id")=="durable-native-30" &&
            bytes(std::filesystem::path{status.at("route_recovery").at("archive_path").get<std::string>()})==raw,
            "Fresh native resume with a new following tool did not preserve its actual request and archive.");
    });
    run("historical_request_ids_cannot_be_reused_after_content_reencoding",[&] {
        VisibleHandoffFixture f;planned(f);auto saved=f.checkpoint();saved["state"]["phase"]=4U;
        saved["state"]["created_successor"]="project/native-resumed.conversation.json";
        saved["state"]["successor"]="project/native-resumed.conversation.json";
        saved["state"]["context_recovered"]=true;saved["state"]["delivery_acknowledged"]=true;
        f.writeCheckpoint(saved);
        f.select("project/native-resumed.conversation.json",conversation(Json::array()));const auto receipt=f.nativeFullResume();
        f.upgradeRoutes();f.reconstruct();f.fixture.observer->recordTool("context_get",true,receipt.dump());
        const auto path=f.fixture.fileFixture.root()/"conversations"/std::filesystem::path{f.selected};
        std::ifstream input{path};auto document=Json::parse(input);input.close();
        std::size_t rewritten{};
        const std::function<void(Json&)> reencode=[&](Json& value) {
            if(value.is_object() && value.value("type",std::string{})=="toolCallResult") {
                const auto before=value.at("content").get<std::string>();
                const auto parsed=Json::parse(before);value["content"]=parsed.dump(2);
                require(value.at("content").get<std::string>()!=before && Json::parse(value.at("content").get<std::string>())==parsed,
                    "private historical native re-encoding changed its result meaning or retained identical bytes");
                ++rewritten;
            }
            if(value.is_object() || value.is_array()) for(auto& child:value) reencode(child);
        };
        reencode(document);require(rewritten==2U,"private historical context and following result were not both re-encoded");
        ConversationFixture::write(path,document);refused(f);
    });
    run("completed_successor_can_return_to_retained_pressure_chat",[&] {
        VisibleHandoffFixture f;f.waiting();const auto predecessor=f.selected;f.saveModelPacket();
        f.run([](const Json& status) {return status.value("state",std::string{})=="resuming";});
        f.nativeResume();f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="resumed";});
        require(f.creations==1U && f.sends==2U,"private first handoff did not complete exactly once");
        f.selected=predecessor;ConversationFixture::write(f.fixture.fileFixture.root()/".internal"/"conversation-config.json",Json{{"selectedConversation",predecessor}});
        f.reconstruct();f.run([](const Json& status) {return status.value("state",std::string{})=="waiting_for_model_packet";});
        const auto state=f.checkpoint().at("state");require(f.creations==1U && f.sends==3U && state.at("previous_sequence")==1U &&
            state.at("previous_packet")=="durable-native-packet" && state.at("packet_id")=="","returning to an aged chat replayed a successor or reused its prior packet");
        require(!state.at("delivery_acknowledged").get<bool>() && !state.at("context_recovered").get<bool>(),
            "returning to an aged predecessor retained the prior successor's delivery/recovery acknowledgements");
    });
    require(failures.empty(),"Explicit visible route recovery cases failed; inspect the per-case evidence above.");
}

void continuityObservationRecoversAfterPartialWrite()
{
    ContinuityObservationFixture fixture;
    fixture.partial();
    fixture.observer->start();
    fixture.await([](const Json& status) {
        return status.value("error", std::string{}).find("incomplete or invalid JSON") != std::string::npos;
    });
    fixture.observer->shutdown();
    fixture.complete();
    fixture.observer->start();
    const auto recovered = fixture.await([](const Json& status) {
        return status.contains("context_telemetry") && status["context_telemetry"].value("available", false) &&
            !status.contains("error");
    });
    fixture.observer->shutdown();
    require(!recovered.contains("error"), "successful observation retained a recovered partial-write error");
    require(recovered.at("state") == "observing" && !recovered.at("available").get<bool>(),
        "observation recovery changed the verified visible-handoff availability contract");
    require(recovered.at("context_telemetry").at("tokens_used") == 100U,
        "recovered observation did not retain actual provider usage");
}

void continuityObservationPipelineRecovery()
{
    ContinuityObservationFixture fixture;
    fixture.projects.listRecentResult.set(Domain::Result<Domain::MemoryPage>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, "Continuity preference read failed.")));
    fixture.complete();
    fixture.observer->start();
    const auto first = fixture.await([](const Json& status) {
        return status.value("error", std::string{}) == "Continuity preference read failed.";
    });
    const auto firstObservation = first.at("context_telemetry").at("observed_at_unix_ms").get<std::int64_t>();
    fixture.await([&](const Json& status) {
        return status.at("context_telemetry").at("observed_at_unix_ms").get<std::int64_t>() > firstObservation &&
            status.value("error", std::string{}) == "Continuity preference read failed.";
    });
    fixture.observer->shutdown();
    fixture.partial();
    fixture.observer->start();
    fixture.await([](const Json& status) {
        return status.value("error", std::string{}).find("incomplete or invalid JSON") != std::string::npos;
    });
    fixture.observer->shutdown();
    fixture.complete();
    fixture.observer->start();
    const auto ongoing = fixture.await([&](const Json& status) {
        return status.at("context_telemetry").at("observed_at_unix_ms").get<std::int64_t>() > firstObservation &&
            status.value("error", std::string{}) == "Continuity preference read failed.";
    });
    fixture.observer->shutdown();
    require(ongoing.contains("error"), "successful file read hid an ongoing preference failure");
    const auto lastObservation = ongoing.at("context_telemetry").at("observed_at_unix_ms").get<std::int64_t>();
    fixture.projects.listRecentResult.set(Domain::Result<Domain::MemoryPage>::success(
        Domain::MemoryPage{fixture.project, {}, std::nullopt, false, 0U, 0U}));
    fixture.observer->start();
    const auto recovered = fixture.await([&](const Json& status) {
        return status.at("context_telemetry").at("observed_at_unix_ms").get<std::int64_t>() > lastObservation &&
            !status.contains("error");
    });
    fixture.observer->shutdown();
    require(!recovered.contains("error"), "successful observation pipeline retained a recovered preference error");
}

void continuityConfigurationReadRecovery()
{
    ContinuityObservationFixture fixture;
    fixture.complete();
    fixture.configuration.reloadResult.set(Domain::Result<Domain::AppConfig>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, "Continuity configuration read failed.")));
    fixture.observer->start();
    fixture.await([](const Json& status) {
        return status.value("error", std::string{}) == "Continuity configuration read failed.";
    });
    fixture.observer->shutdown();
    fixture.configuration.reloadResult.set(Domain::Result<Domain::AppConfig>::success({}));
    fixture.observer->start();
    const auto recovered = fixture.await([](const Json& status) {
        return status.contains("context_telemetry") && status["context_telemetry"].value("available", false) &&
            !status.contains("error");
    });
    fixture.observer->shutdown();
    require(!recovered.contains("error"), "successful observation pipeline retained a recovered configuration error");
}

void continuityMcpRouteReadRecovery()
{
    ContinuityObservationFixture fixture;
    fixture.complete();
    const auto routes = fixture.fileFixture.root() / "mcp.json";
    std::string saved;
    {
        std::ifstream input{routes, std::ios::binary};
        require(input.good(), "private MCP route fixture could not be opened");
        saved.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    }
    {
        std::ofstream output{routes, std::ios::binary | std::ios::trunc};
        output << "{\"mcpServers\":";
        require(static_cast<bool>(output), "partial MCP route fixture write failed");
    }
    fixture.observer->start();
    fixture.await([](const Json& status) { return status.contains("error"); });
    fixture.observer->shutdown();
    {
        std::ofstream output{routes, std::ios::binary | std::ios::trunc};
        output << saved;
        require(static_cast<bool>(output), "private MCP route fixture could not be completed");
    }
    fixture.observer->start();
    const auto recovered = fixture.await([](const Json& status) {
        return status.contains("context_telemetry") && status["context_telemetry"].value("available", false) &&
            !status.contains("error");
    });
    fixture.observer->shutdown();
    require(!recovered.contains("error"), "successful observation pipeline retained a recovered MCP route error");
}

void observationRecoveryPreservesWorkspaceControlFailure()
{
    ContinuityObservationFixture fixture;
    fixture.complete();
    const auto traceDirectory = fixture.fileFixture.root() / "home" / "continuity";
    {
        std::ofstream blocker{traceDirectory, std::ios::binary};
        blocker << "Private fixture blocks workspace binding trace creation.";
        require(static_cast<bool>(blocker), "workspace control trace blocker could not be created");
    }
    const auto rebound = parse<Domain::ProjectId>("fd975f4a-6252-42a1-b28a-d422d8a34c24");
    fixture.observer->bindWorkspace(rebound, fixture.fileFixture.path());
    fixture.observer->start();
    const auto failed = fixture.await([&](const Json& status) {
        return status.contains("error") && status.value("project_id", std::string{}) == rebound.value();
    });
    fixture.observer->shutdown();
    const auto controlError = failed.at("error").get<std::string>();
    require(failed.at("binding_source") == "authorized_mcp_workspace",
        "test did not fail inside the authorized workspace binding control path");
    require(std::filesystem::remove(traceDirectory), "private trace blocker could not be removed");
    fixture.partial();
    fixture.observer->start();
    fixture.await([](const Json& status) {
        return status.value("error", std::string{}).find("incomplete or invalid JSON") != std::string::npos;
    });
    fixture.observer->shutdown();
    fixture.complete();
    fixture.observer->start();
    const auto restored = fixture.await([&](const Json& status) {
        return status.contains("context_telemetry") && status["context_telemetry"].value("available", false) &&
            status.value("error", std::string{}) == controlError;
    });
    fixture.observer->shutdown();
    require(restored.value("error", std::string{}) == controlError,
        "observation recovery swallowed an unresolved workspace control failure");
}


void nativeDeliveryAndMatchedToolEvidence()
{
    ConversationFixture fixture;
    const std::string handed = "Resume packet packet-123\nExact saved packet body";
    const std::string canonical = R"({"agents":[{"id":"review"}],"ok":true})";
    const auto blocks = Json::array({Json{{"type", "text"}, {"text", canonical}}}).dump();
    const Json request{{"type", "toolCallRequest"}, {"callId", 42U},
        {"toolCallRequestId", "request-42"}, {"name", "agent_list"},
        {"parameters", Json::object()}, {"pluginIdentifier", "mcp/forge-conductor"}};
    const Json result{{"type", "toolCallResult"}, {"callId", 42U},
        {"toolCallRequestId", "request-42"}, {"content", blocks}};
    const Json orphan{{"type", "toolCallResult"}, {"callId", 77U},
        {"name", "agent_list"}, {"content", blocks}};
    const Json mismatchedRequest{{"type", "toolCallResult"}, {"callId", 42U},
        {"toolCallRequestId", "a-different-native-request"}, {"name", "agent_list"},
        {"content", blocks}};
    const Json user{{"type", "singleStep"}, {"role", "user"},
        {"content", Json::array({
            Json{{"type", "text"}, {"text", "Resume packet packet-123\n"}},
            Json{{"type", "text"}, {"text", "Exact saved packet body"}}})}};
    const Json unselectedUser{{"type", "singleStep"}, {"role", "user"},
        {"content", Json::array({Json{{"type", "text"}, {"text", "wrong packet"}}})}};
    const Json content{{"type", "contentBlock"},
        {"content", Json::array({request, mismatchedRequest, result, orphan})}};
    fixture.save(conversation(Json::array({
        message(Json::array({unselectedUser, user}), 1U),
        message(Json::array({version(Json::array({content}))}))})));
    TestContext context;
    const auto observation = take(WindowsLMStudioConversationReader::read(
        fixture.path(), context.active()));
    require(observation && observation->userMessages == std::vector<std::string>{handed},
        "delivery evidence did not match exact selected native user text");
    require(observation->nativeToolResults.size() == 1U,
        "an orphan result or conflicting native request identifier was credited");
    const auto& evidence = observation->nativeToolResults.front();
    require(evidence.name == "agent_list" &&
        evidence.pluginIdentifier == "mcp/forge-conductor" &&
        evidence.requestId == "request-42" && evidence.content == blocks &&
        evidence.textBodies == std::vector<std::string>{canonical},
        "matched native tool result evidence lost its exact binding or body");
}

Json nativeFragmentBlocks(const std::string& canonical)
{
    std::vector<std::string> parts;
    for (std::size_t start{}; start < canonical.size();) {
        auto end = (std::min)(canonical.size(), start + 12U * 1024U);
        while (end < canonical.size() &&
            (static_cast<unsigned char>(canonical[end]) & 0xc0U) == 0x80U) --end;
        parts.push_back(canonical.substr(start, end - start));
        start = end;
    }
    Json blocks = Json::array();
    for (std::size_t index{}; index < parts.size(); ++index) {
        const auto body = Json{{"kind", "forge_tool_result_fragment"}, {"version", 1U},
            {"index", index}, {"count", parts.size()}, {"total_bytes", canonical.size()},
            {"part", parts.at(index)},
            {"instruction", "Concatenate part from every fragment in index order, then parse the complete JSON tool result. Do not repeat the tool call."}}.dump();
        require(body.size() <= 32U * 1024U, "native fixture fragment exceeded bridge-safe limit");
        blocks.push_back(Json{{"type", "text"}, {"text", body}});
    }
    require(blocks.size() > 1U, "native fixture did not exercise fragmentation");
    return blocks;
}

void fragmentedNativeContinuityResults()
{
    ConversationFixture fixture;
    std::string longText;
    const std::string unit = std::string{"\x01\"\\\n"} + "\xe6\xb8\xac\xf0\x9f\xa7\xaa";
    for (std::size_t index{}; index < 6000U; ++index) longText += unit;
    const std::vector<std::pair<std::string, Json>> cases{
        {"session_handoff", Json{{"ok", true}, {"handoff_id", "large-native-packet"},
            {"resume_seed", longText}, {"packet", {{"goal", "Retain the original task"}, {"narrative", longText}}}}},
        {"context_get", Json{{"ok", true}, {"found", true}, {"handoff_id", "large-native-packet"}, {"narrative", longText}}},
        {"agent_get", Json{{"ok", false}, {"code", "invalid_request"}, {"message", longText}}}};
    Json steps = Json::array();
    std::uint64_t callId{};
    std::vector<std::string> raw;
    for (const auto& [name, payload] : cases) {
        ++callId;
        raw.push_back(nativeFragmentBlocks(payload.dump()).dump());
        const auto requestId = "fragmented-request-" + std::to_string(callId);
        steps.push_back(Json{{"type", "contentBlock"}, {"content", Json::array({
            Json{{"type", "toolCallRequest"}, {"callId", callId}, {"name", name},
                {"toolCallRequestId", requestId}, {"pluginIdentifier", "mcp/forge-conductor"}},
            Json{{"type", "toolCallResult"}, {"callId", callId},
                {"toolCallRequestId", requestId}, {"content", raw.back()}}})}});
    }
    steps.push_back(generation(18000U, 262144U));
    fixture.save(conversation(Json::array({message(Json::array({version(steps)}))})));
    TestContext context;
    const auto observed = take(WindowsLMStudioConversationReader::read(fixture.path(), context.active()));
    require(observed && observed->nativeToolResults.size() == cases.size(), "fragmented native calls lost identity");
    for (std::size_t index{}; index < cases.size(); ++index) {
        const auto& result = observed->nativeToolResults.at(index);
        require(result.name == cases.at(index).first && result.content == raw.at(index) &&
            result.requestId == "fragmented-request-" + std::to_string(index + 1U) &&
            result.pluginIdentifier == "mcp/forge-conductor", "fragment reassembly changed raw boundary evidence");
        require(result.textBodies == std::vector<std::string>{cases.at(index).second.dump()},
            "native continuity did not receive one exact complete semantic result");
        require(Json::parse(result.textBodies.front()) == cases.at(index).second,
            "native result lost escaped Unicode or error data");
    }
    const auto recovered = Json::parse(observed->nativeToolResults.at(1).textBodies.front());
    require(recovered.at("ok") == true && recovered.at("found") == true &&
        recovered.at("handoff_id") == "large-native-packet", "successor acknowledgement fields were not recovered");
    const auto rejected = Json::parse(observed->nativeToolResults.at(2).textBodies.front());
    require(rejected.at("ok") == false && rejected.at("code") == "invalid_request",
        "native error fragments were fabricated as success");
}

void malformedNativeFragmentSetsCannotAcknowledgeContinuity()
{
    ConversationFixture fixture;
    const auto payload = Json{{"ok", true}, {"found", true}, {"handoff_id", "large-native-packet"},
        {"narrative", std::string(70U * 1024U, 'x')}}.dump();
    const auto valid = nativeFragmentBlocks(payload);
    std::vector<Json> invalid;
    auto missing = valid; missing.erase(missing.end() - 1); invalid.push_back(missing);
    auto duplicate = valid; duplicate.push_back(valid.back()); invalid.push_back(duplicate);
    auto reordered = valid; std::swap(reordered[0], reordered[1]); invalid.push_back(reordered);
    auto mixed = valid; mixed.push_back(Json{{"type", "text"}, {"text", R"({"ok":true,"found":true,"handoff_id":"fabricated"})"}}); invalid.push_back(mixed);
    auto image = valid; image.push_back(Json{{"type", "image"}}); invalid.push_back(image);
    auto malformed = valid; malformed[0]["text"] = "{"; invalid.push_back(malformed);
    auto duplicateKey = valid;
    auto duplicateBody = duplicateKey[0]["text"].get<std::string>();
    duplicateBody.insert(1U, "\"index\":0,");
    duplicateKey[0]["text"] = duplicateBody; invalid.push_back(duplicateKey);
    const auto altered = [&](const char* field, const Json& value) {
        auto blocks = valid;
        auto fragment = Json::parse(blocks[0]["text"].get<std::string>());
        fragment[field] = value; blocks[0]["text"] = fragment.dump(); invalid.push_back(std::move(blocks));
    };
    altered("version", 2U);
    altered("index", valid.size());
    altered("index", "0");
    altered("count", valid.size() + 1U);
    altered("total_bytes", payload.size() + 1U);
    altered("total_bytes", 1024U * 1024U + 1U);
    altered("part", "");
    altered("part", "not JSON");
    altered("kind", "wrong_fragment_kind");
    for (const auto& blocks : invalid) {
        const auto raw = blocks.dump();
        fixture.save(conversation(Json::array({message(Json::array({version(Json::array({
            Json{{"type", "contentBlock"}, {"content", Json::array({
                Json{{"type", "toolCallRequest"}, {"callId", 1U}, {"name", "context_get"},
                    {"toolCallRequestId", "malformed-native"}, {"pluginIdentifier", "mcp/forge-conductor"}},
                Json{{"type", "toolCallResult"}, {"callId", 1U},
                    {"toolCallRequestId", "malformed-native"}, {"content", raw}}})}}}))}))})));
        TestContext context;
        const auto observed = take(WindowsLMStudioConversationReader::read(fixture.path(), context.active()));
        require(observed && observed->nativeToolResults.size() == 1U &&
            observed->nativeToolResults.front().content == raw, "invalid fragments lost raw native evidence");
        require(observed->nativeToolResults.front().textBodies.empty(),
            "invalid or incomplete fragments fabricated semantic recovery evidence");
    }
}

void chronologicalSelectedNativeToolEvidence()
{
    ConversationFixture fixture;
    const auto toolStep = [](const std::string& name, const std::uint64_t callId,
                             const std::string& requestId, const Json& payload) {
        const auto blocks = Json::array({
            Json{{"type", "text"}, {"text", payload.dump()}}}).dump();
        return Json{{"type", "contentBlock"}, {"content", Json::array({
            Json{{"type", "toolCallRequest"}, {"callId", callId},
                {"toolCallRequestId", requestId}, {"name", name},
                {"parameters", Json::object()}, {"pluginIdentifier", "mcp/forge-conductor"}},
            Json{{"type", "toolCallResult"}, {"callId", callId},
                {"toolCallRequestId", requestId}, {"content", blocks}}})}};
    };
    fixture.save(conversation(Json::array({
        message(Json::array({
            version(Json::array({
                toolStep("unselected_tool", 99U, "wrong-version", Json{{"ok", true}}),
                generation(48000U, 48384U, "contextLengthReached")})),
            version(Json::array({
                toolStep("context_get", 1U, "recover-packet", Json{
                    {"ok", true}, {"found", true}, {"handoff_id", "packet-123"}}),
                toolStep("agent_run_status", 2U, "reattach-agent", Json{{"ok", true}}),
                generation(3000U, 32768U)}))}), 1U),
        message(Json::array({version(Json::array({
            toolStep("agent_list", 3U, "following-agent-list", Json{{"ok", true}}),
            toolStep("fs_read", 4U, "following-file-read", Json{{"ok", true}}),
            generation(6000U, 32768U)}))}))})));
    TestContext context;
    const auto observation = take(WindowsLMStudioConversationReader::read(
        fixture.path(), context.active()));
    require(observation && observation->nativeToolResults.size() == 4U,
        "selected native tool evidence was lost or an unselected version was included");
    const auto& evidence = observation->nativeToolResults;
    require(evidence[0].name == "context_get" && evidence[0].requestId == "recover-packet" &&
        evidence[1].name == "agent_run_status" && evidence[1].requestId == "reattach-agent" &&
        evidence[2].name == "agent_list" && evidence[2].requestId == "following-agent-list" &&
        evidence[3].name == "fs_read" && evidence[3].requestId == "following-file-read",
        "native evidence reversed messages or tool steps, obscuring recovery before continuation");
    require(observation->usedTokens == 6000U && observation->contextCapacity == 32768U &&
        Json::parse(observation->generationEvidence) == Json{
            {"message_index", 1U}, {"selected_version", 0U}, {"step_index", 2U},
            {"genInfo", generation(6000U, 32768U)["genInfo"]}} &&
        !observation->overflow,
        "chronological native evidence changed latest selected provider usage");
}

void equalStatisticsIdentifyDistinctSelectedGenerations()
{
    ConversationFixture fixture;
    const auto sameGeneration = generation(18000U, 32768U);
    auto saved = conversation(Json::array({message(Json::array({
        version(Json::array({sameGeneration})),
        version(Json::array({sameGeneration}))}))}));
    TestContext context;
    const auto observe = [&]() {
        fixture.save(saved);
        return take(WindowsLMStudioConversationReader::read(fixture.path(), context.active()));
    };
    const auto first = observe();
    require(first.has_value(), "initial equal-stat generation was not observed");
    saved["messages"][0]["currentlySelected"] = 1U;
    const auto selectedVersion = observe();
    require(selectedVersion && selectedVersion->generationEvidence != first->generationEvidence,
        "equal statistics in a different selected version did not rearm continuity");
    saved["messages"][0]["versions"][1]["steps"].push_back(sameGeneration);
    const auto nextStep = observe();
    require(nextStep && nextStep->generationEvidence != selectedVersion->generationEvidence,
        "equal statistics in a later selected step did not rearm continuity");
    saved["messages"].push_back(message(Json::array({version(Json::array({sameGeneration}))})));
    const auto nextMessage = observe();
    require(nextMessage && nextMessage->generationEvidence != nextStep->generationEvidence,
        "equal statistics in a later message did not rearm continuity");
    const auto repeatedRead = observe();
    require(repeatedRead && repeatedRead->generationEvidence == nextMessage->generationEvidence,
        "reading the same selected generation changed its evidence");
    for (const auto* observation : {&*first, &*selectedVersion, &*nextStep, &*nextMessage, &*repeatedRead}) {
        require(observation->usedTokens == 18000U && observation->contextCapacity == 32768U,
            "generation identity changed measured provider usage or capacity");
    }
}

class VisibleDispatchClock final : public Contracts::IClock {
public:
    VisibleDispatchClock() noexcept : ticks_{std::chrono::steady_clock::now().time_since_epoch().count()} {}
    Domain::UtcTimePoint utcNow() const noexcept override {return std::chrono::system_clock::now();}
    Domain::MonotonicTimePoint monotonicNow() const noexcept override {
        return Domain::MonotonicTimePoint{Domain::MonotonicTimePoint::duration{ticks_.load()}};
    }
    void advance(const std::chrono::seconds elapsed) noexcept {
        ticks_.fetch_add(std::chrono::duration_cast<Domain::MonotonicTimePoint::duration>(elapsed).count());
    }
private:
    std::atomic<Domain::MonotonicTimePoint::duration::rep> ticks_;
};

void continuitySendUsesFreshBudgetAndPreservesAuthority() {
    using Controls=Infrastructure::Windows::Detail::LMStudioChatContinuityAccess;
    using Receipt=Infrastructure::Windows::LMStudioChatEffectReceipt;
    enum class Scenario {Confirmed,Cancelled,AuthorityDenied};
    for(const auto scenario:{Scenario::Confirmed,Scenario::Cancelled,Scenario::AuthorityDenied}) {
        VisibleHandoffFixture f;VisibleDispatchClock clock;
        f.fixture.observer.reset();f.upgradeRoutes();
        std::optional<Domain::OperationContext> tickOperation,dispatchOperation;
        std::size_t freshAuthorityChecks{},effectCallbacks{};
        bool cancelledRefused{},authorityRefused{};
        std::string controlFailure;
        f.controls->pause=[&](const Domain::PathText&,std::string_view expected,const Domain::OperationContext& operation) {
            require(expected==f.selected,"private fresh-budget pause targeted another conversation");
            require(operation.deadline==clock.monotonicNow()+std::chrono::seconds{20},
                "ordinary observation tick lost its existing20second budget");
            tickOperation=operation;clock.advance(std::chrono::seconds{30});
            f.fixture.configuration.setNow(clock.monotonicNow());
            require(operation.isExpired(clock.monotonicNow()),"private clock did not exhaust the observation tick budget");
            return Domain::Result<bool>::success(true);
        };
        const auto originalSend=f.controls->send;
        f.controls->send=[&](const Domain::PathText& executable,std::string_view text,bool newChat,
            const Domain::OperationContext& operation,std::optional<std::string_view> expected,
            const std::function<void(std::string_view)>& successor,
            const Infrastructure::Windows::LMStudioChatEffectObserver& receipt) {
            try {
            dispatchOperation=operation;
            require(tickOperation.has_value(),"private dispatch did not follow the observed pause");
            require(operation.deadline==clock.monotonicNow()+std::chrono::seconds{25},
                "visible Send reused the consumed observation deadline instead of a fresh25second budget");
            require(operation.operationId!=tickOperation->operationId &&
                    operation.correlationId==tickOperation->correlationId &&
                    operation.cancellation==tickOperation->cancellation && operation.cancellation.stop_possible(),
                "fresh dispatch context lost its operation identity, correlation or worker cancellation token");
            if(scenario==Scenario::Cancelled) {
                f.fixture.observer->beginShutdown();
                require(operation.isCancellationRequested() && tickOperation->isCancellationRequested(),
                    "fresh dispatch was detached from the published worker cancellation source");
                const auto rejected=receipt({VisibleHandoffFixture::Effect::Send,VisibleHandoffFixture::Stage::BeforeDispatch,f.selected,0U});
                requireError(rejected,Domain::ErrorCodes::Cancelled,"cancelled fresh dispatch passed its configuration callback guard");
                cancelledRefused=true;return rejected;
            }
            clock.advance(std::chrono::seconds{3});f.fixture.configuration.setNow(clock.monotonicNow());
            const Infrastructure::Windows::LMStudioChatEffectObserver checked=[&](const Receipt& effect) {
                ++effectCallbacks;
                auto recorded=receipt(effect);
                if(scenario==Scenario::AuthorityDenied) {
                    requireError(recorded,Domain::ErrorCodes::Unauthorized,"fresh25second dispatch ignored current authority refusal");
                    authorityRefused=true;return recorded;
                }
                if(recorded) {
                    const auto saved=f.checkpoint().at("state");
                    require(saved.at("effect").at("purpose")=="request" && saved.at("effect").at("conversation_id")==f.selected,
                        "fresh dispatch callback checkpoint lost its exact request/target binding");
                    require(saved.at("effect").at("stage")==
                            (effect.stage==VisibleHandoffFixture::Stage::BeforeDispatch?"uncertain":"confirmed"),
                        "fresh dispatch callback checkpoint did not retain the original uncertainty transition");
                }
                return recorded;
            };
            return originalSend(executable,text,newChat,operation,expected,successor,checked);
            } catch(const TestFailure& error) {
                controlFailure=error.what();
                return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,controlFailure));
            }
        };
        const auto freshAuthority=[&](const Domain::ProjectId& project,const Domain::PathText& root,
            const Domain::OperationContext& operation) {
            ++freshAuthorityChecks;
            require(dispatchOperation.has_value() && project==f.fixture.project && root==f.fixture.fileFixture.path(),
                "fresh dispatch callback lost its actual workspace binding");
            require(operation.operationId==dispatchOperation->operationId &&
                    operation.correlationId==dispatchOperation->correlationId &&
                    operation.deadline==dispatchOperation->deadline &&
                    operation.cancellation==dispatchOperation->cancellation && !operation.isExpired(clock.monotonicNow()),
                "fresh authority callback reused the old tick or changed the dispatch context");
            if(scenario==Scenario::AuthorityDenied) return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,"Private fresh dispatch authority refusal"));
            return Domain::Result<void>::success();
        };
        f.fixture.observer=Controls::createScoped(f.controls,std::nullopt,freshAuthority,f.fixture.project,
            f.fixture.fileFixture.path(),f.home(),f.fixture.fileFixture.path(),f.fixture.fileFixture.path(),
            Domain::LocalModelConfig{},f.fixture.memory,f.fixture.legacyContinuity,f.fixture.projects,
            clock,f.fixture.uuid,f.fixture.configuration,true);
        f.fixture.fileFixture.save(conversation(Json::array({message(Json::array({version(Json::array({generation(31000U,32768U)}))}))})));
        const auto status=f.run([](const Json& value) {
            return value.value("state",std::string{})=="waiting_for_model_packet" || value.contains("error");
        });
        require(controlFailure.empty(),controlFailure);
        const auto state=f.checkpoint().at("state");
        require(tickOperation && dispatchOperation,"fresh dispatch was not exercised: "+status.dump());
        if(scenario==Scenario::Confirmed) {
            require(status.value("state",std::string{})=="waiting_for_model_packet" && !status.contains("error") &&
                    f.sends==1U && f.creations==0U && freshAuthorityChecks==1U && effectCallbacks==2U &&
                    state.at("packet_request_acknowledged")==true && state.at("effect").at("stage")=="confirmed",
                "fresh dispatch did not publish one exact confirmed packet request: "+status.dump());
            require(f.fixture.configuration.calls()==2U,"fresh dispatch did not preserve the initial and BeforeDispatch configuration rereads");
        } else {
            require(f.sends==0U && f.creations==0U && state.at("effect").is_null() &&
                    state.at("packet_request_acknowledged")==false,
                "refused fresh dispatch recorded or performed an uncertain native effect");
            require(scenario==Scenario::Cancelled?(cancelledRefused && freshAuthorityChecks==0U):
                    (authorityRefused && freshAuthorityChecks==1U && effectCallbacks==1U),
                "fresh dispatch refusal did not reach its original cancellation/current authority guard");
        }
    }
}

void freshEmptyConversation()
{
    ConversationFixture fixture;
    fixture.save(conversation(Json::array()));
    TestContext context;
    const auto observation = take(WindowsLMStudioConversationReader::read(
        fixture.path(), context.active()));
    require(observation && observation->conversationId == "project/chat.conversation.json",
        "a freshly created empty conversation lost its native identifier");
    require(observation->usedTokens == 0U && observation->contextCapacity == 0U &&
        !observation->overflow && !observation->toolsActive &&
        observation->userMessages.empty() && observation->nativeToolResults.empty() &&
        observation->generationEvidence.empty(),
        "an empty conversation fabricated usage, delivery, or tool evidence");
}

} // namespace

void registerWindowsLMStudioConversationReaderTests(TestRegistry& tests)
{
    addTest(tests, "LMStudioChatContinuity.fresh_dispatch_budget_preserves_cancellation_and_authority",
        continuitySendUsesFreshBudgetAndPreservesAuthority);
    addTest(tests, "LMStudioConversationReader.selected_provider_statistics",
        selectedProviderStatisticsAndTerminalTools);
    addTest(tests, "LMStudioConversationReader.cached_prompt_separate_provider_statistics",
        cachedRenderedPromptRemainsSeparateFromProviderStatistics);
    addTest(tests, "LMStudioConversationReader.cached_prompt_numeric_admission",
        cachedRenderedPromptNumericAdmission);
    addTest(tests, "LMStudioConversationReader.cached_prompt_selected_model_binding",
        cachedRenderedPromptRequiresSelectedModelBinding);
    addTest(tests, "LMStudioConversationReader.cached_prompt_requires_provider_generation",
        cachedRenderedPromptRequiresValidProviderGeneration);
    addTest(tests, "LMStudioConversationReader.overflow_and_active_tools",
        overflowAndActiveTools);
    addTest(tests, "LMStudioConversationReader.partial_files_and_path",
        partialFilesAndSelectionBoundary);
    addTest(tests, "LMStudioConversationReader.missing_selection_and_cancel",
        missingSelectionAndCancellation);
    addTest(tests, "LMStudioChatContinuity.cached_prompt_requests_packet_with_separate_provider_evidence",
        cachedPromptPressureRequestsPacketWithSeparateProviderEvidence);
    addTest(tests, "LMStudioChatContinuity.cached_prompt_maximum_and_invalid_cache",
        cachedPromptPressureUsesMaximumWithoutPromotingInvalidCache);
    addTest(tests, "LMStudioChatContinuity.cached_prompt_preserves_provider_pressure_and_overflow",
        cachedPromptPressurePreservesProviderPressureAndOverflow);
    addTest(tests, "LMStudioChatContinuity.cached_prompt_confirmed_pause_reread",
        cachedPromptPressureRereadsProjectionAtConfirmedPause);
    addTest(tests, "LMStudioChatContinuity.cached_prompt_trace_dedupe_and_reentry",
        cachedPromptPressureTraceDeduplicatesAndReentersAfterNormal);
    addTest(tests, "LMStudioChatContinuity.cached_prompt_active_tools",
        cachedPromptPressureWaitsForActiveTools);
    addTest(tests, "LMStudioChatContinuity.cached_prompt_successor_budget_clear",
        cachedPromptPressureControlsSuccessorBudgetClear);
    addTest(tests, "LMStudioChatContinuity.completed_successor_cache_only_pressure_change",
        completedSuccessorRechecksChangedCacheWithoutNewGeneration);
    addTest(tests, "LMStudioChatContinuity.cached_prompt_no_selection_clears_telemetry",
        cachedPromptTelemetryClearsWhenSelectionDisappears);
    addTest(tests, "LMStudioChatContinuity.partial_write_recovery_status",
        continuityObservationRecoversAfterPartialWrite);
    addTest(tests, "LMStudioChatContinuity.preference_pipeline_recovery",
        continuityObservationPipelineRecovery);
    addTest(tests, "LMStudioChatContinuity.configuration_read_recovery",
        continuityConfigurationReadRecovery);
    addTest(tests, "LMStudioChatContinuity.mcp_route_read_recovery",
        continuityMcpRouteReadRecovery);
    addTest(tests, "LMStudioChatContinuity.observation_recovery_preserves_workspace_control_error",
        observationRecoveryPreservesWorkspaceControlFailure);
    addTest(tests, "LMStudioChatContinuity.successful_null_observation_statuses",
        successfulNullObservationStatusesAndRecovery);
    addTest(tests, "LMStudioChatContinuity.reconstruction_preserves_waiting_packet",
        observerReconstructionRetainsWaitingPacketWithoutResending);
    addTest(tests, "LMStudioChatContinuity.durable_recovery_cases",
        durableVisibleHandoffRecoveryCases);
    addTest(tests, "LMStudioChatContinuity.explicit_route_recovery_cases",
        explicitVisibleRouteRecoveryCases);
    addTest(tests, "LMStudioConversationReader.native_delivery_and_result_evidence",
        nativeDeliveryAndMatchedToolEvidence);
    addTest(tests, "LMStudioConversationReader.fragmented_native_continuity",
        fragmentedNativeContinuityResults);
    addTest(tests, "LMStudioConversationReader.invalid_fragment_sets",
        malformedNativeFragmentSetsCannotAcknowledgeContinuity);
    addTest(tests, "LMStudioConversationReader.chronological_selected_native_tools",
        chronologicalSelectedNativeToolEvidence);
    addTest(tests, "LMStudioConversationReader.equal_statistics_distinct_generations",
        equalStatisticsIdentifyDistinctSelectedGenerations);
    addTest(tests, "LMStudioConversationReader.fresh_empty_conversation",
        freshEmptyConversation);
}

} // namespace ForgeConductor::Tests
