#include "ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "Detail/OperationContextGuard.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;

[[nodiscard]] Domain::Error invalidReport(const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InvalidRequest, std::string{message});
}

class WindowsAgentCompletionReportInspector final
    : public Contracts::IAgentCompletionReportInspector {
public:
    explicit WindowsAgentCompletionReportInspector(Contracts::IClock& clock) noexcept
        : clock_{clock}
    {
    }

    WindowsAgentCompletionReportInspector(
        const WindowsAgentCompletionReportInspector&) = delete;
    WindowsAgentCompletionReportInspector& operator=(
        const WindowsAgentCompletionReportInspector&) = delete;
    WindowsAgentCompletionReportInspector(
        WindowsAgentCompletionReportInspector&&) = delete;
    WindowsAgentCompletionReportInspector& operator=(
        WindowsAgentCompletionReportInspector&&) = delete;

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentReportField>> inspect(
        const std::string_view canonicalJson,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto validContext = Detail::validateOperationContext(
                context,
                clock_.monotonicNow(),
                "inspect the agent completion report");
            if (!validContext) {
                return Domain::Result<
                    std::vector<Domain::AgentReportField>>::failure(
                        std::move(validContext).error());
            }
            if (canonicalJson.empty() ||
                canonicalJson.size() >
                    Domain::AgentSessionLimits::MaximumReportJsonBytes ||
                canonicalJson.find('\0') != std::string_view::npos ||
                !Domain::isValidUtf8(canonicalJson)) {
                return Domain::Result<
                    std::vector<Domain::AgentReportField>>::failure(
                        invalidReport(
                            "The agent completion report is empty, oversized, or invalid UTF-8."));
            }

            bool invalidStructure{};
            std::vector<std::set<std::string>> objectKeys;
            const auto callback = [&](const int depth,
                                      const Json::parse_event_t event,
                                      Json& parsed) {
                if (depth < 0 || depth > 64) {
                    invalidStructure = true;
                }
                if (event == Json::parse_event_t::object_start) {
                    objectKeys.emplace_back();
                } else if (event == Json::parse_event_t::key) {
                    if (objectKeys.empty() || !parsed.is_string() ||
                        !objectKeys.back().insert(
                            parsed.get_ref<const std::string&>()).second) {
                        invalidStructure = true;
                    }
                } else if (event == Json::parse_event_t::object_end) {
                    if (objectKeys.empty()) {
                        invalidStructure = true;
                    } else {
                        objectKeys.pop_back();
                    }
                }
                return true;
            };
            auto document = Json::parse(
                canonicalJson, callback, false, false);
            if (invalidStructure || !objectKeys.empty() ||
                document.is_discarded() || !document.is_object() ||
                document.size() >
                    Domain::AgentSessionLimits::MaximumReportFields) {
                return Domain::Result<
                    std::vector<Domain::AgentReportField>>::failure(
                        invalidReport(
                            "The agent completion report is not one bounded JSON object."));
            }
            const auto encoded = document.dump(
                -1, ' ', false, Json::error_handler_t::strict);
            if (encoded != canonicalJson) {
                return Domain::Result<
                    std::vector<Domain::AgentReportField>>::failure(
                        invalidReport(
                            "The agent completion report is not canonical JSON."));
            }

            std::vector<Domain::AgentReportField> fields;
            fields.reserve(document.size());
            for (auto iterator = document.cbegin(); iterator != document.cend();
                 ++iterator) {
                if (iterator.key().empty() ||
                    iterator.key().size() >
                        Domain::AgentSessionLimits::MaximumItemBytes ||
                    iterator.key().find('\0') != std::string::npos ||
                    !Domain::isValidUtf8(iterator.key())) {
                    return Domain::Result<
                        std::vector<Domain::AgentReportField>>::failure(
                            invalidReport(
                                "The agent completion report contains an invalid field name."));
                }
                Domain::AgentReportValueKind kind{
                    Domain::AgentReportValueKind::Null};
                std::size_t logicalSize{};
                if (iterator->is_string()) {
                    kind = Domain::AgentReportValueKind::String;
                    logicalSize =
                        iterator->get_ref<const std::string&>().size();
                } else if (iterator->is_array()) {
                    kind = Domain::AgentReportValueKind::Array;
                    logicalSize = iterator->size();
                } else if (iterator->is_object()) {
                    kind = Domain::AgentReportValueKind::Object;
                    logicalSize = iterator->size();
                } else if (iterator->is_boolean()) {
                    kind = Domain::AgentReportValueKind::Boolean;
                } else if (iterator->is_number()) {
                    kind = Domain::AgentReportValueKind::Number;
                } else if (!iterator->is_null()) {
                    return Domain::Result<
                        std::vector<Domain::AgentReportField>>::failure(
                            invalidReport(
                                "The agent completion report contains an unsupported value."));
                }
                fields.push_back(Domain::AgentReportField{
                    iterator.key(), kind, logicalSize});
            }

            validContext = Detail::validateOperationContext(
                context,
                clock_.monotonicNow(),
                "inspect the agent completion report");
            if (!validContext) {
                return Domain::Result<
                    std::vector<Domain::AgentReportField>>::failure(
                        std::move(validContext).error());
            }
            return Domain::Result<
                std::vector<Domain::AgentReportField>>::success(
                    std::move(fields));
        } catch (...) {
            return Domain::Result<
                std::vector<Domain::AgentReportField>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InternalFailure,
                        "The agent completion report could not be inspected."));
        }
    }

private:
    Contracts::IClock& clock_;
};

} // namespace

std::unique_ptr<Contracts::IAgentCompletionReportInspector>
createWindowsAgentCompletionReportInspector(Contracts::IClock& clock)
{
    return std::make_unique<WindowsAgentCompletionReportInspector>(clock);
}

} // namespace ForgeConductor::Infrastructure::Windows
