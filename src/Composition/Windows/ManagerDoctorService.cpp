#include <Windows.h>

#include "ManagerDoctorService.h"

#include "ForgeConductor/Application/AgentCatalog.h"
#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ILMStudioDeploymentService.h"
#include "ForgeConductor/Contracts/ITelemetryService.h"
#include "ForgeConductor/Domain/DiagnosticsModels.h"
#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/TelemetryModels.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Composition::Windows {
namespace {

constexpr std::size_t MaximumProductVersionBytes = 128U;
constexpr std::size_t MaximumDoctorDetailBytes =
    Domain::MaximumDiagnosticFlattenedFieldBytes;
constexpr std::size_t MaximumLegacyLauncherCount = 4U;
constexpr std::size_t MaximumWindowsPathCharacters = 32U * 1024U;

constexpr auto LegacyLauncherNames =
    std::to_array<std::pair<std::wstring_view, std::string_view>>({
        {L"forge-serve", "forge-serve"},
        {L"forge-serve-fallback", "forge-serve-fallback"},
        {L"forge-serve.cmd", "forge-serve.cmd"},
        {L"forge-serve-fallback.cmd", "forge-serve-fallback.cmd"}});

static_assert(
    ManagerDoctorService::RequiredAgentCount ==
    Application::AgentCatalog::MandatoryEntryCount);
static_assert(LegacyLauncherNames.size() == MaximumLegacyLauncherCount);

[[nodiscard]] Domain::Error cancelledError()
{
    return Domain::makeError(
        Domain::ErrorCodes::Cancelled,
        "The Manager Doctor operation was cancelled.");
}

[[nodiscard]] Domain::Error deadlineError()
{
    return Domain::makeError(
        Domain::ErrorCodes::DeadlineExceeded,
        "The Manager Doctor operation deadline expired.",
        true);
}

[[nodiscard]] Domain::Error closedError()
{
    return Domain::makeError(
        Domain::ErrorCodes::Cancelled,
        "The Manager Doctor service is shut down.");
}

[[nodiscard]] Domain::Error internalError(std::string message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        std::move(message));
}

[[nodiscard]] Domain::Error integrityError(std::string message)
{
    return Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure,
        std::move(message));
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Contracts::IClock& clock,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(cancelledError());
        }
        if (context.isExpired(clock.monotonicNow())) {
            return Domain::Result<void>::failure(deadlineError());
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalError(
            "The Manager Doctor operation context could not be validated."));
    }
}

[[nodiscard]] bool containsAccess(
    const std::vector<Domain::FileAccess>& values,
    const Domain::FileAccess expected) noexcept
{
    return std::find(values.begin(), values.end(), expected) != values.end();
}

[[nodiscard]] bool isSafeVersion(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > MaximumProductVersionBytes ||
        value.find('\0') != std::string_view::npos ||
        !Domain::isValidUtf8(value)) {
        return false;
    }
    return std::none_of(
        value.begin(), value.end(), [](const unsigned char character) noexcept {
            return character < 0x20U || character == 0x7FU;
        });
}

[[nodiscard]] bool isSafePath(const Domain::PathText& value) noexcept
{
    return Domain::isValidUtf8(value.value()) &&
        value.value().find('\0') == std::string::npos;
}

[[nodiscard]] bool isSafeTelemetryText(const std::string_view value) noexcept
{
    return value.size() <= MaximumDoctorDetailBytes &&
        value.find('\0') == std::string_view::npos &&
        Domain::isValidUtf8(value);
}

[[nodiscard]] bool isSafeTelemetryReport(
    const Domain::TelemetryHealthReport& report) noexcept
{
    return isSafeTelemetryText(report.service) &&
        isSafeTelemetryText(report.runtime) &&
        isSafeTelemetryText(report.mode) &&
        isSafeTelemetryText(report.collectors) &&
        isSafeTelemetryText(report.ui);
}

[[nodiscard]] bool isTerminalDependencyError(
    const Domain::Error& error,
    const bool authoritySensitive = false) noexcept
{
    return error.code == Domain::ErrorCodes::Cancelled ||
        error.code == Domain::ErrorCodes::DeadlineExceeded ||
        error.code == Domain::ErrorCodes::TransportClosed ||
        (authoritySensitive &&
         (error.code == Domain::ErrorCodes::Unauthorized ||
          error.code == Domain::ErrorCodes::PathOutsideAuthority ||
          error.code == Domain::ErrorCodes::ProjectScopeMismatch));
}

[[nodiscard]] Domain::TelemetryHealthReport unavailableHealth()
{
    return Domain::TelemetryHealthReport{
        false,
        "forge-telemetry",
        "windows-native",
        false,
        "unavailable",
        "Windows native telemetry collectors unavailable",
        "WinUI 3 + SSE",
        false};
}

[[nodiscard]] Domain::Result<std::wstring> strictUtf8ToWide(
    const std::string_view value) noexcept
{
    try {
        if (value.empty() || value.find('\0') != std::string_view::npos ||
            value.size() > static_cast<std::size_t>(
                (std::numeric_limits<int>::max)())) {
            return Domain::Result<std::wstring>::failure(integrityError(
                "A Manager Doctor Windows path is invalid."));
        }
        const auto length = static_cast<int>(value.size());
        const int required = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            length,
            nullptr,
            0);
        if (required <= 0 ||
            static_cast<std::size_t>(required) >
                MaximumWindowsPathCharacters) {
            return Domain::Result<std::wstring>::failure(integrityError(
                "A Manager Doctor Windows path could not be decoded."));
        }
        std::wstring converted(static_cast<std::size_t>(required), L'\0');
        if (::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                length,
                converted.data(),
                required) != required) {
            return Domain::Result<std::wstring>::failure(integrityError(
                "A Manager Doctor Windows path could not be decoded."));
        }
        return Domain::Result<std::wstring>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(internalError(
            "A Manager Doctor Windows path conversion failed safely."));
    }
}

[[nodiscard]] Domain::Result<std::string> strictWideToUtf8(
    const std::wstring_view value) noexcept
{
    try {
        if (value.empty() ||
            value.size() > static_cast<std::size_t>(
                (std::numeric_limits<int>::max)())) {
            return Domain::Result<std::string>::failure(integrityError(
                "A discovered Manager Doctor path is invalid."));
        }
        const auto length = static_cast<int>(value.size());
        const int required = ::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            length,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required <= 0 ||
            static_cast<std::size_t>(required) > Domain::PathText::MaximumBytes) {
            return Domain::Result<std::string>::failure(integrityError(
                "A discovered Manager Doctor path could not be encoded."));
        }
        std::string converted(static_cast<std::size_t>(required), '\0');
        if (::WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.data(),
                length,
                converted.data(),
                required,
                nullptr,
                nullptr) != required) {
            return Domain::Result<std::string>::failure(integrityError(
                "A discovered Manager Doctor path could not be encoded."));
        }
        return Domain::Result<std::string>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::string>::failure(internalError(
            "A discovered Manager Doctor path conversion failed safely."));
    }
}

[[nodiscard]] bool isAbsoluteWindowsPath(
    const Domain::PathText& value) noexcept
{
    auto converted = strictUtf8ToWide(value.value());
    if (!converted) {
        return false;
    }
    const auto& path = converted.value();
    const auto separator = [](const wchar_t character) noexcept {
        return character == L'\\' || character == L'/';
    };
    if (path.size() >= 3U &&
        ((path[0] >= L'A' && path[0] <= L'Z') ||
         (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && separator(path[2])) {
        return true;
    }
    if (path.size() < 5U || !separator(path[0]) ||
        !separator(path[1]) || separator(path[2])) {
        return false;
    }
    const auto serverEnd = path.find_first_of(L"\\/", 2U);
    return serverEnd != std::wstring::npos && serverEnd > 2U &&
        serverEnd + 1U < path.size() && !separator(path[serverEnd + 1U]);
}

[[nodiscard]] bool isMissingPathError(const DWORD error) noexcept
{
    return error == ERROR_FILE_NOT_FOUND ||
        error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_INVALID_NAME;
}

[[nodiscard]] Domain::Result<std::optional<DWORD>> attributesFor(
    const std::wstring& path) noexcept
{
    ::SetLastError(ERROR_SUCCESS);
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return Domain::Result<std::optional<DWORD>>::success(attributes);
    }
    const DWORD error = ::GetLastError();
    if (isMissingPathError(error)) {
        return Domain::Result<std::optional<DWORD>>::success(std::nullopt);
    }
    return Domain::Result<std::optional<DWORD>>::failure(internalError(
        "Windows could not inspect a Manager Doctor path."));
}

[[nodiscard]] bool isDirectory(const std::optional<DWORD>& attributes) noexcept
{
    return attributes && ((*attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) &&
        ((*attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U);
}

[[nodiscard]] bool isRegularFile(
    const std::optional<DWORD>& attributes) noexcept
{
    return attributes && ((*attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) &&
        ((*attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U);
}

[[nodiscard]] constexpr bool isLegacyLauncherEntry(
    const std::optional<DWORD>& attributes) noexcept
{
    return attributes && ((*attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U);
}

static_assert(isLegacyLauncherEntry(
    std::optional<DWORD>{FILE_ATTRIBUTE_REPARSE_POINT}));
static_assert(!isLegacyLauncherEntry(
    std::optional<DWORD>{
        FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT}));

[[nodiscard]] std::wstring childPath(
    const std::wstring& root,
    const std::wstring_view child)
{
    std::wstring value{root};
    if (!value.empty() && value.back() != L'\\' && value.back() != L'/') {
        value.push_back(L'\\');
    }
    value.append(child);
    return value;
}

[[nodiscard]] Domain::Result<std::wstring> parentPath(
    const std::wstring& path)
{
    const auto separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos || separator == 0U) {
        return Domain::Result<std::wstring>::failure(integrityError(
            "The installed Manager binary has no usable parent directory."));
    }
    return Domain::Result<std::wstring>::success(path.substr(0U, separator));
}

[[nodiscard]] Domain::Result<std::optional<Domain::PathText>> discoverGit()
{
    ::SetLastError(ERROR_SUCCESS);
    const DWORD required =
        ::SearchPathW(nullptr, L"git.exe", nullptr, 0U, nullptr, nullptr);
    if (required == 0U) {
        return Domain::Result<std::optional<Domain::PathText>>::success(
            std::nullopt);
    }
    if (required > MaximumWindowsPathCharacters) {
        return Domain::Result<std::optional<Domain::PathText>>::failure(
            integrityError("The discovered Git path exceeds its bound."));
    }
    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    const DWORD written = ::SearchPathW(
        nullptr,
        L"git.exe",
        nullptr,
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    if (written == 0U || written >= buffer.size()) {
        return Domain::Result<std::optional<Domain::PathText>>::failure(
            internalError("The discovered Git path could not be read."));
    }
    buffer.resize(static_cast<std::size_t>(written));
    auto attributes = attributesFor(buffer);
    if (!attributes) {
        return Domain::Result<std::optional<Domain::PathText>>::failure(
            std::move(attributes).error());
    }
    DWORD binaryType{};
    if (!isRegularFile(attributes.value()) ||
        ::GetBinaryTypeW(buffer.c_str(), &binaryType) == FALSE ||
        (binaryType != SCS_32BIT_BINARY &&
         binaryType != SCS_64BIT_BINARY)) {
        return Domain::Result<std::optional<Domain::PathText>>::success(
            std::nullopt);
    }
    auto encoded = strictWideToUtf8(buffer);
    if (!encoded) {
        return Domain::Result<std::optional<Domain::PathText>>::failure(
            std::move(encoded).error());
    }
    auto path = Domain::PathText::create(std::move(encoded).value());
    if (!path) {
        return Domain::Result<std::optional<Domain::PathText>>::failure(
            std::move(path).error());
    }
    return Domain::Result<std::optional<Domain::PathText>>::success(
        std::move(path).value());
}

} // namespace

WindowsManagerDoctorPlatformProbe::WindowsManagerDoctorPlatformProbe(
    const Contracts::IClock& clock) noexcept
    : clock_{clock}
{
}

Domain::Result<ManagerDoctorPlatformSnapshot>
WindowsManagerDoctorPlatformProbe::inspect(
    const Domain::PathText& dataRoot,
    const Domain::PathText& centralStorePath,
    const Domain::PathText& installedBinaryPath,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = validateContext(clock_, context);
        if (!active) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(active).error());
        }
        auto dataRootWide = strictUtf8ToWide(dataRoot.value());
        auto centralStoreWide = strictUtf8ToWide(centralStorePath.value());
        auto installedBinaryWide =
            strictUtf8ToWide(installedBinaryPath.value());
        if (!dataRootWide) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(dataRootWide).error());
        }
        if (!centralStoreWide) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(centralStoreWide).error());
        }
        if (!installedBinaryWide) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(installedBinaryWide).error());
        }
        auto installedBinaryDirectory = parentPath(installedBinaryWide.value());
        if (!installedBinaryDirectory) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(installedBinaryDirectory).error());
        }

        auto homeAttributes = attributesFor(dataRootWide.value());
        if (!homeAttributes) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(homeAttributes).error());
        }
        active = validateContext(clock_, context);
        if (!active) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(active).error());
        }
        auto storeAttributes = attributesFor(centralStoreWide.value());
        if (!storeAttributes) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(storeAttributes).error());
        }
        active = validateContext(clock_, context);
        if (!active) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(active).error());
        }
        auto git = discoverGit();
        if (!git) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(git).error());
        }
        active = validateContext(clock_, context);
        if (!active) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(active).error());
        }

        auto binaryAttributes = attributesFor(installedBinaryWide.value());
        if (!binaryAttributes) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(binaryAttributes).error());
        }
        bool binaryExecutable{};
        if (isRegularFile(binaryAttributes.value())) {
            DWORD binaryType{};
            binaryExecutable = ::GetBinaryTypeW(
                installedBinaryWide.value().c_str(), &binaryType) != FALSE &&
                binaryType == SCS_64BIT_BINARY;
        }

        std::vector<std::string> legacyLaunchers;
        legacyLaunchers.reserve(MaximumLegacyLauncherCount);
        for (const auto& [wideName, utf8Name] : LegacyLauncherNames) {
            active = validateContext(clock_, context);
            if (!active) {
                return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                    std::move(active).error());
            }
            auto attributes = attributesFor(
                childPath(installedBinaryDirectory.value(), wideName));
            if (!attributes) {
                return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                    std::move(attributes).error());
            }
            if (isLegacyLauncherEntry(attributes.value())) {
                legacyLaunchers.emplace_back(utf8Name);
            }
        }
        active = validateContext(clock_, context);
        if (!active) {
            return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
                std::move(active).error());
        }
        return Domain::Result<ManagerDoctorPlatformSnapshot>::success(
            ManagerDoctorPlatformSnapshot{
                isDirectory(homeAttributes.value()),
                isRegularFile(storeAttributes.value()),
                std::move(git).value(),
                binaryExecutable,
                std::move(legacyLaunchers)});
    } catch (...) {
        return Domain::Result<ManagerDoctorPlatformSnapshot>::failure(
            internalError(
                "The Windows Manager Doctor platform probe failed safely."));
    }
}

namespace {

[[nodiscard]] Domain::Result<void> validatePlatformSnapshot(
    const ManagerDoctorPlatformSnapshot& snapshot) noexcept
{
    try {
        if (snapshot.gitExecutablePath &&
            !isSafePath(*snapshot.gitExecutablePath)) {
            return Domain::Result<void>::failure(integrityError(
                "The Manager Doctor platform probe returned an invalid Git "
                "path."));
        }
        if (snapshot.legacyLauncherBasenames.size() >
            MaximumLegacyLauncherCount) {
            return Domain::Result<void>::failure(integrityError(
                "The Manager Doctor platform probe exceeded the legacy "
                "launcher bound."));
        }
        std::array<bool, MaximumLegacyLauncherCount> seen{};
        for (const auto& basename : snapshot.legacyLauncherBasenames) {
            const auto found = std::find_if(
                LegacyLauncherNames.begin(),
                LegacyLauncherNames.end(),
                [&basename](const auto& expected) noexcept {
                    return expected.second == basename;
                });
            if (found == LegacyLauncherNames.end()) {
                return Domain::Result<void>::failure(integrityError(
                    "The Manager Doctor platform probe returned an unknown "
                    "legacy launcher."));
            }
            const auto index = static_cast<std::size_t>(
                std::distance(LegacyLauncherNames.begin(), found));
            if (seen[index]) {
                return Domain::Result<void>::failure(integrityError(
                    "The Manager Doctor platform probe returned a duplicate "
                    "legacy launcher."));
            }
            seen[index] = true;
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalError(
            "The Manager Doctor platform evidence could not be bounded."));
    }
}

[[nodiscard]] bool supportedTelemetryRuntime(
    const std::string_view runtime) noexcept
{
    return runtime == "windows-native" ||
        runtime == "windows-native-realtime";
}

[[nodiscard]] Domain::Result<bool> hasValidMandatoryAgentIdentities(
    const std::vector<Domain::AgentSpec>& agents) noexcept
{
    try {
        std::array<bool, Application::AgentCatalog::MandatoryEntryCount> seen{};
        for (std::size_t index{}; index < agents.size(); ++index) {
            const auto id = agents[index].id.value();
            for (std::size_t prior{}; prior < index; ++prior) {
                if (agents[prior].id.value() == id) {
                    return Domain::Result<bool>::success(false);
                }
            }
            const auto mandatory = std::find(
                Application::AgentCatalog::MandatoryIds.begin(),
                Application::AgentCatalog::MandatoryIds.end(),
                id);
            if (mandatory != Application::AgentCatalog::MandatoryIds.end()) {
                seen[static_cast<std::size_t>(std::distance(
                    Application::AgentCatalog::MandatoryIds.begin(),
                    mandatory))] = true;
            }
        }
        return Domain::Result<bool>::success(
            std::all_of(seen.begin(), seen.end(), [](const bool value) noexcept {
                return value;
            }));
    } catch (...) {
        return Domain::Result<bool>::failure(internalError(
            "The Manager Doctor could not validate agent identities."));
    }
}

[[nodiscard]] Domain::DoctorCheck doctorCheck(
    std::string name,
    const bool ok,
    std::string detail,
    const bool hard = true)
{
    if (detail.size() > MaximumDoctorDetailBytes) {
        detail = "Doctor detail exceeded its bound.";
    }
    return Domain::DoctorCheck{
        std::move(name), ok, std::move(detail), hard};
}

} // namespace

class ManagerDoctorService::Impl final {
public:
    class Admission final {
    public:
        explicit Admission(Impl& owner) noexcept : owner_{&owner} {}

        Admission(const Admission&) = delete;
        Admission& operator=(const Admission&) = delete;

        Admission(Admission&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)}
        {
        }

        Admission& operator=(Admission&& other) noexcept
        {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }

        ~Admission() noexcept { release(); }

    private:
        void release() noexcept
        {
            if (owner_ != nullptr) {
                owner_->release();
                owner_ = nullptr;
            }
        }

        Impl* owner_{};
    };

    Impl(
        ManagerDoctorServiceConfiguration configuration,
        IManagerDoctorPlatformProbe& platformProbe,
        Contracts::IAgentCatalog& agentCatalog,
        Contracts::IAgentSessionRepository& sessionRepository,
        Contracts::ITelemetryService& telemetryService,
        Contracts::ILMStudioDeploymentService& lmStudioDeployment,
        const Contracts::IClock& clock)
        : configuration_{std::move(configuration)},
          platformProbe_{platformProbe},
          agentCatalog_{agentCatalog},
          sessionRepository_{sessionRepository},
          telemetryService_{telemetryService},
          lmStudioDeployment_{lmStudioDeployment},
          clock_{clock}
    {
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<Domain::DoctorReport> run(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto admitted = admit(context);
            if (!admitted) {
                return Domain::Result<Domain::DoctorReport>::failure(
                    std::move(admitted).error());
            }
            [[maybe_unused]] auto admission = std::move(admitted).value();

            std::optional<ManagerDoctorPlatformSnapshot> platform;
            auto platformResult = platformProbe_.inspect(
                configuration_.dataRoot,
                configuration_.centralStorePath,
                configuration_.installedBinaryPath,
                context);
            if (!platformResult) {
                if (isTerminalDependencyError(platformResult.error())) {
                    return Domain::Result<Domain::DoctorReport>::failure(
                        std::move(platformResult).error());
                }
            } else {
                auto snapshot = std::move(platformResult).value();
                auto valid = validatePlatformSnapshot(snapshot);
                if (!valid) {
                    return Domain::Result<Domain::DoctorReport>::failure(
                        std::move(valid).error());
                }
                platform.emplace(std::move(snapshot));
            }
            auto active = validateContext(clock_, context);
            if (!active) {
                return Domain::Result<Domain::DoctorReport>::failure(
                    std::move(active).error());
            }

            std::optional<std::size_t> agentCount;
            bool mandatoryAgentIdentitiesOk{};
            auto catalog = agentCatalog_.all(context);
            if (!catalog) {
                if (isTerminalDependencyError(catalog.error())) {
                    return Domain::Result<Domain::DoctorReport>::failure(
                        std::move(catalog).error());
                }
            } else {
                if (catalog.value().size() >
                    Application::AgentCatalog::MaximumEntries) {
                    return Domain::Result<Domain::DoctorReport>::failure(
                        integrityError(
                            "The agent catalog exceeded the Manager Doctor "
                            "output bound."));
                }
                agentCount = catalog.value().size();
                auto identities =
                    hasValidMandatoryAgentIdentities(catalog.value());
                if (!identities) {
                    return Domain::Result<Domain::DoctorReport>::failure(
                        std::move(identities).error());
                }
                mandatoryAgentIdentitiesOk = identities.value();
            }
            active = validateContext(clock_, context);
            if (!active) {
                return Domain::Result<Domain::DoctorReport>::failure(
                    std::move(active).error());
            }

            bool storeQueryOk{};
            auto storeQuery = sessionRepository_.quickCheck(context);
            if (!storeQuery) {
                if (isTerminalDependencyError(storeQuery.error())) {
                    return Domain::Result<Domain::DoctorReport>::failure(
                        std::move(storeQuery).error());
                }
            } else {
                storeQueryOk = true;
            }
            active = validateContext(clock_, context);
            if (!active) {
                return Domain::Result<Domain::DoctorReport>::failure(
                    std::move(active).error());
            }

            auto telemetry = unavailableHealth();
            bool telemetryHealthAvailable{};
            auto health = telemetryService_.health(context);
            if (!health) {
                if (isTerminalDependencyError(health.error())) {
                    return Domain::Result<Domain::DoctorReport>::failure(
                        std::move(health).error());
                }
            } else {
                auto report = std::move(health).value();
                if (!isSafeTelemetryReport(report)) {
                    return Domain::Result<Domain::DoctorReport>::failure(
                        integrityError(
                            "The telemetry service returned invalid Doctor "
                            "health text."));
                }
                telemetry = std::move(report);
                telemetryHealthAvailable = true;
            }
            active = validateContext(clock_, context);
            if (!active) {
                return Domain::Result<Domain::DoctorReport>::failure(
                    std::move(active).error());
            }

            bool telemetrySnapshotOk{};
            auto sample = telemetryService_.sample(true, context);
            if (!sample) {
                if (isTerminalDependencyError(sample.error())) {
                    return Domain::Result<Domain::DoctorReport>::failure(
                        std::move(sample).error());
                }
            } else if (sample.value()) {
                telemetrySnapshotOk = static_cast<bool>(
                    Domain::validateTelemetrySnapshot(
                        *sample.value(), configuration_.resourceBudgets));
            }
            active = validateContext(clock_, context);
            if (!active) {
                return Domain::Result<Domain::DoctorReport>::failure(
                    std::move(active).error());
            }

            std::optional<Domain::LMStudioPluginStatus> lmStudio;
            auto lmStatus = lmStudioDeployment_.status(
                Domain::LMStudioDeploymentRequest{
                    configuration_.installedBinaryPath,
                    true},
                configuration_.lmStudioReadAuthority,
                context);
            if (!lmStatus) {
                if (isTerminalDependencyError(lmStatus.error(), true)) {
                    return Domain::Result<Domain::DoctorReport>::failure(
                        std::move(lmStatus).error());
                }
            } else {
                lmStudio.emplace(std::move(lmStatus).value());
            }
            active = validateContext(clock_, context);
            if (!active) {
                return Domain::Result<Domain::DoctorReport>::failure(
                    std::move(active).error());
            }

            std::vector<Domain::DoctorCheck> checks;
            checks.reserve(12U);
            bool overallOk{true};
            const auto append = [&checks, &overallOk](
                                    std::string name,
                                    const bool ok,
                                    std::string detail,
                                    const bool hard = true) {
                if (hard && !ok) {
                    overallOk = false;
                }
                checks.push_back(doctorCheck(
                    std::move(name), ok, std::move(detail), hard));
            };

            const bool homeOk = platform && platform->homeDirectoryPresent;
            append(
                "home_layout",
                homeOk,
                homeOk ? "application data root present"
                       : "application data root unavailable");
            const bool storeOk = platform && platform->centralStorePresent;
            append(
                "sqlite_store",
                storeOk,
                storeOk ? "central store present"
                        : "central store unavailable");
            const bool catalogOk = agentCount && mandatoryAgentIdentitiesOk;
            append(
                "agent_catalog",
                catalogOk,
                catalogOk
                    ? std::to_string(*agentCount) + " agents loaded"
                    : agentCount ? "mandatory agent identities unavailable"
                                 : "agent catalog unavailable");
            append(
                "sqlite_query",
                storeQueryOk,
                storeQueryOk ? "central store quick check passed"
                             : "central store quick check failed");
            const bool gitOk = platform && platform->gitExecutablePath;
            append(
                "git_available",
                gitOk,
                gitOk ? "git.exe available" : "git.exe unavailable");
            const bool telemetryNativeOk =
                telemetryHealthAvailable && telemetry.ok;
            append(
                "telemetry_native",
                telemetryNativeOk,
                telemetryNativeOk ? "native telemetry healthy"
                                  : "native telemetry unavailable");
            const bool telemetryRuntimeOk = telemetryHealthAvailable &&
                supportedTelemetryRuntime(telemetry.runtime);
            append(
                "telemetry_runtime",
                telemetryRuntimeOk,
                telemetryRuntimeOk ? "Windows native telemetry runtime"
                                   : "unexpected telemetry runtime");
            append(
                "telemetry_snapshot",
                telemetrySnapshotOk,
                telemetrySnapshotOk ? "native telemetry contract valid"
                                    : "native telemetry snapshot unavailable");
            const bool binaryInstalled =
                platform && platform->installedBinaryExecutable;
            append(
                "windows_binary_install",
                binaryInstalled,
                binaryInstalled ? "installed CLI executable present"
                                : "installed CLI executable unavailable",
                false);
            const bool noLegacy =
                platform && platform->legacyLauncherBasenames.empty();
            append(
                "no_legacy_forge_serve",
                noLegacy,
                noLegacy ? "no legacy forge-serve launchers present"
                         : "legacy forge-serve launcher present");
            const bool nativeStdio = lmStudio &&
                lmStudio->binaryExecutable &&
                lmStudio->mcpConfigurationRegistered;
            append(
                "lm_studio_native_stdio",
                nativeStdio,
                nativeStdio ? "LM Studio native stdio registered"
                            : "LM Studio native stdio unavailable",
                false);
            const bool pluginOk = nativeStdio &&
                lmStudio->primaryPluginInstalled &&
                lmStudio->fallbackPluginInstalled;
            append(
                "lm_studio_mcp_plugin",
                pluginOk,
                pluginOk ? "LM Studio MCP plugins installed"
                         : "LM Studio MCP plugins unavailable",
                false);

            return Domain::Result<Domain::DoctorReport>::success(
                Domain::DoctorReport{
                    overallOk,
                    configuration_.productVersion,
                    configuration_.dataRoot,
                    std::move(checks),
                    std::move(telemetry),
                    binaryInstalled,
                    configuration_.installedBinaryPath});
        } catch (...) {
            return Domain::Result<Domain::DoctorReport>::failure(internalError(
                "The Manager Doctor operation failed safely."));
        }
    }

    void shutdown() noexcept
    {
        try {
            std::unique_lock lock{lifecycleMutex_};
            accepting_ = false;
            lifecycleChanged_.wait(
                lock, [this]() noexcept { return activeRuns_ == 0U; });
            shutdownComplete_ = true;
        } catch (...) {
        }
    }

private:
    [[nodiscard]] Domain::Result<Admission> admit(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto active = validateContext(clock_, context);
            if (!active) {
                return Domain::Result<Admission>::failure(
                    std::move(active).error());
            }
            std::lock_guard lock{lifecycleMutex_};
            if (!accepting_ || shutdownComplete_) {
                return Domain::Result<Admission>::failure(closedError());
            }
            active = validateContext(clock_, context);
            if (!active) {
                return Domain::Result<Admission>::failure(
                    std::move(active).error());
            }
            if (activeRuns_ == (std::numeric_limits<std::size_t>::max)()) {
                return Domain::Result<Admission>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The Manager Doctor admission count is exhausted."));
            }
            ++activeRuns_;
            return Domain::Result<Admission>::success(Admission{*this});
        } catch (...) {
            return Domain::Result<Admission>::failure(internalError(
                "The Manager Doctor operation could not be admitted."));
        }
    }

    void release() noexcept
    {
        try {
            std::lock_guard lock{lifecycleMutex_};
            if (activeRuns_ > 0U) {
                --activeRuns_;
            }
            if (activeRuns_ == 0U) {
                lifecycleChanged_.notify_all();
            }
        } catch (...) {
        }
    }

    const ManagerDoctorServiceConfiguration configuration_;
    IManagerDoctorPlatformProbe& platformProbe_;
    Contracts::IAgentCatalog& agentCatalog_;
    Contracts::IAgentSessionRepository& sessionRepository_;
    Contracts::ITelemetryService& telemetryService_;
    Contracts::ILMStudioDeploymentService& lmStudioDeployment_;
    const Contracts::IClock& clock_;

    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    std::size_t activeRuns_{};
    bool accepting_{true};
    bool shutdownComplete_{};
};

ManagerDoctorService::ManagerDoctorService(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

ManagerDoctorService::~ManagerDoctorService() noexcept = default;

Domain::Result<std::unique_ptr<ManagerDoctorService>>
ManagerDoctorService::create(
    ManagerDoctorServiceConfiguration configuration,
    IManagerDoctorPlatformProbe& platformProbe,
    Contracts::IAgentCatalog& agentCatalog,
    Contracts::IAgentSessionRepository& sessionRepository,
    Contracts::ITelemetryService& telemetryService,
    Contracts::ILMStudioDeploymentService& lmStudioDeployment,
    const Contracts::IClock& clock) noexcept
{
    try {
        if (!isSafeVersion(configuration.productVersion)) {
            return Domain::Result<std::unique_ptr<ManagerDoctorService>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The Manager Doctor product version is invalid."));
        }
        if (!isSafePath(configuration.dataRoot) ||
            !isSafePath(configuration.centralStorePath) ||
            !isSafePath(configuration.installedBinaryPath) ||
            !isAbsoluteWindowsPath(configuration.dataRoot) ||
            !isAbsoluteWindowsPath(configuration.centralStorePath) ||
            !isAbsoluteWindowsPath(configuration.installedBinaryPath)) {
            return Domain::Result<std::unique_ptr<ManagerDoctorService>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The Manager Doctor configuration contains an invalid "
                    "path."));
        }
        const auto& budgets = configuration.resourceBudgets;
        if (budgets.historyPointsDefault == 0U ||
            budgets.historyPointsHardMaximum == 0U ||
            budgets.historyPointsDefault > budgets.historyPointsHardMaximum) {
            return Domain::Result<std::unique_ptr<ManagerDoctorService>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The Manager Doctor telemetry history budget is invalid."));
        }
        const auto& authority = configuration.lmStudioReadAuthority;
        if (authority.intent() != Domain::FileAccess::Read ||
            !containsAccess(authority.grants(), Domain::FileAccess::Read) ||
            containsAccess(authority.denials(), Domain::FileAccess::Read) ||
            authority.trustedRoots().empty() ||
            std::any_of(
                authority.trustedRoots().begin(),
                authority.trustedRoots().end(),
                [](const Domain::PathText& root) noexcept {
                    return !isSafePath(root) ||
                        !isAbsoluteWindowsPath(root);
                })) {
            return Domain::Result<std::unique_ptr<ManagerDoctorService>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "The Manager Doctor requires a valid read-only LM Studio "
                    "authority."));
        }
        return Domain::Result<std::unique_ptr<ManagerDoctorService>>::success(
            std::unique_ptr<ManagerDoctorService>{new ManagerDoctorService{
                std::make_unique<Impl>(
                    std::move(configuration),
                    platformProbe,
                    agentCatalog,
                    sessionRepository,
                    telemetryService,
                    lmStudioDeployment,
                    clock)}});
    } catch (...) {
        return Domain::Result<std::unique_ptr<ManagerDoctorService>>::failure(
            internalError("The Manager Doctor service could not be created."));
    }
}

Domain::Result<Domain::DoctorReport> ManagerDoctorService::run(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return Domain::Result<Domain::DoctorReport>::failure(closedError());
    }
    return implementation_->run(context);
}

void ManagerDoctorService::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Composition::Windows
