#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioConversationReader.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;
using Observation = std::optional<LMStudioConversationObservation>;
constexpr std::uintmax_t MaximumConversationBytes = 64U * 1024U * 1024U;

void check(const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) {
        throw Domain::makeError(
            Domain::ErrorCodes::Cancelled, "LM Studio conversation read cancelled.");
    }
    if (context.isExpired(std::chrono::steady_clock::now())) {
        throw Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded,
            "LM Studio conversation read deadline expired.", true);
    }
}

std::filesystem::path nativePath(const std::string_view text)
{
    return std::filesystem::path{std::u8string_view{
        reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

std::string pathText(const std::filesystem::path& path)
{
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

bool samePath(const std::filesystem::path& left, const std::filesystem::path& right)
{
    const auto a = left.lexically_normal().native();
    const auto b = right.lexically_normal().native();
    return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
        b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool containedPath(
    const std::filesystem::path& root, const std::filesystem::path& candidate)
{
    auto child = candidate.begin();
    for (auto parent = root.begin(); parent != root.end(); ++parent, ++child) {
        if (child == candidate.end() || !samePath(*parent, *child)) {
            return false;
        }
    }
    return child != candidate.end();
}

std::optional<Json> readJson(
    const std::filesystem::path& path,
    const Domain::OperationContext& context)
{
    check(context);
    std::error_code error;
    if (!std::filesystem::exists(path, error) && !error) {
        return std::nullopt;
    }
    const auto bytes = std::filesystem::file_size(path, error);
    if (error) {
        throw Domain::makeError(Domain::ErrorCodes::RecordNotFound,
            "Cannot read LM Studio file metadata: " + pathText(path), true);
    }
    if (bytes > MaximumConversationBytes) {
        throw Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
            "LM Studio conversation exceeds the 64 MiB read limit: " + pathText(path));
    }
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) {
        throw Domain::makeError(Domain::ErrorCodes::RecordNotFound,
            "Cannot read LM Studio file revision: " + pathText(path), true);
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw Domain::makeError(Domain::ErrorCodes::RecordNotFound,
            "Cannot open LM Studio file: " + pathText(path), true);
    }
    std::string content;
    content.reserve(static_cast<std::size_t>(bytes));
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        check(context);
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            content.append(buffer.data(), static_cast<std::size_t>(count));
        }
        if (content.size() > MaximumConversationBytes) {
            throw Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                "LM Studio conversation grew beyond the read limit: " + pathText(path));
        }
    }
    const auto afterBytes = std::filesystem::file_size(path, error);
    const auto afterModified = error
        ? std::filesystem::file_time_type{}
        : std::filesystem::last_write_time(path, error);
    if (!input.eof() || error || bytes != afterBytes ||
        modified != afterModified || content.size() != bytes) {
        throw Domain::makeError(Domain::ErrorCodes::Conflict,
            "LM Studio file changed during its read; retry: " + pathText(path), true);
    }
    auto document = Json::parse(content, nullptr, false);
    if (document.is_discarded()) {
        throw Domain::makeError(Domain::ErrorCodes::MalformedMessage,
            "LM Studio file is incomplete or invalid JSON; retry: " + pathText(path), true);
    }
    return document;
}

std::optional<std::uint64_t> tokenNumber(const Json& value)
{
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
        return std::nullopt;
    }
    return value.get<std::uint64_t>();
}

std::optional<std::uint64_t> fieldTokens(
    const Json& config, const std::string_view key)
{
    if (!config.is_object() || !config.contains("fields") ||
        !config["fields"].is_array()) {
        return std::nullopt;
    }
    for (const auto& field : config["fields"]) {
        if (field.is_object() && field.contains("key") && field["key"].is_string() &&
            field["key"].get_ref<const std::string&>() == key &&
            field.contains("value")) {
            return tokenNumber(field["value"]);
        }
    }
    return std::nullopt;
}

std::string selectedChat(const Json& config)
{
    if (!config.is_object()) {
        throw Domain::makeError(Domain::ErrorCodes::MalformedMessage,
            "LM Studio conversation config is not an object.", true);
    }
    const auto selected = config.find("selectedConversation");
    if (selected == config.end() || selected->is_null()) {
        return {};
    }
    if (!selected->is_string()) {
        throw Domain::makeError(Domain::ErrorCodes::MalformedMessage,
            "LM Studio selectedConversation is not a path string.", true);
    }
    return selected->get<std::string>();
}

std::string projectIdentifier(
    const std::filesystem::path& root,
    const Domain::OperationContext& context)
{
    const auto registry = readJson(
        root / L".internal" / L"projects-registry.json", context);
    // LM Studio's default project is its root; project registries are optional
    // in a fresh profile and use this identifier in the installed renderer.
    if (!registry) {
        return "default-project-identifier";
    }
    const auto& document = registry->contains("json") ? registry->at("json") : *registry;
    if (!document.is_object() || !document.contains("projects") ||
        !document["projects"].is_array()) {
        throw Domain::makeError(Domain::ErrorCodes::MalformedMessage,
            "LM Studio project registry has no project map.", true);
    }
    for (const auto& entry : document["projects"]) {
        if (!entry.is_array() || entry.size() != 2U || !entry[0].is_string() ||
            !entry[1].is_object() || !entry[1].contains("path") ||
            !entry[1]["path"].is_string()) {
            continue;
        }
        std::error_code error;
        const auto registered = std::filesystem::weakly_canonical(
            nativePath(entry[1]["path"].get<std::string>()), error);
        if (!error && samePath(root, registered)) {
            return entry[0].get<std::string>();
        }
    }
    throw Domain::makeError(Domain::ErrorCodes::ProjectNotFound,
        "The LM Studio root is absent from its project registry.", true);
}

bool activeTool(const std::string_view type)
{
    return type == "generatingToolCall" || type == "toolCallQueued" ||
        type == "confirmingToolCall" || type == "callingTool";
}


// The installed native schema stores tool results as content strings and keeps
// the associated request's plugin identity on the preceding request part.
std::vector<std::string> nativeTextBodies(const Json& blocks)
{
    std::vector<std::string> bodies;
    bool fragmented{};
    if (!blocks.is_array()) return bodies;
    for (const auto& block : blocks) {
        if (!block.is_object() || block.value("type", std::string{}) != "text" ||
            !block.contains("text") || !block.at("text").is_string()) continue;
        bodies.push_back(block.at("text").get<std::string>());
        const auto value = Json::parse(bodies.back(), nullptr, false);
        if (value.is_object() && value.contains("kind") &&
            value.at("kind") == "forge_tool_result_fragment") fragmented = true;
    }
    if (!fragmented) return bodies;
    // Preserve the raw per-call content separately. Invalid fragment sets never
    // become semantic tool results that could acknowledge native continuity.
    if (bodies.size() < 2U || bodies.size() > 128U || bodies.size() != blocks.size()) return {};
    constexpr std::size_t MaximumPayloadBytes = 1024U * 1024U;
    std::string assembled;
    std::uint64_t total{};
    for (std::size_t index{}; index < bodies.size(); ++index) {
        if (bodies[index].size() > 32U * 1024U) return {};
        const auto value = Json::parse(bodies[index], nullptr, false);
        if (!value.is_object() || value.dump() != bodies[index] || value.size() != 7U ||
            !value.contains("kind") || value.at("kind") != "forge_tool_result_fragment" ||
            !value.contains("version") || !value.contains("index") ||
            !value.contains("count") || !value.contains("total_bytes") ||
            !value.contains("part") || !value.at("part").is_string() ||
            !value.contains("instruction") || value.at("instruction") !=
                "Concatenate part from every fragment in index order, then parse the complete JSON tool result. Do not repeat the tool call.") return {};
        const auto version = tokenNumber(value.at("version"));
        const auto ordinal = tokenNumber(value.at("index"));
        const auto count = tokenNumber(value.at("count"));
        const auto bytes = tokenNumber(value.at("total_bytes"));
        if (!version || *version != 1U || !ordinal || *ordinal != index ||
            !count || *count != bodies.size() || !bytes || *bytes == 0U ||
            *bytes > MaximumPayloadBytes) return {};
        if (index == 0U) { total = *bytes; assembled.reserve(static_cast<std::size_t>(total)); }
        if (*bytes != total) return {};
        const auto& part = value.at("part").get_ref<const std::string&>();
        if (part.empty() || part.size() > 12U * 1024U || !Domain::isValidUtf8(part) ||
            part.size() > total - assembled.size()) return {};
        assembled.append(part);
    }
    if (assembled.size() != total) return {};
    const auto payload = Json::parse(assembled, nullptr, false);
    if (!payload.is_object() || payload.dump() != assembled) return {};
    return {std::move(assembled)};
}

void observeNativeContent(
    const Json& version, LMStudioConversationObservation& observation)
{
    struct RequestEvidence final {
        std::string name;
        std::string pluginIdentifier;
        std::string requestId;
    };
    std::map<std::string, RequestEvidence> requests;
    std::string userText;
    const bool user = version.value("role", std::string{}) == "user";
    const auto collect = [&](const Json& content) {
        if (!content.is_array()) {
            return;
        }
        for (const auto& part : content) {
            if (!part.is_object()) {
                continue;
            }
            const auto type = part.value("type", std::string{});
            if (user && type == "text" && part.contains("text") && part["text"].is_string()) {
                userText += part["text"].get<std::string>();
            }
            if (!part.contains("callId") || !part["callId"].is_number()) {
                continue;
            }
            const auto callId = part["callId"].dump();
            if (type == "toolCallRequest" && part.contains("name") && part["name"].is_string()) {
                requests[callId] = RequestEvidence{
                    part["name"].get<std::string>(),
                    part.value("pluginIdentifier", std::string{}),
                    part.value("toolCallRequestId", std::string{})};
            } else if (type == "toolCallResult" && part.contains("content") &&
                part["content"].is_string()) {
                const auto request = requests.find(callId);
                if (request == requests.end()) {
                    continue;
                }
                const auto name = part.value("name", request->second.name);
                if (name != request->second.name) {
                    continue;
                }
                const auto resultRequestId = part.value("toolCallRequestId", std::string{});
                if (!request->second.requestId.empty() && !resultRequestId.empty() &&
                    resultRequestId != request->second.requestId) {
                    continue;
                }
                LMStudioNativeToolResult result{
                    name, part["content"].get<std::string>(),
                    request->second.pluginIdentifier,
                    resultRequestId.empty() ? request->second.requestId : resultRequestId, {}};
                const auto blocks = Json::parse(result.content, nullptr, false);
                result.textBodies = nativeTextBodies(blocks);
                observation.nativeToolResults.push_back(std::move(result));
            }
        }
    };
    if (version.contains("content")) {
        collect(version["content"]);
    }
    if (version.contains("steps") && version["steps"].is_array()) {
        for (const auto& step : version["steps"]) {
            if (step.is_object() && step.value("type", std::string{}) == "contentBlock" &&
                step.contains("content")) {
                collect(step["content"]);
            }
        }
    }
    if (user) {
        observation.userMessages.push_back(std::move(userText));
    }
}

void observeConversation(
    const Json& document, LMStudioConversationObservation& observation)
{
    if (!document.is_object() || !document.contains("messages") ||
        !document["messages"].is_array()) {
        throw Domain::makeError(Domain::ErrorCodes::MalformedMessage,
            "LM Studio conversation has no messages array.", true);
    }
    if (document.contains("plugins") && document["plugins"].is_array()) {
        for (const auto& plugin : document["plugins"]) {
            if (plugin.is_string()) {
                observation.plugins.push_back(plugin.get<std::string>());
            }
        }
    }
    bool foundUsage{};
    bool foundStopReason{};
    const auto& messages = document["messages"];
    auto messageIndex = messages.size();
    for (auto message = messages.rbegin(); message != messages.rend(); ++message) {
        --messageIndex;
        if (!message->is_object() || !message->contains("versions") ||
            !(*message)["versions"].is_array() ||
            !message->contains("currentlySelected")) {
            throw Domain::makeError(Domain::ErrorCodes::MalformedMessage,
                "LM Studio message version selection is incomplete.", true);
        }
        const auto selected = tokenNumber((*message)["currentlySelected"]);
        const auto& versions = (*message)["versions"];
        if (!selected || *selected >= versions.size()) {
            throw Domain::makeError(Domain::ErrorCodes::MalformedMessage,
                "LM Studio selected message version is out of range.", true);
        }
        const auto& version = versions[static_cast<std::size_t>(*selected)];
        if (!version.is_object() || !version.contains("steps") ||
            !version["steps"].is_array()) {
            // User messages are singleStep and contain content rather than steps.
            if (version.is_object() && version.value("type", std::string{}) == "singleStep") {
                continue;
            }
            throw Domain::makeError(Domain::ErrorCodes::MalformedMessage,
                "LM Studio selected message steps are incomplete.", true);
        }
        auto stepIndex = version["steps"].size();
        for (auto step = version["steps"].rbegin(); step != version["steps"].rend(); ++step) {
            --stepIndex;
            if (!step->is_object()) {
                continue;
            }
            if (step->value("type", std::string{}) == "toolStatus" &&
                step->contains("statusState") && (*step)["statusState"].is_object()) {
                const auto& state = (*step)["statusState"];
                if (state.contains("status") && state["status"].is_object()) {
                    observation.toolsActive = observation.toolsActive ||
                        activeTool(state["status"].value("type", std::string{}));
                }
            }
            if (foundUsage || !step->contains("genInfo") || !(*step)["genInfo"].is_object()) {
                continue;
            }
            const auto& info = (*step)["genInfo"];
            if (!info.contains("stats") || !info["stats"].is_object()) {
                continue;
            }
            const auto& stats = info["stats"];
            if (!foundStopReason) {
                observation.stopReason = stats.value("stopReason", std::string{});
                observation.overflow = observation.stopReason == "contextLengthReached";
                foundStopReason = true;
            }
            auto used = stats.contains("totalTokensCount")
                ? tokenNumber(stats["totalTokensCount"]) : std::nullopt;
            if (!used && stats.contains("promptTokensCount") &&
                stats.contains("predictedTokensCount")) {
                const auto prompt = tokenNumber(stats["promptTokensCount"]);
                const auto predicted = tokenNumber(stats["predictedTokensCount"]);
                if (prompt && predicted &&
                    *predicted <= (std::numeric_limits<std::uint64_t>::max)() - *prompt) {
                    used = *prompt + *predicted;
                }
            }
            const auto capacity = info.contains("loadModelConfig")
                ? fieldTokens(info["loadModelConfig"], "llm.load.contextLength")
                : std::nullopt;
            if (used && capacity && *capacity > 0U) {
                observation.usedTokens = *used;
                observation.contextCapacity = *capacity;
                const auto model = document.find("lastUsedModel");
                if (model != document.end() && model->is_object() &&
                    model->contains("identifier") && (*model)["identifier"].is_string() &&
                    !(*model)["identifier"].get_ref<const std::string&>().empty() &&
                    info.contains("identifier") && info["identifier"].is_string() &&
                    (*model)["identifier"] == info["identifier"] &&
                    model->contains("instanceLoadTimeConfig") &&
                    fieldTokens((*model)["instanceLoadTimeConfig"], "llm.load.contextLength") == capacity &&
                    document.contains("tokenCount")) {
                    observation.cachedRenderedPromptTokens = tokenNumber(document["tokenCount"]);
                }
                observation.generationEvidence = Json{
                    {"message_index", messageIndex}, {"selected_version", *selected},
                    {"step_index", stepIndex}, {"genInfo", info}}.dump();
                foundUsage = true;
            }
        }
    }
    // The reverse scan above validates every selected version and finds the
    // latest provider usage. Native delivery and tool evidence must retain
    // chronological message order, including the forward step order within it.
    for (const auto& message : messages) {
        const auto selected = message.at("currentlySelected").get<std::size_t>();
        observeNativeContent(message.at("versions").at(selected), observation);
    }
    if (!foundUsage && document.contains("lastUsedModel") &&
        document["lastUsedModel"].is_object() &&
        document["lastUsedModel"].contains("instanceLoadTimeConfig")) {
        observation.contextCapacity = fieldTokens(
            document["lastUsedModel"]["instanceLoadTimeConfig"],
            "llm.load.contextLength").value_or(0U);
    }
}

} // namespace

Domain::Result<std::optional<LMStudioConversationObservation>>
WindowsLMStudioConversationReader::read(
    const Domain::PathText& lmStudioRoot,
    const Domain::OperationContext& context) noexcept
{
    try {
        check(context);
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(
            nativePath(lmStudioRoot.value()), error);
        if (error) {
            return Domain::Result<Observation>::failure(Domain::makeError(
                Domain::ErrorCodes::RecordNotFound, "LM Studio root is unavailable.", true));
        }
        const auto configPath = root / L".internal" / L"conversation-config.json";
        const auto config = readJson(configPath, context);
        if (!config) {
            return Domain::Result<Observation>::success(std::nullopt);
        }
        const auto selected = selectedChat(*config);
        if (selected.empty()) {
            return Domain::Result<Observation>::success(std::nullopt);
        }
        const auto relative = nativePath(selected);
        const auto repository = std::filesystem::weakly_canonical(root / L"conversations", error);
        if (error || relative.is_absolute() || relative.has_root_path()) {
            return Domain::Result<Observation>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "LM Studio selectedConversation must name a relative conversation file."));
        }
        const auto conversation = std::filesystem::weakly_canonical(repository / relative, error);
        if (error || !containedPath(repository, conversation)) {
            return Domain::Result<Observation>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "LM Studio selectedConversation is outside its conversations repository."));
        }
        const auto document = readJson(conversation, context);
        if (!document) {
            return Domain::Result<Observation>::failure(Domain::makeError(
                Domain::ErrorCodes::RecordNotFound,
                "The selected LM Studio conversation is not yet readable.", true));
        }
        LMStudioConversationObservation observation;
        observation.conversationId = selected;
        observation.conversationPath = pathText(conversation);
        observation.projectIdentifier = projectIdentifier(root, context);
        observeConversation(*document, observation);
        const auto currentConfig = readJson(configPath, context);
        if (!currentConfig || selectedChat(*currentConfig) != selected) {
            return Domain::Result<Observation>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "LM Studio changed the selected conversation during its read.", true));
        }
        check(context);
        return Domain::Result<Observation>::success(std::move(observation));
    } catch (const Domain::Error& error) {
        return Domain::Result<Observation>::failure(error);
    } catch (const Json::exception& error) {
        return Domain::Result<Observation>::failure(Domain::makeError(
            Domain::ErrorCodes::MalformedMessage,
            std::string{"LM Studio conversation schema is incomplete: "} + error.what(), true));
    } catch (const std::exception& error) {
        return Domain::Result<Observation>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            std::string{"LM Studio conversation read failed: "} + error.what(), true));
    } catch (...) {
        return Domain::Result<Observation>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "LM Studio conversation read failed.", true));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
