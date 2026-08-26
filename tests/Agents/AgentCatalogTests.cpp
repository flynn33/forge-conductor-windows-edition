#include "ForgeConductor/Application/AgentCatalog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;

static_assert(std::is_final_v<Application::AgentCatalog>);
static_assert(!std::is_copy_constructible_v<Application::AgentCatalog>);
static_assert(!std::is_move_constructible_v<Application::AgentCatalog>);

#define REQUIRE(condition)                                                       \
    do {                                                                         \
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

template <typename T>
[[nodiscard]] T parseId(const std::string_view value)
{
    return take(T::parse(value));
}

class FixedClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return now_;
    }

    void advance(const std::chrono::steady_clock::duration by) noexcept
    {
        now_ += by;
    }

private:
    Domain::MonotonicTimePoint now_{};
};

[[nodiscard]] Domain::OperationContext context(
    const FixedClock& clock,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parseId<Domain::OperationId>("10000000-0000-4000-8000-000000000001"),
        clock.monotonicNow() + 1h,
        cancellation,
        parseId<Domain::CorrelationId>("agent-catalog-tests")};
}

[[nodiscard]] std::string readBounded(const std::filesystem::path& path)
{
    const auto bytes = std::filesystem::file_size(path);
    REQUIRE(bytes <= Application::AgentCatalog::MaximumDefinitionBytes);
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.good());
    std::ostringstream stream;
    stream << input.rdbuf();
    REQUIRE(input.good() || input.eof());
    return stream.str();
}

[[nodiscard]] std::vector<Application::AgentDefinitionDocument> resourceDocuments(
    const std::filesystem::path& resourceRoot)
{
    static constexpr std::string_view Names[]{
        "debug.md", "docs.md", "explore.md", "implement.md", "plan.md",
        "precommit-audit.md", "research.md", "review.md", "security.md", "test.md"};
    std::vector<Application::AgentDefinitionDocument> definitions;
    definitions.reserve(std::size(Names));
    for (const auto name : Names) {
        definitions.push_back(Application::AgentDefinitionDocument{
            std::string{name},
            readBounded(resourceRoot / name),
            Application::AgentDefinitionOrigin::BuiltIn});
    }
    return definitions;
}

[[nodiscard]] Application::AgentDefinitionDocument definition(
    std::string id,
    std::string stableName,
    std::string displayName,
    const Application::AgentDefinitionOrigin origin =
        Application::AgentDefinitionOrigin::Custom)
{
    std::string markdown =
        "---\n"
        "id: " + id + "\n"
        "display_name: " + displayName + "\n"
        "description: Bounded custom definition.\n"
        "tools: [fs_read]\n"
        "first_moves: [Read the selected source, Call agent_run_complete]\n"
        "done_definition: [Evidence is recorded, agent_run_complete called]\n"
        "output_schema: [summary]\n"
        "handoff: [explore]\n"
        "quality_bar: [Always call agent_run_complete]\n"
        "---\n"
        "# Custom agent\n\nRead authoritative sources and always call `agent_run_complete`.\n";
    return Application::AgentDefinitionDocument{
        std::move(stableName), std::move(markdown), origin};
}

[[nodiscard]] std::string repeatedList(
    const std::string_view value,
    const std::size_t count)
{
    std::string result{"["};
    for (std::size_t index{}; index < count; ++index) {
        if (index != 0U) {
            result.append(", ");
        }
        result.append(value);
    }
    result.push_back(']');
    return result;
}

[[nodiscard]] bool sameSpec(
    const Domain::AgentSpec& left,
    const Domain::AgentSpec& right)
{
    return left.id == right.id && left.displayName == right.displayName &&
        left.description == right.description && left.tools == right.tools &&
        left.toolsForbidden == right.toolsForbidden &&
        left.whenToUse == right.whenToUse && left.firstMoves == right.firstMoves &&
        left.doneDefinition == right.doneDefinition &&
        left.outputSchema == right.outputSchema && left.handoff == right.handoff &&
        left.qualityBar == right.qualityBar && left.body == right.body &&
        left.source == right.source;
}

[[nodiscard]] Domain::AgentSpec get(
    Application::AgentCatalog& catalog,
    const std::string_view id,
    const Domain::OperationContext& operation)
{
    auto result = take(catalog.get(parseId<Domain::AgentId>(id), operation));
    REQUIRE(result.has_value());
    return std::move(*result);
}

void testEmbeddedFallbacksAndResourcesAreEquivalent(
    const std::filesystem::path& resourceRoot)
{
    auto clock = std::make_shared<FixedClock>();
    const auto operation = context(*clock);
    auto fallbackCatalog = take(Application::AgentCatalog::create(
        clock,
        std::span<const Application::AgentDefinitionDocument>{},
        operation));
    auto resources = resourceDocuments(resourceRoot);
    auto resourceCatalog = take(Application::AgentCatalog::create(
        clock, resources, operation));

    const auto fallbacks = take(fallbackCatalog->all(operation));
    const auto parsedResources = take(resourceCatalog->all(operation));
    REQUIRE(fallbacks.size() == Application::AgentCatalog::MandatoryEntryCount);
    REQUIRE(parsedResources.size() == fallbacks.size());
    REQUIRE(std::is_sorted(
        parsedResources.begin(), parsedResources.end(), [](const auto& left, const auto& right) {
            return left.id.value() < right.id.value();
        }));
    for (std::size_t index{}; index < fallbacks.size(); ++index) {
        REQUIRE(sameSpec(fallbacks[index], parsedResources[index]));
        REQUIRE(!fallbacks[index].description.empty());
        REQUIRE(!fallbacks[index].tools.empty());
        REQUIRE(!fallbacks[index].firstMoves.empty());
        REQUIRE(!fallbacks[index].doneDefinition.empty());
        REQUIRE(!fallbacks[index].outputSchema.empty());
        REQUIRE(fallbacks[index].body.size() > 40U);
        REQUIRE(
            fallbacks[index].body.find("agent_run_complete") != std::string::npos ||
            std::any_of(
                fallbacks[index].doneDefinition.begin(),
                fallbacks[index].doneDefinition.end(),
                [](const auto& value) {
                    return value.find("agent_run_complete") != std::string::npos;
                }));
    }
    const auto docs = get(*resourceCatalog, "docs", operation);
    REQUIRE(std::find(docs.tools.begin(), docs.tools.end(), "pdf_write") != docs.tools.end());
    REQUIRE(std::find(docs.tools.begin(), docs.tools.end(), "pdf_from_file") != docs.tools.end());
    const auto implement = get(*resourceCatalog, "implement", operation);
    REQUIRE(std::find(
        implement.tools.begin(), implement.tools.end(), "session_checkpoint") !=
        implement.tools.end());
    REQUIRE(std::find(implement.tools.begin(), implement.tools.end(), "memory_set") !=
        implement.tools.end());
}

void testSourceCompatibleParserAndValidation()
{
    FixedClock clock;
    const auto operation = context(clock);
    const Application::AgentDefinitionDocument source{
        "custom.md",
        "---\r\n"
        "# comment\r\n"
        "id: custom-agent\r\n"
        "display_name: \"Custom\"\r\n"
        "description: >\r\n"
        "  First line.\r\n"
        "  Second line.\r\n"
        "tools: [fs_read, fs_list]\r\n"
        "tools_forbidden:\r\n"
        "  - git_push\r\n"
        "output_schema:\r\n"
        "  - summary\r\n"
        "  - next\r\n"
        "---\r\n"
        "Body before---body after.\r\n",
        Application::AgentDefinitionOrigin::Custom};
    const auto parsed = take(Application::AgentCatalog::parseDefinition(
        source, clock, operation));
    REQUIRE(parsed.id.value() == "custom-agent");
    REQUIRE(parsed.displayName == "Custom");
    REQUIRE(parsed.description == "First line. Second line.");
    REQUIRE(parsed.tools == std::vector<std::string>({"fs_read", "fs_list"}));
    REQUIRE(parsed.toolsForbidden == std::vector<std::string>({"git_push"}));
    REQUIRE(parsed.outputSchema == std::vector<std::string>({"summary", "next"}));
    REQUIRE(parsed.body == "Body before---body after.");
    REQUIRE(parsed.source == "custom");

    auto idOnly = Application::AgentCatalog::parseDefinition(
        Application::AgentDefinitionDocument{
            "thin.md", "---\nid: thin\n---\nThin custom body.",
            Application::AgentDefinitionOrigin::Custom},
        clock,
        operation);
    REQUIRE(idOnly);
    REQUIRE(idOnly.value().displayName == "thin");
    REQUIRE(idOnly.value().tools.empty());

    const std::vector<Application::AgentDefinitionDocument> malformed{
        {"missing-open.md", "id: bad\n---\nbody", Application::AgentDefinitionOrigin::Custom},
        {"missing-close.md", "---\nid: bad\nbody", Application::AgentDefinitionOrigin::Custom},
        {"opening-prefix.md", "----\nid: bad\n---\nbody", Application::AgentDefinitionOrigin::Custom},
        {"closing-suffix.md", "---\nid: bad\n---extra\nbody", Application::AgentDefinitionOrigin::Custom},
        {"embedded-fence.md", "---\nid: bad\ndescription: alpha---omega\nbody", Application::AgentDefinitionOrigin::Custom},
        {"missing-id.md", "---\ntools: [fs_read]\n---\nbody", Application::AgentDefinitionOrigin::Custom},
        {"unknown-tool.md", "---\nid: bad\ntools: [unknown_tool]\n---\nbody", Application::AgentDefinitionOrigin::Custom}};
    for (const auto& candidate : malformed) {
        const auto result = Application::AgentCatalog::parseDefinition(
            candidate, clock, operation);
        REQUIRE(!result);
        REQUIRE(result.error().code == Domain::ErrorCodes::InvalidRequest);
    }

    auto embeddedNull = definition("bad-null", "bad-null.md", "Bad");
    embeddedNull.markdown.insert(embeddedNull.markdown.begin() + 4, '\0');
    const auto nullResult = Application::AgentCatalog::parseDefinition(
        embeddedNull, clock, operation);
    REQUIRE(!nullResult);
    REQUIRE(nullResult.error().code == Domain::ErrorCodes::InvalidRequest);

    auto invalidUtf8 = definition("bad-utf8", "bad-utf8.md", "Bad");
    invalidUtf8.markdown.append("\xC3\x28", 2U);
    const auto utf8Result = Application::AgentCatalog::parseDefinition(
        invalidUtf8, clock, operation);
    REQUIRE(!utf8Result);
    REQUIRE(utf8Result.error().code == Domain::ErrorCodes::InvalidRequest);

    Application::AgentDefinitionDocument oversized{
        "oversized.md",
        std::string(Application::AgentCatalog::MaximumDefinitionBytes + 1U, 'x'),
        Application::AgentDefinitionOrigin::Custom};
    const auto oversizedResult = Application::AgentCatalog::parseDefinition(
        oversized, clock, operation);
    REQUIRE(!oversizedResult);
    REQUIRE(oversizedResult.error().code == Domain::ErrorCodes::PayloadTooLarge);
}

void testParserStructuralBounds()
{
    FixedClock clock;
    const auto operation = context(clock);

    auto oversizedStableName = definition("stable", "stable.md", "Stable");
    oversizedStableName.stableName.assign(
        Application::AgentCatalog::MaximumStableNameBytes + 1U, 's');
    const auto stableNameResult = Application::AgentCatalog::parseDefinition(
        oversizedStableName, clock, operation);
    REQUIRE(!stableNameResult);
    REQUIRE(stableNameResult.error().code == Domain::ErrorCodes::PayloadTooLarge);

    const std::string maximumTools = repeatedList(
        "fs_read", Application::AgentCatalog::MaximumListItems);
    const Application::AgentDefinitionDocument maximumList{
        "maximum-list.md",
        "---\nid: maximum-list\ntools: " + maximumTools + "\n---\nBody.",
        Application::AgentDefinitionOrigin::Custom};
    REQUIRE(Application::AgentCatalog::parseDefinition(
        maximumList, clock, operation));

    const Application::AgentDefinitionDocument excessiveList{
        "excessive-list.md",
        "---\nid: excessive-list\ntools: " +
            repeatedList("fs_read", Application::AgentCatalog::MaximumListItems + 1U) +
            "\n---\nBody.",
        Application::AgentDefinitionOrigin::Custom};
    const auto excessiveListResult = Application::AgentCatalog::parseDefinition(
        excessiveList, clock, operation);
    REQUIRE(!excessiveListResult);
    REQUIRE(excessiveListResult.error().code == Domain::ErrorCodes::LimitExceeded);

    const auto aggregateMarkdown = [](const std::size_t firstMoves) {
        return "---\nid: aggregate-items\ntools: " + repeatedList("fs_read", 64U) +
            "\ntools_forbidden: " + repeatedList("git_push", 64U) +
            "\nwhen_to_use: " + repeatedList("bounded", 64U) +
            "\nfirst_moves: " + repeatedList("inspect", firstMoves) +
            "\n---\nBody.";
    };
    const Application::AgentDefinitionDocument maximumItems{
        "maximum-items.md", aggregateMarkdown(63U),
        Application::AgentDefinitionOrigin::Custom};
    REQUIRE(Application::AgentCatalog::parseDefinition(
        maximumItems, clock, operation));
    const Application::AgentDefinitionDocument excessiveItems{
        "excessive-items.md", aggregateMarkdown(64U),
        Application::AgentDefinitionOrigin::Custom};
    const auto excessiveItemsResult = Application::AgentCatalog::parseDefinition(
        excessiveItems, clock, operation);
    REQUIRE(!excessiveItemsResult);
    REQUIRE(excessiveItemsResult.error().code == Domain::ErrorCodes::LimitExceeded);

    const Application::AgentDefinitionDocument oversizedField{
        "oversized-field.md",
        "---\nid: oversized-field\ndescription: " +
            std::string(Application::AgentCatalog::MaximumFieldBytes + 1U, 'd') +
            "\n---\nBody.",
        Application::AgentDefinitionOrigin::Custom};
    const auto oversizedFieldResult = Application::AgentCatalog::parseDefinition(
        oversizedField, clock, operation);
    REQUIRE(!oversizedFieldResult);
    REQUIRE(oversizedFieldResult.error().code == Domain::ErrorCodes::PayloadTooLarge);

    const std::string longId(128U, 'i');
    std::string oversizedNormalizedText = "---\nid: " + longId + "\n---\n";
    oversizedNormalizedText.append(
        Application::AgentCatalog::MaximumDefinitionBytes -
            oversizedNormalizedText.size(),
        'b');
    const Application::AgentDefinitionDocument oversizedSpec{
        "oversized-spec.md", std::move(oversizedNormalizedText),
        Application::AgentDefinitionOrigin::Custom};
    const auto oversizedSpecResult = Application::AgentCatalog::parseDefinition(
        oversizedSpec, clock, operation);
    REQUIRE(!oversizedSpecResult);
    REQUIRE(oversizedSpecResult.error().code == Domain::ErrorCodes::PayloadTooLarge);
}

void testDeterministicOverridesAndMalformedSkip()
{
    auto clock = std::make_shared<FixedClock>();
    const auto operation = context(*clock);
    auto catalog = take(Application::AgentCatalog::create(
        clock,
        std::span<const Application::AgentDefinitionDocument>{},
        operation));

    std::vector<Application::AgentDefinitionDocument> documents;
    documents.push_back(definition(
        "custom-agent", "z.md", "Z definition"));
    documents.push_back(definition(
        "custom-agent", "a.md", "A definition"));
    documents.push_back(definition(
        "security", "z-builtin.md", "Built-in replacement",
        Application::AgentDefinitionOrigin::BuiltIn));
    documents.push_back(definition(
        "security", "a-custom.md", "Custom replacement"));
    documents.push_back(Application::AgentDefinitionDocument{
        "malformed.md", "not frontmatter", Application::AgentDefinitionOrigin::Custom});

    const auto report = take(catalog->reload(documents, operation));
    REQUIRE(report.acceptedDocuments == 4U);
    REQUIRE(report.rejectedDocuments == 1U);
    REQUIRE(get(*catalog, "custom-agent", operation).displayName == "Z definition");
    REQUIRE(get(*catalog, "security", operation).displayName == "Custom replacement");

    std::vector<Application::AgentDefinitionDocument> malformedOnly{
        {"explore.md", "---\ntools: [fs_read]\n---\ninvalid override",
         Application::AgentDefinitionOrigin::Custom}};
    const auto malformedReport = take(catalog->reload(malformedOnly, operation));
    REQUIRE(malformedReport.acceptedDocuments == 0U);
    REQUIRE(malformedReport.rejectedDocuments == 1U);
    REQUIRE(get(*catalog, "explore", operation).displayName == "Explore");
}

void testCapacityRetainsMandatoryDefinitions()
{
    auto clock = std::make_shared<FixedClock>();
    const auto operation = context(*clock);
    auto catalog = take(Application::AgentCatalog::create(
        clock,
        std::span<const Application::AgentDefinitionDocument>{},
        operation));

    std::vector<Application::AgentDefinitionDocument> documents;
    documents.reserve(300U);
    for (std::size_t index{}; index < 300U; ++index) {
        std::ostringstream id;
        id << "custom-";
        id.width(3);
        id.fill('0');
        id << index;
        documents.push_back(definition(id.str(), id.str() + ".md", id.str()));
    }
    const auto report = take(catalog->reload(documents, operation));
    REQUIRE(report.acceptedDocuments == documents.size());
    REQUIRE(report.retainedDefinitions == Application::AgentCatalog::MaximumEntries);
    REQUIRE(report.truncatedDefinitions == 54U);
    const auto all = take(catalog->all(operation));
    REQUIRE(all.size() == Application::AgentCatalog::MaximumEntries);
    for (const auto mandatory : {
             "debug", "docs", "explore", "implement", "plan", "precommit-audit",
             "research", "review", "security", "test"}) {
        REQUIRE(std::any_of(all.begin(), all.end(), [&](const auto& specification) {
            return specification.id.value() == mandatory;
        }));
    }
    REQUIRE(get(*catalog, "custom-245", operation).id.value() == "custom-245");
    const auto omitted = take(catalog->get(
        parseId<Domain::AgentId>("custom-246"), operation));
    REQUIRE(!omitted.has_value());

    const auto prior = take(catalog->all(operation));
    std::vector<Application::AgentDefinitionDocument> excessive(
        Application::AgentCatalog::MaximumDefinitionDocuments + 1U,
        definition("same", "same.md", "Same"));
    const auto excessiveResult = catalog->reload(excessive, operation);
    REQUIRE(!excessiveResult);
    REQUIRE(excessiveResult.error().code == Domain::ErrorCodes::LimitExceeded);
    const auto afterFailure = take(catalog->all(operation));
    REQUIRE(afterFailure.size() == prior.size());
    REQUIRE(afterFailure.front().id == prior.front().id);

    std::vector<Application::AgentDefinitionDocument> excessiveAggregate;
    excessiveAggregate.reserve(256U);
    for (std::size_t index{}; index < 256U; ++index) {
        excessiveAggregate.push_back(Application::AgentDefinitionDocument{
            "aggregate-" + std::to_string(index) + ".md",
            std::string(Application::AgentCatalog::MaximumDefinitionBytes, 'x'),
            Application::AgentDefinitionOrigin::Custom});
    }
    const auto aggregateResult = catalog->reload(excessiveAggregate, operation);
    REQUIRE(!aggregateResult);
    REQUIRE(aggregateResult.error().code == Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(take(catalog->all(operation)).size() == prior.size());
}

void testRecommendationOrderAndBounds()
{
    auto clock = std::make_shared<FixedClock>();
    const auto operation = context(*clock);
    auto catalog = take(Application::AgentCatalog::create(
        clock,
        std::span<const Application::AgentDefinitionDocument>{},
        operation));
    const std::vector<std::pair<std::string, std::string>> fixtures{
        {"commit security", "precommit-audit"},
        {"AUTH crash", "security"},
        {"failing tests", "debug"},
        {"ctest coverage", "test"},
        {"test documentation", "test"},
        {"PDF research", "docs"},
        {"web search review", "research"},
        {"critique architecture", "review"},
        {"architecture codebase", "plan"},
        {"unfamiliar feature", "explore"},
        {"write code", "implement"},
        {"contest behavior", "test"},
        {"", "explore"}};
    for (const auto& [task, expected] : fixtures) {
        const auto result = take(catalog->recommend(task, operation));
        REQUIRE(result.id.value() == expected);
    }

    std::vector<Application::AgentDefinitionDocument> custom{
        definition("security", "security-custom.md", "Custom Security")};
    const auto customReload = take(catalog->reload(custom, operation));
    REQUIRE(customReload.acceptedDocuments == 1U);
    REQUIRE(take(catalog->recommend("auth review", operation)).displayName ==
        "Custom Security");

    const std::string atLimit(
        Application::AgentCatalog::MaximumRecommendationTaskBytes, 'x');
    REQUIRE(take(catalog->recommend(atLimit, operation)).id.value() == "explore");
    const std::string overLimit(
        Application::AgentCatalog::MaximumRecommendationTaskBytes + 1U, 'x');
    const auto over = catalog->recommend(overLimit, operation);
    REQUIRE(!over);
    REQUIRE(over.error().code == Domain::ErrorCodes::LimitExceeded);

    const std::string embeddedNull{"test\0request", 12U};
    const auto nullResult = catalog->recommend(embeddedNull, operation);
    REQUIRE(!nullResult);
    REQUIRE(nullResult.error().code == Domain::ErrorCodes::InvalidRequest);
    const std::string invalidUtf8{"\xF0\x28\x8C\x28", 4U};
    const auto utf8Result = catalog->recommend(invalidUtf8, operation);
    REQUIRE(!utf8Result);
    REQUIRE(utf8Result.error().code == Domain::ErrorCodes::InvalidRequest);
}

void testCancellationDeadlineAndAtomicSnapshots()
{
    auto clock = std::make_shared<FixedClock>();
    const auto operation = context(*clock);
    auto catalog = take(Application::AgentCatalog::create(
        clock,
        std::span<const Application::AgentDefinitionDocument>{},
        operation));

    std::stop_source cancellation;
    cancellation.request_stop();
    const auto cancelledContext = context(*clock, cancellation.get_token());
    const auto cancelled = catalog->reload(
        std::span<const Application::AgentDefinitionDocument>{},
        cancelledContext);
    REQUIRE(!cancelled);
    REQUIRE(cancelled.error().code == Domain::ErrorCodes::Cancelled);

    auto expiredContext = context(*clock);
    clock->advance(2h);
    const auto expired = catalog->all(expiredContext);
    REQUIRE(!expired);
    REQUIRE(expired.error().code == Domain::ErrorCodes::DeadlineExceeded);

    const auto active = context(*clock);
    std::atomic<std::size_t> failures{};
    std::vector<std::thread> readers;
    for (std::size_t index{}; index < 4U; ++index) {
        readers.emplace_back([&]() {
            for (std::size_t iteration{}; iteration < 100U; ++iteration) {
                const auto result = catalog->all(active);
                if (!result || (result.value().size() != 10U && result.value().size() != 11U)) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::size_t iteration{}; iteration < 20U; ++iteration) {
        const std::vector<Application::AgentDefinitionDocument> documents{
            definition(
                "concurrent-agent", "concurrent.md",
                iteration % 2U == 0U ? "Concurrent A" : "Concurrent B")};
        const auto result = catalog->reload(documents, active);
        if (!result) {
            failures.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    for (auto& reader : readers) {
        reader.join();
    }
    REQUIRE(failures.load(std::memory_order_relaxed) == 0U);
}

} // namespace

int main(const int argumentCount, const char* const arguments[])
{
    try {
        if (argumentCount != 2) {
            std::cerr << "Usage: AgentCatalogTests <Agents-resource-directory>\n";
            return 2;
        }
        const std::filesystem::path resourceRoot{arguments[1]};
        REQUIRE(std::filesystem::is_directory(resourceRoot));
        testEmbeddedFallbacksAndResourcesAreEquivalent(resourceRoot);
        testSourceCompatibleParserAndValidation();
        testParserStructuralBounds();
        testDeterministicOverridesAndMalformedSkip();
        testCapacityRetainsMandatoryDefinitions();
        testRecommendationOrderAndBounds();
        testCancellationDeadlineAndAtomicSnapshots();
        std::cout << "Agent catalog tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Agent catalog tests failed: " << error.what() << '\n';
        return 1;
    }
}
