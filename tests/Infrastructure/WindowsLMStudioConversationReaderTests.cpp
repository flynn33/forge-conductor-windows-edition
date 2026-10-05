#include "TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioConversationReader.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
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
