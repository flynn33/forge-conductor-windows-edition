#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectRegistryRepository.h"
#include "Fakes/FoundationFakes.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;
namespace Persistence = ForgeConductor::Persistence::Windows;

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
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] std::string utf8(const std::wstring_view value)
{
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    REQUIRE(required > 0);
    std::string converted(static_cast<std::size_t>(required), '\0');
    REQUIRE(::WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), converted.data(), required,
                nullptr, nullptr) == required);
    return converted;
}

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        std::vector<wchar_t> buffer(32U * 1024U, L'\0');
        const DWORD length = ::GetTempPathW(
            static_cast<DWORD>(buffer.size()), buffer.data());
        REQUIRE(length > 0U && length < buffer.size());
        const std::filesystem::path base{
            std::wstring{buffer.data(), static_cast<std::size_t>(length)}};
        for (std::uint64_t attempt = 0U; attempt < 32U; ++attempt) {
            path_ = base /
                    (L"forge-project-registry-tests-" +
                     std::to_wstring(::GetCurrentProcessId()) + L"-" +
                     std::to_wstring(::GetTickCount64()) + L"-" +
                     std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
        }
        throw std::runtime_error{"Could not create project-registry test directory."};
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class CapabilityIssuer final : public Contracts::IWorkspaceAuthority {
public:
    [[nodiscard]] static Contracts::AuthorizedPath issue(
        const std::filesystem::path& root,
        const std::filesystem::path& target,
        const Domain::FileAccess access)
    {
        auto authority = take(issueAuthority(
            parse<Domain::AuthorityId>("11111111-1111-4111-8111-111111111111"),
            parse<Domain::ProjectId>("22222222-2222-4222-8222-222222222222"),
            parse<Domain::ClientId>("project-registry-test"),
            {take(Domain::PathText::create(utf8(root.native())))}, access,
            {access}, {}, false, 1U));
        return take(issueAuthorizedPath(
            authority,
            take(Domain::PathText::create(utf8(target.native()))),
            take(Domain::PathText::create(utf8(root.native()))), access));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The test capability issuer is static."));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority&,
        const std::vector<Domain::PathText>&,
        const std::vector<Domain::FileAccess>&,
        bool,
        std::uint64_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The test capability issuer is static."));
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority&,
        const Domain::PathAuthorizationRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::AuthorizedPath>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The test capability issuer is static."));
    }
};

[[nodiscard]] Domain::OperationContext context()
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("33333333-3333-4333-8333-333333333333"),
        std::chrono::steady_clock::now() + 10s,
        std::stop_token{},
        parse<Domain::CorrelationId>("project-registry-windows-test")};
}

void writeBytes(const std::filesystem::path& path, const std::string_view bytes)
{
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        static_cast<void>(std::filesystem::create_directories(parent, error));
        REQUIRE(!error);
    }
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    REQUIRE(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

struct Fixture final {
    TemporaryDirectory temporary;
    std::filesystem::path dataRoot{temporary.path() / L"AppData"};
    std::filesystem::path registryPath{dataRoot / L"projects" / L"registry.json"};
    std::filesystem::path projectOne{temporary.path() / L"ProjectOne"};
    std::filesystem::path projectTwo{temporary.path() / L"ProjectTwo"};
    std::shared_ptr<Infrastructure::WindowsApplicationPaths> applicationPaths;
    std::shared_ptr<Infrastructure::WindowsAtomicFileStore> fileStore{
        std::make_shared<Infrastructure::WindowsAtomicFileStore>()};
    std::shared_ptr<Fakes::SequenceUuidGenerator> uuidGenerator;
    std::shared_ptr<Infrastructure::BCryptSha256Hasher> hasher{
        std::make_shared<Infrastructure::BCryptSha256Hasher>()};
    std::shared_ptr<Fakes::FakeClock> clock{
        std::make_shared<Fakes::FakeClock>(
            Domain::UtcTimePoint{std::chrono::seconds{1'700'000'000}},
            std::chrono::steady_clock::now())};

    Fixture()
    {
        REQUIRE(std::filesystem::create_directory(dataRoot));
        REQUIRE(std::filesystem::create_directory(projectOne));
        REQUIRE(std::filesystem::create_directory(projectTwo));
        applicationPaths = std::make_shared<Infrastructure::WindowsApplicationPaths>(
            Infrastructure::WindowsApplicationPathsOptions{
                take(Domain::PathText::create(utf8(dataRoot.native()))), false});
        uuidGenerator = std::make_shared<Fakes::SequenceUuidGenerator>(
            std::vector<Domain::Uuid>{
                parse<Domain::Uuid>("44444444-4444-4444-8444-444444444444"),
                parse<Domain::Uuid>("55555555-5555-4555-8555-555555555555")});
    }

    [[nodiscard]] Persistence::WindowsProjectRegistryStoragePaths capabilities() const
    {
        return Persistence::WindowsProjectRegistryStoragePaths{
            CapabilityIssuer::issue(dataRoot, registryPath, Domain::FileAccess::Read),
            CapabilityIssuer::issue(dataRoot, registryPath, Domain::FileAccess::Write),
            CapabilityIssuer::issue(dataRoot, registryPath, Domain::FileAccess::Create),
            CapabilityIssuer::issue(
                dataRoot, registryPath.native() + L".bak", Domain::FileAccess::Read)};
    }

    [[nodiscard]] std::unique_ptr<Persistence::WindowsProjectRegistryRepository>
    repository() const
    {
        return std::make_unique<Persistence::WindowsProjectRegistryRepository>(
            applicationPaths, fileStore, capabilities(), uuidGenerator, hasher, clock,
            Domain::projectMemoryLimitsForProfile(
                Domain::ResourceProfile::Standard16GiB));
    }

    [[nodiscard]] static Domain::InitializeProjectRequest request(
        const std::filesystem::path& path,
        std::optional<Domain::ProjectId> requestedId = std::nullopt,
        std::optional<std::string> displayName = std::nullopt,
        std::optional<std::string> repositoryIdentity = std::nullopt)
    {
        return Domain::InitializeProjectRequest{
            take(Domain::PathText::create(utf8(path.native()))),
            std::move(requestedId), std::move(displayName),
            std::move(repositoryIdentity), std::nullopt};
    }
};

void initializePersistsAndDetachIsCaseInsensitive()
{
    Fixture fixture;
    const auto gitDirectory = fixture.projectOne / L".git";
    REQUIRE(std::filesystem::create_directory(gitDirectory));
    {
        std::ofstream config{gitDirectory / L"config", std::ios::binary};
        config << "[remote \"z\"]\n  url = https://example.test/z.git\n"
                  "[remote \"a\"]\n  url = https://example.test/a.git\n";
    }

    auto registry = fixture.repository();
    auto initializeResult = registry->initialize(
        Fixture::request(fixture.projectOne, std::nullopt, "First Project"),
        context());
    const auto initialized = take(std::move(initializeResult));
    REQUIRE(initialized.project.id.value() ==
            "44444444-4444-4444-8444-444444444444");
    REQUIRE(initialized.project.repositoryIdentity.has_value());
    REQUIRE(initialized.project.repositoryIdentity->starts_with("git:"));
    REQUIRE(initialized.project.aliases.size() == 1U);
    REQUIRE(std::filesystem::exists(fixture.registryPath));

    const auto reinitialized = take(registry->initialize(
        Fixture::request(
            fixture.projectOne, initialized.project.id, "Renamed Project"),
        context()));
    REQUIRE(reinitialized.project.id == initialized.project.id);
    REQUIRE(reinitialized.project.displayName == "Renamed Project");
    REQUIRE(std::filesystem::exists(fixture.registryPath.native() + L".bak"));

    auto restarted = fixture.repository();
    const auto descriptors = take(restarted->list(10U, context()));
    REQUIRE(descriptors.size() == 1U);
    REQUIRE(descriptors.front().id == initialized.project.id);

    auto aliasText = descriptors.front().aliases.front().value();
    std::transform(aliasText.begin(), aliasText.end(), aliasText.begin(),
                   [](const unsigned char value) {
                       return value >= 'A' && value <= 'Z'
                                  ? static_cast<char>(value + ('a' - 'A'))
                                  : static_cast<char>(value);
                   });
    REQUIRE(restarted->detachAlias(
        initialized.project.id, take(Domain::PathText::create(aliasText)), context()));
    const auto detached = take(restarted->descriptor(initialized.project.id, context()));
    REQUIRE(detached.aliases.empty());
    REQUIRE(std::filesystem::exists(fixture.registryPath));
}

void conflictingEvidenceFailsClosed()
{
    Fixture fixture;
    auto registry = fixture.repository();
    const auto first = take(registry->initialize(
        Fixture::request(
            fixture.projectOne, std::nullopt, "One", "repository-one"),
        context()));
    const auto second = take(registry->initialize(
        Fixture::request(
            fixture.projectTwo, std::nullopt, "Two", "repository-two"),
        context()));
    REQUIRE(first.project.id != second.project.id);

    const auto mismatch = registry->initialize(
        Fixture::request(
            fixture.projectTwo, first.project.id, std::nullopt, "repository-two"),
        context());
    REQUIRE(!mismatch);
    REQUIRE(mismatch.error().code == Domain::ErrorCodes::ProjectScopeMismatch);

    const auto missing = registry->initialize(
        Fixture::request(
            fixture.projectOne,
            parse<Domain::ProjectId>("66666666-6666-4666-8666-666666666666")),
        context());
    REQUIRE(!missing);
    REQUIRE(missing.error().code == Domain::ErrorCodes::ProjectNotFound);
}

void nonDirectoryIsRejected()
{
    Fixture fixture;
    const auto file = fixture.temporary.path() / L"not-a-directory.txt";
    {
        std::ofstream output{file, std::ios::binary};
        output << "not a project";
    }
    auto registry = fixture.repository();
    const auto result = registry->initialize(Fixture::request(file), context());
    REQUIRE(!result);
    REQUIRE(result.error().code == Domain::ErrorCodes::PathOutsideAuthority ||
            result.error().code == Domain::ErrorCodes::InvalidRequest);
    REQUIRE(!std::filesystem::exists(fixture.registryPath));
}

void hostileRegistryDocumentsFailClosedAndBackupRecovers()
{
    for (const auto& [document, expectedCode] :
         std::vector<std::pair<std::string, std::string>>{
             {R"({"schemaVersion":1,"schemaVersion":1,"projects":[]})",
              std::string{Domain::ErrorCodes::IntegrityFailure}},
             {R"({"schemaVersion":2,"projects":[]})",
              std::string{Domain::ErrorCodes::UnsupportedVersion}},
             {R"({"schemaVersion":1,"projects":[],"unexpected":true})",
              std::string{Domain::ErrorCodes::IntegrityFailure}}}) {
        Fixture fixture;
        writeBytes(fixture.registryPath, document);
        auto registry = fixture.repository();
        const auto result = registry->list(10U, context());
        REQUIRE(!result);
        REQUIRE(result.error().code == expectedCode);
    }

    {
        Fixture fixture;
        std::string oversized(
            Persistence::WindowsProjectRegistryRepository::MaximumDocumentBytes + 1U,
            'x');
        writeBytes(fixture.registryPath, oversized);
        auto registry = fixture.repository();
        const auto result = registry->list(10U, context());
        REQUIRE(!result);
        REQUIRE(result.error().code == Domain::ErrorCodes::IntegrityFailure);
    }

    {
        Fixture fixture;
        auto registry = fixture.repository();
        const auto initialized = take(registry->initialize(
            Fixture::request(fixture.projectOne, std::nullopt, "Original"),
            context()));
        const auto updated = take(registry->initialize(
            Fixture::request(
                fixture.projectOne, initialized.project.id, "Updated"),
            context()));
        REQUIRE(updated.project.displayName == "Updated");
        REQUIRE(std::filesystem::exists(fixture.registryPath.native() + L".bak"));

        writeBytes(fixture.registryPath, "{");
        auto restarted = fixture.repository();
        const auto recovered = take(restarted->descriptor(
            initialized.project.id, context()));
        REQUIRE(recovered.id == initialized.project.id);
        REQUIRE(recovered.displayName == "Original");
    }
}

void unsafePathFormsFailClosed()
{
    Fixture fixture;
    auto registry = fixture.repository();
    for (const std::string_view candidate : {
             R"(\\server\share\project)",
             R"(\\?\C:\project)",
             R"(C:\project:stream)",
             R"(..\relative-project)"}) {
        const auto path = take(Domain::PathText::create(candidate));
        const auto result = registry->initialize(
            Domain::InitializeProjectRequest{path}, context());
        REQUIRE(!result);
        REQUIRE(result.error().code == Domain::ErrorCodes::InvalidRequest ||
                result.error().code == Domain::ErrorCodes::PathOutsideAuthority);
    }
    REQUIRE(!std::filesystem::exists(fixture.registryPath));
}

} // namespace

int main()
{
    try {
        initializePersistsAndDetachIsCaseInsensitive();
        std::cout << "PASS project_registry.persistence_detach\n";
        conflictingEvidenceFailsClosed();
        std::cout << "PASS project_registry.conflicting_evidence\n";
        nonDirectoryIsRejected();
        std::cout << "PASS project_registry.non_directory\n";
        hostileRegistryDocumentsFailClosedAndBackupRecovers();
        std::cout << "PASS project_registry.hostile_document_recovery\n";
        unsafePathFormsFailClosed();
        std::cout << "PASS project_registry.unsafe_paths\n";
        std::cout << "SUMMARY passed=5 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
