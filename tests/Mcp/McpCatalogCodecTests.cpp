#include "ForgeConductor/Mcp/Mcp.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Mcp = ForgeConductor::Mcp;
using Json = nlohmann::json;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} + #condition}; \
        }                                                                        \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] const Domain::McpToolDescriptor& descriptor(
    const std::span<const Domain::McpToolDescriptor> tools,
    const std::string_view name)
{
    const auto match = std::find_if(
        tools.begin(), tools.end(), [name](const auto& candidate) {
            return candidate.tool.name == name;
        });
    REQUIRE(match != tools.end());
    return *match;
}

[[nodiscard]] Json schema(
    const std::span<const Domain::McpToolDescriptor> tools,
    const std::string_view name)
{
    return Json::parse(descriptor(tools, name).inputSchema);
}

void testCanonicalCatalog()
{
    static_assert(std::is_final_v<Mcp::McpToolCatalog>);
    static_assert(!std::is_copy_constructible_v<Mcp::McpToolCatalog>);
    static_assert(!std::is_move_constructible_v<Mcp::McpToolCatalog>);

    constexpr std::array<std::string_view, 53U> ExpectedNames{
        "agent_context", "agent_get", "agent_list", "agent_recommend",
        "agent_run_complete", "agent_run_start", "agent_run_status",
        "context_get", "context_list", "continuity.acknowledge_handoff",
        "continuity.checkpoint", "continuity.get_pending_handoff",
        "continuity.prepare_handoff", "continuity.request_rollover",
        "continuity.resume", "continuity.status", "forge_status", "fs_delete",
        "fs_edit", "fs_glob", "fs_list", "fs_mkdir", "fs_move", "fs_read",
        "fs_write", "git_add", "git_commit", "git_diff", "git_log",
        "git_status", "memory_delete", "memory_get", "memory_list",
        "memory_search", "memory_set", "pdf_from_file", "pdf_write",
        "project_memory.export", "project_memory.forget", "project_memory.get",
        "project_memory.import", "project_memory.initialize",
        "project_memory.link", "project_memory.list_recent",
        "project_memory.remember", "project_memory.remember_batch",
        "project_memory.search", "project_memory.status",
        "project_memory.update", "search_text", "session_checkpoint",
        "session_handoff", "shell_exec"};

    auto catalog = take(Mcp::McpToolCatalog::create());
    const auto tools = catalog->tools();
    REQUIRE(tools.size() == Mcp::McpToolCatalog::ExpectedToolCount);
    REQUIRE(tools.size() == ExpectedNames.size());
    REQUIRE(std::is_sorted(
        tools.begin(), tools.end(), [](const auto& left, const auto& right) {
            return left.tool.name < right.tool.name;
        }));

    Mcp::McpJsonCodec codec;
    std::size_t readEffects{};
    std::size_t writeEffects{};
    for (std::size_t index{}; index < tools.size(); ++index) {
        REQUIRE(tools[index].tool.name == ExpectedNames[index]);
        REQUIRE(!tools[index].tool.description.empty());
        REQUIRE(!tools[index].tool.pack.empty());
        REQUIRE(tools[index].tool.availability == Domain::ToolAvailability::Available);
        const auto canonical = take(codec.canonicalize(tools[index].inputSchema));
        REQUIRE(canonical == tools[index].inputSchema);
        const auto parsed = Json::parse(canonical);
        REQUIRE(parsed.is_object());
        REQUIRE(parsed.at("type") == "object");
        if (tools[index].tool.effect == Domain::ToolEffect::Read) {
            ++readEffects;
        } else if (tools[index].tool.effect == Domain::ToolEffect::Write) {
            ++writeEffects;
        }
    }
    REQUIRE(readEffects == 23U);
    REQUIRE(writeEffects == 30U);
    REQUIRE(descriptor(tools, "agent_run_status").tool.effect == Domain::ToolEffect::Write);
    REQUIRE(descriptor(tools, "project_memory.export").tool.effect == Domain::ToolEffect::Write);
    REQUIRE(descriptor(tools, "fs_read").tool.description.find("next_offset") !=
        std::string::npos);
    REQUIRE(descriptor(tools, "session_handoff").tool.description.ends_with(
        "Prefer before context is full."));
    REQUIRE(descriptor(tools, "shell_exec").tool.description.find("PowerShell") !=
        std::string::npos);
    REQUIRE(descriptor(tools, "shell_exec").tool.requiresShell);
}

void testSourceSchemasAndWindowsDelta()
{
    auto catalog = take(Mcp::McpToolCatalog::create());
    const auto tools = catalog->tools();

    const auto agentList = schema(tools, "agent_list");
    REQUIRE(agentList == Json({
        {"type", "object"},
        {"properties", Json::object()},
        {"additionalProperties", true}}));

    const auto status = schema(tools, "agent_run_status");
    REQUIRE(status.at("required") == Json::array({"session_id"}));
    REQUIRE(status.at("properties").at("report").at("type") == "object");
    REQUIRE(!status.contains("additionalProperties"));

    const auto fsEdit = schema(tools, "fs_edit");
    REQUIRE(fsEdit == agentList);
    const auto fsRead = schema(tools, "fs_read");
    REQUIRE(fsRead.at("required") == Json::array({"path"}));
    REQUIRE(fsRead.at("properties").at("offset").at("description") ==
        "1-based start line for a partial read");
    REQUIRE(fsRead.at("properties").at("length").at("description") ==
        "Number of lines to return (alias: limit)");

    const auto memorySet = schema(tools, "memory_set");
    REQUIRE(memorySet.at("required") == Json::array({"key", "body"}));
    REQUIRE(memorySet.at("properties").at("content").at("description") ==
        "Alias of body");

    const auto batch = schema(tools, "project_memory.remember_batch");
    REQUIRE(batch.at("additionalProperties") == false);
    REQUIRE(batch.at("properties").at("items").at("maxItems") == 50U);

    const auto search = schema(tools, "project_memory.search");
    REQUIRE(search.at("additionalProperties") == false);
    REQUIRE(search.at("properties").at("kinds").at("maxItems") ==
        Mcp::McpToolCatalog::MaximumProjectMemoryKinds);
    REQUIRE(!search.at("properties").at("tags").contains("maxItems"));

    const auto recent = schema(tools, "project_memory.list_recent");
    REQUIRE(recent.at("properties").at("kinds").at("maxItems") == 100U);
    const auto exported = schema(tools, "project_memory.export");
    REQUIRE(exported.at("required") == Json::array({"project_id"}));
    REQUIRE(exported.at("properties").size() == 2U);
    REQUIRE(exported.at("properties").contains("deadline_ms"));

    const auto checkpoint = schema(tools, "continuity.checkpoint");
    REQUIRE(checkpoint.at("additionalProperties") == false);
    REQUIRE(checkpoint.at("required") ==
        Json::array({"project_id", "predecessor_session_id", "mission"}));
    REQUIRE(checkpoint.at("properties").at("constraints").at("maxItems") == 128U);
    REQUIRE(checkpoint.at("properties").at("next_actions").at("maxItems") == 128U);

    const auto shell = schema(tools, "shell_exec");
    REQUIRE(shell.at("properties").at("timeout_sec").at("exclusiveMinimum") == 0);
    REQUIRE(shell.at("properties").at("timeout_sec").at("maximum") == 120);
}

void testDescriptorValidation()
{
    auto catalog = take(Mcp::McpToolCatalog::create());
    const auto tools = catalog->tools();
    REQUIRE(Mcp::McpToolCatalog::validateDescriptors(tools).hasValue());

    std::vector<Domain::McpToolDescriptor> shortened{tools.begin(), tools.end()};
    shortened.pop_back();
    REQUIRE(!Mcp::McpToolCatalog::validateDescriptors(shortened).hasValue());

    std::vector<Domain::McpToolDescriptor> duplicated{tools.begin(), tools.end()};
    duplicated[1] = duplicated[0];
    REQUIRE(!Mcp::McpToolCatalog::validateDescriptors(duplicated).hasValue());

    std::vector<Domain::McpToolDescriptor> unsorted{tools.begin(), tools.end()};
    std::swap(unsorted[0], unsorted[1]);
    REQUIRE(!Mcp::McpToolCatalog::validateDescriptors(unsorted).hasValue());

    std::vector<Domain::McpToolDescriptor> invalidSchema{tools.begin(), tools.end()};
    invalidSchema[0].inputSchema = "[]";
    REQUIRE(!Mcp::McpToolCatalog::validateDescriptors(invalidSchema).hasValue());
}

void testCanonicalJsonCodec()
{
    const Mcp::McpJsonCodec codec;
    REQUIRE(take(codec.canonicalize(
        R"({"z":1,"a":{"y":2,"b":3},"m":[{"d":4,"c":5}]})")) ==
        R"({"a":{"b":3,"y":2},"m":[{"c":5,"d":4}],"z":1})");
    REQUIRE(take(codec.canonicalize(" { \"values\": [ true, null, 2 ] } ")) ==
        "{\"values\":[true,null,2]}");
    REQUIRE(!codec.canonicalize("[true,null,2]").hasValue());
    REQUIRE(!codec.canonicalize(R"({"a":1,"a":2})").hasValue());
    REQUIRE(!codec.canonicalize(R"({"a":{"b":1,"b":2}})").hasValue());
    REQUIRE(!codec.canonicalize(R"({"value":"\u0000"})").hasValue());
    REQUIRE(!codec.canonicalize("{").hasValue());
    REQUIRE(!codec.canonicalize("").hasValue());
    REQUIRE(!codec.canonicalize(
        std::string(Mcp::McpJsonCodec::MaximumDocumentBytes + 1U, 'x')).hasValue());

    std::string tooDeep;
    for (std::size_t index{};
         index < Mcp::McpJsonCodec::MaximumNestingDepth + 2U;
         ++index) {
        tooDeep.append("{\"a\":");
    }
    tooDeep.append("0");
    tooDeep.append(Mcp::McpJsonCodec::MaximumNestingDepth + 2U, '}');
    REQUIRE(!codec.canonicalize(tooDeep).hasValue());
}

void testProtocolNegotiation()
{
    REQUIRE(Mcp::McpProtocol::SupportedVersions.size() == 4U);
    for (const auto version : Mcp::McpProtocol::SupportedVersions) {
        REQUIRE(Mcp::McpProtocol::negotiate(version) == version);
    }
    REQUIRE(Mcp::McpProtocol::negotiate(" 2024-11-05\r\n") == "2024-11-05");
    REQUIRE(Mcp::McpProtocol::negotiate("") == "2025-11-25");
    REQUIRE(Mcp::McpProtocol::negotiate("2099-01-01") == "2025-11-25");
}

} // namespace

int main()
{
    try {
        testCanonicalCatalog();
        testSourceSchemasAndWindowsDelta();
        testDescriptorValidation();
        testCanonicalJsonCodec();
        testProtocolNegotiation();
        std::cout << "MCP catalog/codec tests passed: " << assertions
                  << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP catalog/codec tests failed after " << assertions
                  << " assertions: " << error.what() << '\n';
        return 1;
    }
}
