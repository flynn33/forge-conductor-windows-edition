#include "TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioConversationReader.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

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
        "reader used unselected version or aggregate tokenCount");
    require(Json::parse(observation->generationEvidence) == Json{
        {"message_index", 0U}, {"selected_version", 1U}, {"step_index", 0U},
        {"genInfo", generation(18000U, 32768U)["genInfo"]}},
        "generation evidence did not identify the latest selected provider generation");
    require(!observation->overflow && !observation->toolsActive,
        "unselected overflow or completed tool blocked the pause");
    require(observation->plugins.size() == 3U, "plugin bindings were lost");
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
    addTest(tests, "LMStudioConversationReader.selected_provider_statistics",
        selectedProviderStatisticsAndTerminalTools);
    addTest(tests, "LMStudioConversationReader.overflow_and_active_tools",
        overflowAndActiveTools);
    addTest(tests, "LMStudioConversationReader.partial_files_and_path",
        partialFilesAndSelectionBoundary);
    addTest(tests, "LMStudioConversationReader.missing_selection_and_cancel",
        missingSelectionAndCancellation);
    addTest(tests, "LMStudioConversationReader.native_delivery_and_result_evidence",
        nativeDeliveryAndMatchedToolEvidence);
    addTest(tests, "LMStudioConversationReader.chronological_selected_native_tools",
        chronologicalSelectedNativeToolEvidence);
    addTest(tests, "LMStudioConversationReader.equal_statistics_distinct_generations",
        equalStatisticsIdentifyDistinctSelectedGenerations);
    addTest(tests, "LMStudioConversationReader.fresh_empty_conversation",
        freshEmptyConversation);
}

} // namespace ForgeConductor::Tests
