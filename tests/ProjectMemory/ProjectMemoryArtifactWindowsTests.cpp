#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryArtifactStore.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepository.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Fakes/ToolServiceFakes.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Persistence/PersistenceTestSupport.h"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Support = ForgeConductor::Tests::PersistenceSupport;
using Json = nlohmann::json;

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
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::OperationContext operationContext(
    const std::string_view correlation,
    const Domain::MonotonicTimePoint now)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        now + std::chrono::minutes{2},
        {},
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] Domain::OperationContext controlledOperationContext(
    const std::string_view correlation,
    const Domain::MonotonicTimePoint deadline,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        deadline,
        cancellation,
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] std::vector<Domain::Uuid> uuids(const std::size_t count)
{
    std::vector<Domain::Uuid> values;
    values.reserve(count);
    for (std::size_t index = 1U; index <= count; ++index) {
        std::array<char, 37U> text{};
        const int written = std::snprintf(
            text.data(),
            text.size(),
            "10000000-0000-4000-8000-%012llx",
            static_cast<unsigned long long>(index));
        REQUIRE(written == 36);
        values.push_back(parse<Domain::Uuid>(text.data()));
    }
    return values;
}

class BlockingUuidGenerator final : public Contracts::IUuidGenerator {
public:
    explicit BlockingUuidGenerator(std::vector<Domain::Uuid> values)
        : values_{std::move(values)}
    {
    }

    ~BlockingUuidGenerator() override
    {
        releaseFirst();
    }

    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            if (next_ >= values_.size()) {
                return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The blocking UUID sequence is exhausted."));
            }
            const std::size_t index = next_++;
            if (index == 0U) {
                firstEntered_ = true;
                condition_.notify_all();
                condition_.wait(lock, [this]() noexcept { return firstReleased_; });
            }
            return Domain::Result<Domain::Uuid>::success(values_[index]);
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The blocking UUID generator failed safely."));
        }
    }

    [[nodiscard]] bool waitUntilFirstEntered(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return condition_.wait_for(
                lock, timeout, [this]() noexcept { return firstEntered_; });
        } catch (...) {
            return false;
        }
    }

    void releaseFirst() noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            firstReleased_ = true;
            condition_.notify_all();
        } catch (...) {
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Domain::Uuid> values_;
    std::size_t next_{};
    bool firstEntered_{};
    bool firstReleased_{};
};

class IntegrityFailureHasher final : public Contracts::IHasher {
public:
    [[nodiscard]] Domain::Result<Domain::Sha256Digest> sha256(
        const std::span<const std::byte> bytes) noexcept override
    {
        lastByteCount_ = bytes.size();
        ++calls_;
        return Domain::Result<Domain::Sha256Digest>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The injected hasher failed independently of artifact content."));
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
    [[nodiscard]] std::size_t lastByteCount() const noexcept
    {
        return lastByteCount_;
    }

private:
    std::size_t calls_{};
    std::size_t lastByteCount_{};
};

[[nodiscard]] std::filesystem::path nativePath(const Domain::PathText& path)
{
    return std::filesystem::path{take(
        InfrastructureWindows::Detail::strictUtf8ToUtf16(path.value()))};
}

[[nodiscard]] Domain::ProjectMemoryWrite memoryWrite(const std::size_t index)
{
    Domain::ProjectMemoryWrite write{};
    write.kind = "decision";
    write.title = "record " + std::to_string(index);
    write.summary = "summary " + std::to_string(index);
    write.body = "body " + std::to_string(index);
    write.tags = {"roundtrip"};
    return write;
}

struct ExportCapability final {
    Contracts::WorkspaceAuthority authority;
    Contracts::AuthorizedToolCall authorization;
};

[[nodiscard]] ExportCapability exportCapability(
    const Domain::ProjectId& projectId,
    const Domain::PathText& trustedRoot,
    const Domain::OperationContext& context,
    const Domain::ToolEffect effect)
{
    const auto authorityId = parse<Domain::AuthorityId>(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const auto clientId = parse<Domain::ClientId>("artifact-test-client");
    Fakes::DeterministicWorkspaceAuthority issuer{
        authorityId,
        clientId,
        {trustedRoot},
        Domain::FileAccess::Write,
        {Domain::FileAccess::Write},
        {},
        false,
        1U};
    issuer.setNow(std::chrono::steady_clock::now());
    auto authority = take(issuer.authorityFor(projectId, context));
    Fakes::DeterministicToolAuthorizerFake authorizer{
        "project_memory.export", effect, std::chrono::steady_clock::now()};
    Domain::ToolCallRequest call{
        Domain::McpRequestMetadata{
            parse<Domain::RequestId>(
                effect == Domain::ToolEffect::Write
                    ? "artifact-export-write"
                    : "artifact-export-read"),
            context.correlationId,
            clientId,
            projectId,
            "1.0"},
        "project_memory.export",
        "{}"};
    auto authorization = take(authorizer.authorize(
        Domain::ToolAuthorizationRequest{
            std::move(call),
            effect,
            Domain::AuthorityReference{
                authority.authorityId(), authority.generation()}},
        authority,
        context));
    return ExportCapability{
        std::move(authority), std::move(authorization)};
}

[[nodiscard]] std::string readText(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.good());
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    REQUIRE(end >= 0);
    REQUIRE(static_cast<std::uint64_t>(end) <= 32U * 1024U * 1024U);
    std::string text(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    if (!text.empty()) {
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(input.gcount() == static_cast<std::streamsize>(text.size()));
    }
    return text;
}

void writeText(const std::filesystem::path& path, const std::string_view text)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    REQUIRE(output.good());
}

void executeDatabaseSql(
    const std::filesystem::path& databasePath,
    const std::string_view sql)
{
    const auto encodedPath = Support::pathText(databasePath).value();
    sqlite3* database{};
    const int openResult = sqlite3_open_v2(
        encodedPath.c_str(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr);
    if (openResult != SQLITE_OK) {
        if (database != nullptr) {
            static_cast<void>(sqlite3_close_v2(database));
        }
        throw std::runtime_error{
            "Could not open the adversarial project-memory database."};
    }

    const int timeoutResult = sqlite3_busy_timeout(database, 5'000);
    char* errorMessage{};
    const int executeResult = timeoutResult == SQLITE_OK
        ? sqlite3_exec(
              database,
              std::string{sql}.c_str(),
              nullptr,
              nullptr,
              &errorMessage)
        : timeoutResult;
    std::string errorText;
    if (errorMessage != nullptr) {
        errorText = errorMessage;
        sqlite3_free(errorMessage);
    }
    const int closeResult = sqlite3_close_v2(database);
    if (executeResult != SQLITE_OK) {
        throw std::runtime_error{
            "Could not inject adversarial project-memory data: " + errorText};
    }
    REQUIRE(closeResult == SQLITE_OK);
}

void writeJsonWithTrailingWhitespace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::uintmax_t targetBytes)
{
    constexpr std::size_t BufferBytes = 64U * 1024U;
    const std::uintmax_t sourceBytes = std::filesystem::file_size(source);
    REQUIRE(sourceBytes != 0U);
    REQUIRE(sourceBytes < targetBytes);
    REQUIRE(targetBytes <= (32U * 1024U * 1024U) + 1U);

    std::ifstream input{source, std::ios::binary};
    REQUIRE(input.good());
    std::ofstream output{
        destination, std::ios::binary | std::ios::trunc};
    REQUIRE(output.good());

    std::array<char, BufferBytes> buffer{};
    std::uintmax_t remainingSource = sourceBytes;
    while (remainingSource != 0U) {
        const auto chunk = static_cast<std::size_t>(
            (std::min)(remainingSource, static_cast<std::uintmax_t>(buffer.size())));
        input.read(buffer.data(), static_cast<std::streamsize>(chunk));
        REQUIRE(input.gcount() == static_cast<std::streamsize>(chunk));
        output.write(buffer.data(), static_cast<std::streamsize>(chunk));
        REQUIRE(output.good());
        remainingSource -= chunk;
    }

    buffer.fill(' ');
    std::uintmax_t remainingPadding = targetBytes - sourceBytes;
    while (remainingPadding != 0U) {
        const auto chunk = static_cast<std::size_t>(
            (std::min)(remainingPadding, static_cast<std::uintmax_t>(buffer.size())));
        output.write(buffer.data(), static_cast<std::streamsize>(chunk));
        REQUIRE(output.good());
        remainingPadding -= chunk;
    }
    output.flush();
    REQUIRE(output.good());
    REQUIRE(std::filesystem::file_size(destination) == targetBytes);
}

[[nodiscard]] std::size_t immediateRegularFileCount(
    const std::filesystem::path& directory)
{
    std::size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        count += entry.is_regular_file() ? 1U : 0U;
    }
    return count;
}

[[nodiscard]] std::size_t ownedArtifactFileCount(
    const std::filesystem::path& exportsDirectory)
{
    const auto hasShape = [](
                              const std::wstring_view name,
                              const std::wstring_view prefix,
                              const std::wstring_view suffix) noexcept {
        return name.size() > prefix.size() + suffix.size() &&
               name.starts_with(prefix) && name.ends_with(suffix);
    };
    std::size_t count{};
    for (const auto& entry :
         std::filesystem::directory_iterator{exportsDirectory}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::wstring name = entry.path().filename().wstring();
        count += hasShape(name, L"memory-export-", L".json") ||
                         hasShape(name, L".memory-export-", L".json.tmp")
            ? 1U
            : 0U;
    }
    const auto quarantineDirectory = exportsDirectory / L"quarantine";
    if (std::filesystem::is_directory(quarantineDirectory)) {
        for (const auto& entry :
             std::filesystem::directory_iterator{quarantineDirectory}) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::wstring name = entry.path().filename().wstring();
            count += hasShape(name, L"corrupt-", L".json") ? 1U : 0U;
        }
    }
    return count;
}

void writeSparseFile(
    const std::filesystem::path& path,
    const std::uint64_t size,
    const std::string_view prefix)
{
    REQUIRE(size >= prefix.size());
    InfrastructureWindows::Detail::UniqueHandle file{::CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0U,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
        nullptr)};
    REQUIRE(static_cast<bool>(file));
    DWORD written{};
    REQUIRE(::WriteFile(
                file.get(),
                prefix.data(),
                static_cast<DWORD>(prefix.size()),
                &written,
                nullptr) != FALSE);
    REQUIRE(written == prefix.size());
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(size);
    REQUIRE(::SetFilePointerEx(file.get(), end, nullptr, FILE_BEGIN) != FALSE);
    REQUIRE(::SetEndOfFile(file.get()) != FALSE);
    REQUIRE(::FlushFileBuffers(file.get()) != FALSE);
}

[[nodiscard]] Domain::Sha256Digest sha256Text(
    Contracts::IHasher& hasher,
    const std::string& text)
{
    const std::span<const char> characters{text.data(), text.size()};
    return take(hasher.sha256(std::as_bytes(characters)));
}

[[nodiscard]] std::string foundationCompatibleJson(const Json& value)
{
    const std::string encoded = value.dump(
        -1, ' ', false, Json::error_handler_t::strict);
    std::string compatible;
    compatible.reserve(encoded.size());
    for (const char character : encoded) {
        if (character == '/') {
            compatible.push_back('\\');
        }
        compatible.push_back(character);
    }
    return compatible;
}

[[nodiscard]] char asciiLower(const char value) noexcept
{
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A'))
        : value;
}

[[nodiscard]] std::string lowerAscii(std::string value)
{
    for (auto& character : value) {
        character = asciiLower(character);
    }
    return value;
}

[[nodiscard]] Domain::Sha256Digest artifactRecordHash(
    Contracts::IHasher& hasher,
    const Json& record)
{
    std::string tags;
    for (const auto& tag : record.at("tags")) {
        if (!tags.empty()) {
            tags.push_back('\x1f');
        }
        tags += tag.get<std::string>();
    }
    const std::array<std::string, 5U> components{
        lowerAscii(record.at("kind").get<std::string>()),
        lowerAscii(record.at("title").get<std::string>()),
        lowerAscii(record.at("summary").get<std::string>()),
        lowerAscii(record.at("body").is_null()
                       ? std::string{}
                       : record.at("body").get<std::string>()),
        lowerAscii(std::move(tags))};
    std::string canonical;
    for (std::size_t index = 0U; index < components.size(); ++index) {
        if (index != 0U) {
            canonical.push_back('\x1e');
        }
        canonical += components[index];
    }
    return sha256Text(hasher, canonical);
}

struct Fixture final {
    Fixture(
        const std::filesystem::path& directory,
        const Domain::ProjectId& projectId,
        const std::size_t uuidCount = 256U)
        : Fixture{
              directory,
              projectId,
              uuids(uuidCount),
              std::make_shared<InfrastructureWindows::BCryptSha256Hasher>()}
    {
    }

    Fixture(
        const std::filesystem::path& directory,
        const Domain::ProjectId& projectId,
        std::vector<Domain::Uuid> uuidValues)
        : Fixture{
              directory,
              projectId,
              std::move(uuidValues),
              std::make_shared<InfrastructureWindows::BCryptSha256Hasher>()}
    {
    }

    Fixture(
        const std::filesystem::path& directory,
        const Domain::ProjectId& projectId,
        std::shared_ptr<Contracts::IHasher> injectedHasher,
        const std::size_t uuidCount = 256U)
        : Fixture{
              directory,
              projectId,
              uuids(uuidCount),
              std::move(injectedHasher)}
    {
    }

    Fixture(
        const std::filesystem::path& directory,
        const Domain::ProjectId& projectId,
        std::vector<Domain::Uuid> uuidValues,
        std::shared_ptr<Contracts::IHasher> injectedHasher)
    {
        REQUIRE(injectedHasher != nullptr);
        paths = std::make_shared<Fakes::RecordingApplicationPathsFake>();
        now = std::chrono::steady_clock::now();
        paths->setNow(now);
        paths->projectRootResult.set(
            Domain::Result<Domain::PathText>::success(Support::pathText(directory)));
        diagnostics = std::make_shared<Fakes::RuntimeDiagnosticsFake>(now);
        const auto day = std::chrono::sys_days{
            std::chrono::year{2026} / std::chrono::August / 26};
        clock = std::make_shared<Support::FixedClock>(
            Domain::UtcTimePoint{day.time_since_epoch() + std::chrono::hours{12}},
            now);
        redactor = std::make_shared<InfrastructureWindows::SecretRedactor>();
        hasher = std::move(injectedHasher);
        uuidGenerator = std::make_shared<Fakes::SequenceUuidGenerator>(
            std::move(uuidValues));
        artifactStore =
            std::make_shared<PersistenceWindows::WindowsProjectMemoryArtifactStore>(
                paths, uuidGenerator);
        PersistenceWindows::WindowsProjectMemoryRepositoryOptions options{};
        options.database.enableFts5 = false;
        repository = take(PersistenceWindows::WindowsProjectMemoryRepository::open(
            projectId,
            paths,
            artifactStore,
            diagnostics,
            redactor,
            hasher,
            uuidGenerator,
            clock,
            options,
            operationContext("artifact-repository-open", now)));
    }

    Domain::MonotonicTimePoint now{};
    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<Support::FixedClock> clock;
    std::shared_ptr<InfrastructureWindows::SecretRedactor> redactor;
    std::shared_ptr<Contracts::IHasher> hasher;
    std::shared_ptr<Fakes::SequenceUuidGenerator> uuidGenerator;
    std::shared_ptr<PersistenceWindows::WindowsProjectMemoryArtifactStore> artifactStore;
    std::shared_ptr<PersistenceWindows::WindowsProjectMemoryRepository> repository;
};

void storeAdmissionCancellationAndQuarantine()
{
    using namespace std::chrono_literals;

    Support::ScopedTestDirectory directory{L"project-memory-artifact-store"};
    const auto projectId = parse<Domain::ProjectId>(
        "66666666-6666-4666-8666-666666666666");
    const auto started = std::chrono::steady_clock::now();
    auto paths = std::make_shared<Fakes::RecordingApplicationPathsFake>();
    paths->setNow(started);
    paths->projectRootResult.set(
        Domain::Result<Domain::PathText>::success(
            Support::pathText(directory.path())));
    auto generator = std::make_shared<BlockingUuidGenerator>(uuids(8U));
    auto store =
        std::make_shared<PersistenceWindows::WindowsProjectMemoryArtifactStore>(
            paths, generator);
    const std::string artifactText = "{\"corrupt\":true}";
    const std::span<const char> artifactCharacters{
        artifactText.data(), artifactText.size()};
    const auto artifactBytes = std::as_bytes(artifactCharacters);

    const auto firstContext = controlledOperationContext(
        "artifact-admission-first", started + 2min);
    const auto firstCapability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        firstContext,
        Domain::ToolEffect::Write);
    auto first = std::async(std::launch::async, [&]() {
        return store->publish(
            projectId,
            artifactBytes,
            firstCapability.authority,
            firstCapability.authorization,
            firstContext);
    });
    if (!generator->waitUntilFirstEntered(2s)) {
        generator->releaseFirst();
        static_cast<void>(first.get());
        throw std::runtime_error{"The first artifact publish did not enter its UUID gate."};
    }

    std::stop_source cancellation;
    const auto cancelledContext = controlledOperationContext(
        "artifact-admission-cancelled",
        std::chrono::steady_clock::now() + 2min,
        cancellation.get_token());
    const auto cancelledCapability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        cancelledContext,
        Domain::ToolEffect::Write);
    auto cancelled = std::async(std::launch::async, [&]() {
        return store->publish(
            projectId,
            artifactBytes,
            cancelledCapability.authority,
            cancelledCapability.authorization,
            cancelledContext);
    });
    std::this_thread::sleep_for(20ms);
    cancellation.request_stop();
    const auto cancelledResult = cancelled.get();
    REQUIRE(!cancelledResult);
    REQUIRE(cancelledResult.error().code == Domain::ErrorCodes::Cancelled);

    const auto deadlineContext = controlledOperationContext(
        "artifact-admission-deadline",
        std::chrono::steady_clock::now() + 40ms);
    const auto deadlineCapability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        deadlineContext,
        Domain::ToolEffect::Write);
    const auto deadlineResult = store->publish(
        projectId,
        artifactBytes,
        deadlineCapability.authority,
        deadlineCapability.authorization,
        deadlineContext);
    REQUIRE(!deadlineResult);
    REQUIRE(deadlineResult.error().code == Domain::ErrorCodes::DeadlineExceeded);

    const auto secondContext = controlledOperationContext(
        "artifact-admission-second",
        std::chrono::steady_clock::now() + 2min);
    const auto secondCapability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        secondContext,
        Domain::ToolEffect::Write);
    auto second = std::async(std::launch::async, [&]() {
        return store->publish(
            projectId,
            artifactBytes,
            secondCapability.authority,
            secondCapability.authorization,
            secondContext);
    });
    const bool secondWaited = second.wait_for(25ms) == std::future_status::timeout;
    generator->releaseFirst();
    REQUIRE(secondWaited);
    const auto firstResult = take(first.get());
    const auto secondResult = take(second.get());
    REQUIRE(firstResult != secondResult);
    REQUIRE(std::filesystem::exists(nativePath(firstResult)));
    REQUIRE(std::filesystem::exists(nativePath(secondResult)));

    const auto readContext = controlledOperationContext(
        "artifact-quarantine-read",
        std::chrono::steady_clock::now() + 2min);
    auto retained = take(store->read(
        projectId, firstResult, 32U * 1024U * 1024U, readContext));
    const auto originalPath = nativePath(firstResult);
    writeText(originalPath, "changed-after-read");
    const auto changed = store->quarantineCorrupt(
        projectId,
        retained,
        controlledOperationContext(
            "artifact-quarantine-changed",
            std::chrono::steady_clock::now() + 2min));
    REQUIRE(!changed);
    REQUIRE(changed.error().code == Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(std::filesystem::exists(originalPath));
    REQUIRE(!std::filesystem::exists(
        directory.path() / L"exports" / L"quarantine"));

    writeText(originalPath, artifactText);
    const auto quarantined = take(store->quarantineCorrupt(
        projectId,
        retained,
        controlledOperationContext(
            "artifact-quarantine-commit",
            std::chrono::steady_clock::now() + 2min)));
    const auto quarantinePath = nativePath(quarantined);
    REQUIRE(!std::filesystem::exists(originalPath));
    REQUIRE(std::filesystem::exists(quarantinePath));
    REQUIRE(
        quarantinePath.parent_path() ==
        directory.path() / L"exports" / L"quarantine");
    REQUIRE(readText(quarantinePath) == artifactText);
}

void oversizedQuarantineUsesRetainedHandle()
{
    constexpr std::uint64_t MaximumArtifactBytes = 32U * 1024U * 1024U;
    Support::ScopedTestDirectory directory{
        L"project-memory-artifact-oversized-handle"};
    const auto projectId = parse<Domain::ProjectId>(
        "33333333-3333-4333-8333-333333333333");
    const auto started = std::chrono::steady_clock::now();
    auto paths = std::make_shared<Fakes::RecordingApplicationPathsFake>();
    paths->setNow(started);
    paths->projectRootResult.set(
        Domain::Result<Domain::PathText>::success(
            Support::pathText(directory.path())));
    auto generator = std::make_shared<Fakes::SequenceUuidGenerator>(uuids(1U));
    auto store =
        std::make_shared<PersistenceWindows::WindowsProjectMemoryArtifactStore>(
            paths, generator);

    const auto exportsDirectory = directory.path() / L"exports";
    REQUIRE(std::filesystem::create_directory(exportsDirectory));
    const auto oversizedPath = exportsDirectory / L"oversized-import.json";
    const std::string prefix = "retained-source";
    writeSparseFile(oversizedPath, MaximumArtifactBytes + 1U, prefix);
    const auto oversizedArtifact = Support::pathText(oversizedPath);
    const auto readResult = store->read(
        projectId,
        oversizedArtifact,
        static_cast<std::size_t>(MaximumArtifactBytes),
        operationContext("artifact-oversized-retain", started));
    REQUIRE(!readResult);
    REQUIRE(readResult.error().code == Domain::ErrorCodes::PayloadTooLarge);

    const auto replacementPath = exportsDirectory / L"replacement.json";
    writeText(replacementPath, "replacement");
    ::SetLastError(ERROR_SUCCESS);
    const BOOL replaced = ::MoveFileExW(
        replacementPath.c_str(),
        oversizedPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    REQUIRE(replaced == FALSE);
    REQUIRE(std::filesystem::exists(oversizedPath));
    REQUIRE(std::filesystem::exists(replacementPath));

    const auto quarantined = take(store->quarantineOversized(
        projectId,
        oversizedArtifact,
        static_cast<std::size_t>(MaximumArtifactBytes),
        operationContext("artifact-oversized-quarantine", started)));
    const auto quarantinePath = nativePath(quarantined);
    REQUIRE(!std::filesystem::exists(oversizedPath));
    REQUIRE(std::filesystem::exists(quarantinePath));
    REQUIRE(
        std::filesystem::file_size(quarantinePath) ==
        MaximumArtifactBytes + 1U);
    std::ifstream quarantinedInput{quarantinePath, std::ios::binary};
    REQUIRE(quarantinedInput.good());
    std::string retainedPrefix(prefix.size(), '\0');
    quarantinedInput.read(
        retainedPrefix.data(),
        static_cast<std::streamsize>(retainedPrefix.size()));
    REQUIRE(retainedPrefix == prefix);
    REQUIRE(generator->consumed() == 1U);
}

void ownedArtifactFileQuota()
{
    constexpr std::size_t MaximumOwnedFiles =
        Contracts::IProjectMemoryArtifactStore::
            MaximumOwnedArtifactFilesPerProject;
    static_assert(MaximumOwnedFiles == 256U);
    Support::ScopedTestDirectory directory{L"project-memory-artifact-file-quota"};
    const auto projectId = parse<Domain::ProjectId>(
        "22222222-2222-4222-8222-222222222222");
    const auto started = std::chrono::steady_clock::now();
    auto paths = std::make_shared<Fakes::RecordingApplicationPathsFake>();
    paths->setNow(started);
    paths->projectRootResult.set(
        Domain::Result<Domain::PathText>::success(
            Support::pathText(directory.path())));
    auto generator = std::make_shared<Fakes::SequenceUuidGenerator>(
        uuids(MaximumOwnedFiles));
    auto store =
        std::make_shared<PersistenceWindows::WindowsProjectMemoryArtifactStore>(
            paths, generator);
    const std::string artifactText = "{\"quota\":true}";
    const std::span<const char> characters{
        artifactText.data(), artifactText.size()};
    const auto content = std::as_bytes(characters);
    const auto context = operationContext("artifact-file-quota", started);
    const auto capability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        context,
        Domain::ToolEffect::Write);

    const auto first = take(store->publish(
        projectId,
        content,
        capability.authority,
        capability.authorization,
        context));
    auto retained = take(store->read(
        projectId, first, 32U * 1024U * 1024U, context));
    static_cast<void>(take(store->quarantineCorrupt(
        projectId, retained, context)));
    const auto exportsDirectory = directory.path() / L"exports";
    REQUIRE(ownedArtifactFileCount(exportsDirectory) == 1U);

    const auto staleStaging =
        exportsDirectory / L".memory-export-stale.json.tmp";
    writeText(staleStaging, "stale");
    REQUIRE(ownedArtifactFileCount(exportsDirectory) == 2U);

    for (std::size_t index = 0U; index < MaximumOwnedFiles - 2U; ++index) {
        static_cast<void>(take(store->publish(
            projectId,
            content,
            capability.authority,
            capability.authorization,
            context)));
    }
    REQUIRE(ownedArtifactFileCount(exportsDirectory) == MaximumOwnedFiles);
    REQUIRE(std::filesystem::exists(staleStaging));
    REQUIRE(generator->consumed() == MaximumOwnedFiles);

    const auto rejected = store->publish(
        projectId,
        content,
        capability.authority,
        capability.authorization,
        context);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == Domain::ErrorCodes::LimitExceeded);
    REQUIRE(ownedArtifactFileCount(exportsDirectory) == MaximumOwnedFiles);
    REQUIRE(generator->consumed() == MaximumOwnedFiles);
}

void quarantineQuotaEdge()
{
    constexpr std::size_t MaximumOwnedFiles =
        Contracts::IProjectMemoryArtifactStore::
            MaximumOwnedArtifactFilesPerProject;
    constexpr std::uint64_t MaximumArtifactBytes = 32U * 1024U * 1024U;
    Support::ScopedTestDirectory directory{
        L"project-memory-artifact-quarantine-quota"};
    const auto projectId = parse<Domain::ProjectId>(
        "11111111-1111-4111-8111-111111111111");
    const auto started = std::chrono::steady_clock::now();
    auto paths = std::make_shared<Fakes::RecordingApplicationPathsFake>();
    paths->setNow(started);
    paths->projectRootResult.set(
        Domain::Result<Domain::PathText>::success(
            Support::pathText(directory.path())));
    auto generator = std::make_shared<Fakes::SequenceUuidGenerator>(uuids(2U));
    auto store =
        std::make_shared<PersistenceWindows::WindowsProjectMemoryArtifactStore>(
            paths, generator);
    const auto context = operationContext("artifact-quarantine-quota", started);
    const auto exportsDirectory = directory.path() / L"exports";
    REQUIRE(std::filesystem::create_directory(exportsDirectory));
    for (std::size_t index = 0U; index < MaximumOwnedFiles; ++index) {
        writeText(
            exportsDirectory /
                (L"memory-export-manual-" + std::to_wstring(index) + L".json"),
            "owned");
    }
    REQUIRE(ownedArtifactFileCount(exportsDirectory) == MaximumOwnedFiles);
    REQUIRE(generator->consumed() == 0U);

    const std::string artifactText = "{\"quota\":true}";
    const auto arbitraryCorruptPath = exportsDirectory / L"untrusted-import.json";
    writeText(arbitraryCorruptPath, artifactText);
    auto arbitraryDocument = take(store->read(
        projectId,
        Support::pathText(arbitraryCorruptPath),
        32U * 1024U * 1024U,
        context));
    const auto corruptQuarantine = store->quarantineCorrupt(
        projectId, arbitraryDocument, context);
    REQUIRE(!corruptQuarantine);
    REQUIRE(
        corruptQuarantine.error().code == Domain::ErrorCodes::LimitExceeded);
    REQUIRE(std::filesystem::exists(arbitraryCorruptPath));
    REQUIRE(ownedArtifactFileCount(exportsDirectory) == MaximumOwnedFiles);
    REQUIRE(!std::filesystem::exists(exportsDirectory / L"quarantine"));
    REQUIRE(generator->consumed() == 0U);

    const auto arbitraryOversizedPath =
        exportsDirectory / L"untrusted-oversized.json";
    writeSparseFile(
        arbitraryOversizedPath, MaximumArtifactBytes + 1U, "oversized");
    const auto oversizedArtifact = Support::pathText(arbitraryOversizedPath);
    const auto oversizedRead = store->read(
        projectId,
        oversizedArtifact,
        static_cast<std::size_t>(MaximumArtifactBytes),
        context);
    REQUIRE(!oversizedRead);
    REQUIRE(oversizedRead.error().code == Domain::ErrorCodes::PayloadTooLarge);
    const auto oversizedQuarantine = store->quarantineOversized(
        projectId,
        oversizedArtifact,
        static_cast<std::size_t>(MaximumArtifactBytes),
        context);
    REQUIRE(!oversizedQuarantine);
    REQUIRE(
        oversizedQuarantine.error().code == Domain::ErrorCodes::LimitExceeded);
    REQUIRE(std::filesystem::exists(arbitraryOversizedPath));
    REQUIRE(ownedArtifactFileCount(exportsDirectory) == MaximumOwnedFiles);
    REQUIRE(!std::filesystem::exists(exportsDirectory / L"quarantine"));
    REQUIRE(generator->consumed() == 0U);
}

void snapshotRoundTripSecurityAndRollback()
{
    Support::ScopedTestDirectory directory{L"project-memory-artifact"};
    const auto projectId = parse<Domain::ProjectId>(
        "77777777-7777-4777-8777-777777777777");
    Fixture fixture{directory.path(), projectId};
    for (std::size_t index = 0U; index < 51U; ++index) {
        static_cast<void>(take(fixture.repository->remember(
            Domain::RememberProjectMemoryRequest{
                projectId, memoryWrite(index)},
            operationContext(
                "artifact-remember-" + std::to_string(index), fixture.now))));
    }

    const auto exportContext = operationContext("artifact-export", fixture.now);
    const auto wrongCapability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        exportContext,
        Domain::ToolEffect::Read);
    const auto unauthorized = fixture.repository->exportMemory(
        Domain::ExportProjectMemoryRequest{projectId},
        wrongCapability.authority,
        wrongCapability.authorization,
        exportContext);
    REQUIRE(!unauthorized);
    REQUIRE(unauthorized.error().code == Domain::ErrorCodes::Unauthorized);
    REQUIRE(!std::filesystem::exists(directory.path() / L"exports"));

    const auto capability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        exportContext,
        Domain::ToolEffect::Write);
    const auto exported = take(fixture.repository->exportMemory(
        Domain::ExportProjectMemoryRequest{projectId},
        capability.authority,
        capability.authorization,
        exportContext));
    REQUIRE(exported.recordCount == 51U);
    const auto artifact = nativePath(exported.artifact);
    REQUIRE(std::filesystem::exists(artifact));
    REQUIRE(artifact.parent_path() == directory.path() / L"exports");
    const std::string originalArtifact = readText(artifact);

    const auto hardLink = artifact.parent_path() / L"hard-link.json";
    REQUIRE(::CreateHardLinkW(hardLink.c_str(), artifact.c_str(), nullptr) != FALSE);
    const auto hardLinkRejected = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, exported.artifact, true, false},
        operationContext("artifact-hard-link", fixture.now));
    REQUIRE(!hardLinkRejected);
    REQUIRE(hardLinkRejected.error().code == Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(::DeleteFileW(hardLink.c_str()) != FALSE);

    const std::wstring alternateStream = artifact.native() + L":forbidden";
    const HANDLE stream = ::CreateFileW(
        alternateStream.c_str(),
        GENERIC_WRITE,
        0U,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    REQUIRE(stream != INVALID_HANDLE_VALUE);
    const char marker = 'x';
    DWORD written{};
    REQUIRE(::WriteFile(stream, &marker, 1U, &written, nullptr) != FALSE);
    REQUIRE(written == 1U);
    REQUIRE(::CloseHandle(stream) != FALSE);
    const auto streamRejected = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, exported.artifact, true, false},
        operationContext("artifact-alternate-stream", fixture.now));
    REQUIRE(!streamRejected);
    REQUIRE(streamRejected.error().code == Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(::DeleteFileW(alternateStream.c_str()) != FALSE);

    const auto outsidePath = directory.path() / L"outside.json";
    std::filesystem::copy_file(artifact, outsidePath);
    const auto outside = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, Support::pathText(outsidePath), true, false},
        operationContext("artifact-outside", fixture.now));
    REQUIRE(!outside);
    REQUIRE(outside.error().code == Domain::ErrorCodes::ProjectScopeMismatch);

    const std::string resetToken = "RESET PROJECT MEMORY " + projectId.value();
    static_cast<void>(take(fixture.repository->resetMemory(
        Domain::DestructiveConfirmation{
            "reset_project_memory", projectId.value(), resetToken},
        operationContext("artifact-reset-before-preview", fixture.now))));
    const auto preview = take(fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, exported.artifact, true, false},
        operationContext("artifact-preview", fixture.now)));
    REQUIRE(preview.disposition == Domain::ImportDisposition::Preview);
    REQUIRE(preview.recordCount == 51U);
    REQUIRE(preview.imported.empty());
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{projectId},
                operationContext("artifact-status-after-preview", fixture.now)))
                .recordCount == 0U);

    const auto imported = take(fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, exported.artifact, false, false},
        operationContext("artifact-import", fixture.now)));
    REQUIRE(imported.disposition == Domain::ImportDisposition::Imported);
    REQUIRE(imported.imported.size() == 51U);
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{projectId},
                operationContext("artifact-status-after-import", fixture.now)))
                .recordCount == 51U);
    const auto retry = take(fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, exported.artifact, false, false},
        operationContext("artifact-import-retry", fixture.now)));
    REQUIRE(retry.imported.size() == 51U);
    for (const auto& outcome : retry.imported) {
        REQUIRE(outcome.disposition == Domain::MemoryWriteDisposition::Deduplicated);
    }

    static_cast<void>(take(fixture.repository->resetMemory(
        Domain::DestructiveConfirmation{
            "reset_project_memory", projectId.value(), resetToken},
        operationContext("artifact-reset-before-hostile", fixture.now))));
    auto privateKeyDocument = Json::parse(originalArtifact);
    privateKeyDocument["records"][0U]["body"] =
        "-----BEGIN PRIVATE KEY-----\nmaterial";
    privateKeyDocument["records"][0U]["content_hash"] = artifactRecordHash(
        *fixture.hasher, privateKeyDocument["records"][0U]).value();
    const std::string privateRecords = foundationCompatibleJson(
        privateKeyDocument["records"]);
    privateKeyDocument["checksum"] =
        sha256Text(*fixture.hasher, privateRecords).value();
    const std::string privateArtifact = privateKeyDocument.dump();
    writeText(artifact, privateArtifact);
    const auto privateKey = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, exported.artifact, false, false},
        operationContext("artifact-private-key", fixture.now));
    REQUIRE(!privateKey);
    REQUIRE(privateKey.error().code == Domain::ErrorCodes::RedactionRejected);
    REQUIRE(!std::filesystem::exists(artifact));
    const auto quarantineDirectory =
        artifact.parent_path() / L"quarantine";
    REQUIRE(std::filesystem::is_directory(quarantineDirectory));
    std::vector<std::filesystem::path> quarantinedArtifacts;
    for (const auto& entry :
         std::filesystem::directory_iterator{quarantineDirectory}) {
        REQUIRE(entry.is_regular_file());
        quarantinedArtifacts.push_back(entry.path());
    }
    REQUIRE(quarantinedArtifacts.size() == 1U);
    REQUIRE(readText(quarantinedArtifacts[0U]) == privateArtifact);
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{projectId},
                operationContext("artifact-status-private-key", fixture.now)))
                .recordCount == 0U);

    auto corruptDocument = Json::parse(originalArtifact);
    corruptDocument["checksum"] = std::string(64U, '0');
    const std::string corruptArtifact = corruptDocument.dump();
    const auto corruptPath = artifact.parent_path() / L"preview-corrupt.json";
    writeText(corruptPath, corruptArtifact);
    const auto corruptArtifactPath = Support::pathText(corruptPath);
    const auto corrupt = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, corruptArtifactPath, true, false},
        operationContext("artifact-corrupt-checksum", fixture.now));
    REQUIRE(!corrupt);
    REQUIRE(corrupt.error().code == Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(std::filesystem::exists(corruptPath));
    REQUIRE(
        std::distance(
            std::filesystem::directory_iterator{quarantineDirectory},
            std::filesystem::directory_iterator{}) == 1);
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{projectId},
                operationContext("artifact-status-corrupt", fixture.now)))
                .recordCount == 0U);

    const auto committedCorrupt = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, corruptArtifactPath, false, false},
        operationContext("artifact-corrupt-checksum-commit", fixture.now));
    REQUIRE(!committedCorrupt);
    REQUIRE(
        committedCorrupt.error().code == Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(!std::filesystem::exists(corruptPath));
    quarantinedArtifacts.clear();
    for (const auto& entry :
         std::filesystem::directory_iterator{quarantineDirectory}) {
        REQUIRE(entry.is_regular_file());
        quarantinedArtifacts.push_back(entry.path());
    }
    REQUIRE(quarantinedArtifacts.size() == 2U);
    std::size_t privateArtifacts{};
    std::size_t corruptArtifacts{};
    for (const auto& quarantined : quarantinedArtifacts) {
        const auto text = readText(quarantined);
        privateArtifacts += text == privateArtifact ? 1U : 0U;
        corruptArtifacts += text == corruptArtifact ? 1U : 0U;
    }
    REQUIRE(privateArtifacts == 1U);
    REQUIRE(corruptArtifacts == 1U);
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{projectId},
                operationContext(
                    "artifact-status-committed-corrupt", fixture.now)))
                .recordCount == 0U);
}

void validArtifactSurvivesTargetDatabaseFailure()
{
    Support::ScopedTestDirectory directory{
        L"project-memory-artifact-database-failure"};
    const auto projectId = parse<Domain::ProjectId>(
        "33333333-3333-4333-8333-333333333333");
    Fixture fixture{directory.path(), projectId};

    auto insertedBeforeFailure = memoryWrite(1U);
    insertedBeforeFailure.title = "inserted before rollback";
    auto corruptCandidate = memoryWrite(2U);
    corruptCandidate.title = "corrupt dedup target";
    static_cast<void>(take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, insertedBeforeFailure},
        operationContext("artifact-database-first-source", fixture.now))));
    static_cast<void>(take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, corruptCandidate},
        operationContext("artifact-database-second-source", fixture.now))));

    const auto exportContext = operationContext(
        "artifact-database-export", fixture.now);
    const auto capability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        exportContext,
        Domain::ToolEffect::Write);
    const auto exported = take(fixture.repository->exportMemory(
        Domain::ExportProjectMemoryRequest{projectId},
        capability.authority,
        capability.authorization,
        exportContext));
    REQUIRE(exported.recordCount == 2U);
    const auto artifact = nativePath(exported.artifact);
    const std::string artifactText = readText(artifact);

    const std::string resetToken = "RESET PROJECT MEMORY " + projectId.value();
    static_cast<void>(take(fixture.repository->resetMemory(
        Domain::DestructiveConfirmation{
            "reset_project_memory", projectId.value(), resetToken},
        operationContext("artifact-database-reset", fixture.now))));
    const auto corruptTarget = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, corruptCandidate},
        operationContext("artifact-database-target", fixture.now)));
    executeDatabaseSql(
        directory.path() / L"memory.sqlite",
        "UPDATE memory_records SET title='tampered target row' WHERE id='" +
            corruptTarget.recordId.value() + "';");

    const auto before = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        operationContext("artifact-database-status-before", fixture.now)));
    REQUIRE(before.recordCount == 1U);
    const auto imported = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, exported.artifact, false, false},
        operationContext("artifact-database-import", fixture.now));
    REQUIRE(!imported);
    REQUIRE(imported.error().code == Domain::ErrorCodes::IntegrityFailure);

    const auto after = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        operationContext("artifact-database-status-after", fixture.now)));
    REQUIRE(after.recordCount == before.recordCount);
    REQUIRE(after.tombstoneCount == before.tombstoneCount);
    REQUIRE(after.eventCount == before.eventCount);
    REQUIRE(std::filesystem::exists(artifact));
    REQUIRE(readText(artifact) == artifactText);
    const auto quarantineDirectory = artifact.parent_path() / L"quarantine";
    REQUIRE(
        !std::filesystem::exists(quarantineDirectory) ||
        immediateRegularFileCount(quarantineDirectory) == 0U);
}

void validArtifactSurvivesValidationDependencyFailure()
{
    Support::ScopedTestDirectory sourceDirectory{
        L"project-memory-artifact-dependency-source"};
    const auto projectId = parse<Domain::ProjectId>(
        "77777777-7777-4777-8777-777777777777");
    Fixture source{sourceDirectory.path(), projectId};
    static_cast<void>(take(source.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, memoryWrite(1U)},
        operationContext("artifact-dependency-source-remember", source.now))));
    const auto sourceExportContext = operationContext(
        "artifact-dependency-source-export", source.now);
    const auto sourceCapability = exportCapability(
        projectId,
        Support::pathText(sourceDirectory.path()),
        sourceExportContext,
        Domain::ToolEffect::Write);
    const auto sourceExport = take(source.repository->exportMemory(
        Domain::ExportProjectMemoryRequest{projectId},
        sourceCapability.authority,
        sourceCapability.authorization,
        sourceExportContext));
    const std::string validArtifact = readText(nativePath(sourceExport.artifact));

    Support::ScopedTestDirectory targetDirectory{
        L"project-memory-artifact-dependency-target"};
    auto failingHasher = std::make_shared<IntegrityFailureHasher>();
    Fixture target{targetDirectory.path(), projectId, failingHasher};
    const auto retainContext = operationContext(
        "artifact-dependency-retain", target.now);
    const auto targetCapability = exportCapability(
        projectId,
        Support::pathText(targetDirectory.path()),
        retainContext,
        Domain::ToolEffect::Write);
    const std::span<const char> artifactCharacters{
        validArtifact.data(), validArtifact.size()};
    const auto retainedArtifact = take(target.artifactStore->publish(
        projectId,
        std::as_bytes(artifactCharacters),
        targetCapability.authority,
        targetCapability.authorization,
        retainContext));
    const auto retainedPath = nativePath(retainedArtifact);
    REQUIRE(std::filesystem::exists(retainedPath));
    REQUIRE(readText(retainedPath) == validArtifact);

    const auto before = take(target.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        operationContext("artifact-dependency-status-before", target.now)));
    const auto imported = target.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, retainedArtifact, false, false},
        operationContext("artifact-dependency-import", target.now));
    REQUIRE(!imported);
    REQUIRE(imported.error().code == Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(failingHasher->calls() >= 1U);
    REQUIRE(failingHasher->lastByteCount() > 0U);

    const auto after = take(target.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        operationContext("artifact-dependency-status-after", target.now)));
    REQUIRE(after.recordCount == before.recordCount);
    REQUIRE(after.tombstoneCount == before.tombstoneCount);
    REQUIRE(after.eventCount == before.eventCount);
    REQUIRE(std::filesystem::exists(retainedPath));
    REQUIRE(readText(retainedPath) == validArtifact);
    const auto quarantineDirectory = retainedPath.parent_path() / L"quarantine";
    REQUIRE(
        !std::filesystem::exists(quarantineDirectory) ||
        immediateRegularFileCount(quarantineDirectory) == 0U);
}

void futureSchemaExtensionsRemainAvailable()
{
    Support::ScopedTestDirectory directory{
        L"project-memory-artifact-future-schema"};
    const auto projectId = parse<Domain::ProjectId>(
        "88888888-8888-4888-8888-888888888888");
    Fixture fixture{directory.path(), projectId};
    static_cast<void>(take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, memoryWrite(1U)},
        operationContext("artifact-future-source-remember", fixture.now))));
    const auto exportContext = operationContext(
        "artifact-future-source-export", fixture.now);
    const auto exportAuthorization = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        exportContext,
        Domain::ToolEffect::Write);
    const auto exported = take(fixture.repository->exportMemory(
        Domain::ExportProjectMemoryRequest{projectId},
        exportAuthorization.authority,
        exportAuthorization.authorization,
        exportContext));
    const Json currentArtifact = Json::parse(readText(nativePath(exported.artifact)));
    REQUIRE(currentArtifact.at("records").size() == 1U);

    const auto retain = [&](const std::string_view text,
                            const std::string_view correlation) {
        const auto context = operationContext(correlation, fixture.now);
        const auto capability = exportCapability(
            projectId,
            Support::pathText(directory.path()),
            context,
            Domain::ToolEffect::Write);
        const std::span<const char> characters{text.data(), text.size()};
        return take(fixture.artifactStore->publish(
            projectId,
            std::as_bytes(characters),
            capability.authority,
            capability.authorization,
            context));
    };

    auto futureArtifact = currentArtifact;
    futureArtifact["schema_version"] = Domain::ProjectMemorySchemaVersion + 1U;
    futureArtifact["future_root"] = Json{
        {"capability", "extended"}, {"enabled", true}};
    futureArtifact["records"][0U]["schema_version"] =
        Domain::ProjectMemorySchemaVersion + 1U;
    futureArtifact["records"][0U]["future_record"] = Json{
        {"revision", 2U}, {"values", Json::array({"one", "two"})}};
    const auto futureRecords = foundationCompatibleJson(
        futureArtifact.at("records"));
    futureArtifact["checksum"] =
        sha256Text(*fixture.hasher, futureRecords).value();
    // Keep records first and schema_version last to prove the probe never invokes
    // current record semantics before discovering the future envelope version.
    const std::string futureText =
        std::string{"{\"records\":"} + futureRecords +
        ",\"future_root\":" +
        foundationCompatibleJson(futureArtifact.at("future_root")) +
        ",\"checksum\":" +
        foundationCompatibleJson(futureArtifact.at("checksum")) +
        ",\"created_at\":" +
        foundationCompatibleJson(futureArtifact.at("created_at")) +
        ",\"project_id\":" +
        foundationCompatibleJson(futureArtifact.at("project_id")) +
        ",\"schema_version\":" +
        foundationCompatibleJson(futureArtifact.at("schema_version")) + "}";
    const auto retainedFuture = retain(
        futureText, "artifact-future-retain");
    const auto retainedFuturePath = nativePath(retainedFuture);
    const auto before = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        operationContext("artifact-future-status-before", fixture.now)));
    const auto futureImport = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, retainedFuture, false, false},
        operationContext("artifact-future-import", fixture.now));
    REQUIRE(!futureImport);
    REQUIRE(futureImport.error().code == Domain::ErrorCodes::UnsupportedVersion);
    REQUIRE(std::filesystem::exists(retainedFuturePath));
    REQUIRE(readText(retainedFuturePath) == futureText);
    auto quarantineDirectory = retainedFuturePath.parent_path() / L"quarantine";
    REQUIRE(
        !std::filesystem::exists(quarantineDirectory) ||
        immediateRegularFileCount(quarantineDirectory) == 0U);

    auto olderArtifact = currentArtifact;
    olderArtifact["schema_version"] = 0U;
    olderArtifact["older_root_extension"] = "retained";
    olderArtifact["records"][0U]["older_record_extension"] = true;
    const auto olderRecords = foundationCompatibleJson(
        olderArtifact.at("records"));
    olderArtifact["checksum"] =
        sha256Text(*fixture.hasher, olderRecords).value();
    const auto olderText = foundationCompatibleJson(olderArtifact);
    const auto retainedOlder = retain(olderText, "artifact-older-retain");
    const auto retainedOlderPath = nativePath(retainedOlder);
    const auto olderImport = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, retainedOlder, false, false},
        operationContext("artifact-older-import", fixture.now));
    REQUIRE(!olderImport);
    REQUIRE(olderImport.error().code == Domain::ErrorCodes::UnsupportedVersion);
    REQUIRE(std::filesystem::exists(retainedOlderPath));
    REQUIRE(readText(retainedOlderPath) == olderText);
    REQUIRE(
        !std::filesystem::exists(quarantineDirectory) ||
        immediateRegularFileCount(quarantineDirectory) == 0U);

    auto recordMismatchArtifact = currentArtifact;
    recordMismatchArtifact["records"][0U]["schema_version"] =
        Domain::ProjectMemorySchemaVersion + 1U;
    recordMismatchArtifact["records"][0U]["future_record"] = true;
    const auto recordMismatchRecords = foundationCompatibleJson(
        recordMismatchArtifact.at("records"));
    recordMismatchArtifact["checksum"] =
        sha256Text(*fixture.hasher, recordMismatchRecords).value();
    const auto recordMismatchText = foundationCompatibleJson(
        recordMismatchArtifact);
    const auto retainedRecordMismatch = retain(
        recordMismatchText, "artifact-record-mismatch-retain");
    const auto retainedRecordMismatchPath = nativePath(retainedRecordMismatch);
    const auto recordMismatchImport = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, retainedRecordMismatch, false, false},
        operationContext("artifact-record-mismatch-import", fixture.now));
    REQUIRE(!recordMismatchImport);
    REQUIRE(
        recordMismatchImport.error().code == Domain::ErrorCodes::UnsupportedVersion);
    REQUIRE(std::filesystem::exists(retainedRecordMismatchPath));
    REQUIRE(readText(retainedRecordMismatchPath) == recordMismatchText);
    REQUIRE(
        !std::filesystem::exists(quarantineDirectory) ||
        immediateRegularFileCount(quarantineDirectory) == 0U);

    auto currentUnknown = currentArtifact;
    currentUnknown["records"][0U]["future_record"] = true;
    const auto currentUnknownRecords = foundationCompatibleJson(
        currentUnknown.at("records"));
    currentUnknown["checksum"] =
        sha256Text(*fixture.hasher, currentUnknownRecords).value();
    const auto currentUnknownText = foundationCompatibleJson(currentUnknown);
    const auto retainedCurrentUnknown = retain(
        currentUnknownText, "artifact-current-unknown-retain");
    const auto retainedCurrentUnknownPath = nativePath(retainedCurrentUnknown);
    const auto currentUnknownImport = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, retainedCurrentUnknown, false, false},
        operationContext("artifact-current-unknown-import", fixture.now));
    REQUIRE(!currentUnknownImport);
    REQUIRE(
        currentUnknownImport.error().code == Domain::ErrorCodes::InvalidRequest);
    REQUIRE(!std::filesystem::exists(retainedCurrentUnknownPath));
    REQUIRE(std::filesystem::exists(retainedFuturePath));
    REQUIRE(readText(retainedFuturePath) == futureText);
    REQUIRE(std::filesystem::exists(retainedOlderPath));
    REQUIRE(readText(retainedOlderPath) == olderText);
    REQUIRE(std::filesystem::exists(retainedRecordMismatchPath));
    REQUIRE(readText(retainedRecordMismatchPath) == recordMismatchText);
    quarantineDirectory = retainedCurrentUnknownPath.parent_path() / L"quarantine";
    REQUIRE(std::filesystem::is_directory(quarantineDirectory));
    REQUIRE(immediateRegularFileCount(quarantineDirectory) == 1U);
    REQUIRE(
        readText(std::filesystem::directory_iterator{quarantineDirectory}->path()) ==
        currentUnknownText);
    const auto after = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        operationContext("artifact-future-status-after", fixture.now)));
    REQUIRE(after.recordCount == before.recordCount);
    REQUIRE(after.tombstoneCount == before.tombstoneCount);
    REQUIRE(after.eventCount == before.eventCount);
}

void streamingSnapshotEncodingAndStrictParsing()
{
    Support::ScopedTestDirectory directory{L"project-memory-artifact-streaming"};
    const auto projectId = parse<Domain::ProjectId>(
        "66666666-6666-4666-8666-666666666666");
    Fixture fixture{directory.path(), projectId};

    auto write = memoryWrite(1U);
    write.title = "Root / Order";
    write.summary = "unicode caf\xc3\xa9";
    write.body = "C:\\repo/path\nquoted \"value\"";
    write.tags = {"zeta", "alpha"};
    write.importance = 0.75;
    write.confidence = 0.625;
    static_cast<void>(take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, std::move(write)},
        operationContext("artifact-streaming-remember", fixture.now))));

    const auto exportContext = operationContext(
        "artifact-streaming-export", fixture.now);
    const auto capability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        exportContext,
        Domain::ToolEffect::Write);
    const auto exported = take(fixture.repository->exportMemory(
        Domain::ExportProjectMemoryRequest{projectId},
        capability.authority,
        capability.authorization,
        exportContext));
    const auto exportedPath = nativePath(exported.artifact);
    const std::string exactExport = readText(exportedPath);
    const Json reference = Json::parse(exactExport);
    REQUIRE(exactExport == foundationCompatibleJson(reference));
    REQUIRE(exactExport.find("\\/") != std::string::npos);

    const auto exportsDirectory = exportedPath.parent_path();
    const std::string reordered =
        std::string{"{\n  \"records\":"} +
        foundationCompatibleJson(reference.at("records")) +
        ",\n  \"project_id\":" + reference.at("project_id").dump() +
        ",\n  \"schema_version\":" + reference.at("schema_version").dump() +
        ",\n  \"created_at\":" + reference.at("created_at").dump() +
        ",\n  \"checksum\":" + reference.at("checksum").dump() + "\n}";
    const auto reorderedPath = exportsDirectory / L"records-first.json";
    writeText(reorderedPath, reordered);
    const auto reorderedPreview = take(fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, Support::pathText(reorderedPath), true, false},
        operationContext("artifact-streaming-reordered", fixture.now)));
    REQUIRE(reorderedPreview.disposition == Domain::ImportDisposition::Preview);
    REQUIRE(reorderedPreview.recordCount == 1U);
    REQUIRE(reorderedPreview.imported.empty());

    const auto requirePreviewFailure = [&fixture, &projectId, &exportsDirectory](
                                           const std::wstring_view leaf,
                                           const std::string_view document,
                                           const std::string_view expectedCode) {
        const auto path = exportsDirectory / std::wstring{leaf};
        writeText(path, document);
        const auto result = fixture.repository->importMemory(
            Domain::ImportProjectMemoryRequest{
                projectId, Support::pathText(path), true, false},
            operationContext("artifact-streaming-invalid", fixture.now));
        REQUIRE(!result);
        REQUIRE(result.error().code == expectedCode);
        REQUIRE(std::filesystem::exists(path));
        REQUIRE(std::filesystem::remove(path));
    };

    std::string duplicateRoot = exactExport;
    duplicateRoot.insert(
        1U,
        "\"checksum\":" + reference.at("checksum").dump() + ",");
    requirePreviewFailure(
        L"duplicate-root.json",
        duplicateRoot,
        Domain::ErrorCodes::InvalidRequest);

    std::string duplicateRecord = exactExport;
    const auto recordStart = duplicateRecord.find("\"records\":[{");
    REQUIRE(recordStart != std::string::npos);
    duplicateRecord.insert(
        recordStart + std::string_view{"\"records\":[{"}.size(),
        "\"title\":\"duplicate\",");
    requirePreviewFailure(
        L"duplicate-record.json",
        duplicateRecord,
        Domain::ErrorCodes::InvalidRequest);

    requirePreviewFailure(
        L"trailing-junk.json",
        exactExport + "x",
        Domain::ErrorCodes::InvalidRequest);

    std::string invalidUtf8 = exactExport;
    invalidUtf8.insert(1U, 1U, static_cast<char>(0xff));
    requirePreviewFailure(
        L"invalid-utf8.json",
        invalidUtf8,
        Domain::ErrorCodes::InvalidRequest);

    auto unknownRoot = reference;
    unknownRoot["unknown"] = true;
    requirePreviewFailure(
        L"unknown-root.json",
        foundationCompatibleJson(unknownRoot),
        Domain::ErrorCodes::InvalidRequest);

    auto wrongNumericType = reference;
    wrongNumericType["records"][0U]["version"] = 1.0;
    const std::string wrongNumericRecords = foundationCompatibleJson(
        wrongNumericType.at("records"));
    wrongNumericType["checksum"] =
        sha256Text(*fixture.hasher, wrongNumericRecords).value();
    requirePreviewFailure(
        L"wrong-numeric-type.json",
        foundationCompatibleJson(wrongNumericType),
        Domain::ErrorCodes::InvalidRequest);

    auto mixedProjects = reference;
    auto secondRecord = mixedProjects["records"][0U];
    secondRecord["id"] = "22222222-2222-4222-8222-222222222222";
    secondRecord["project_id"] = "99999999-9999-4999-8999-999999999999";
    mixedProjects["records"].push_back(std::move(secondRecord));
    const std::string mixedRecords = foundationCompatibleJson(
        mixedProjects.at("records"));
    mixedProjects["checksum"] = sha256Text(*fixture.hasher, mixedRecords).value();
    requirePreviewFailure(
        L"mixed-projects.json",
        foundationCompatibleJson(mixedProjects),
        Domain::ErrorCodes::IntegrityFailure);

    auto excessiveDepth = reference;
    Json nestedTag = "alpha";
    for (std::size_t depth = 0U; depth < 33U; ++depth) {
        nestedTag = Json::array({std::move(nestedTag)});
    }
    excessiveDepth["records"][0U]["tags"] = std::move(nestedTag);
    requirePreviewFailure(
        L"excessive-depth.json",
        foundationCompatibleJson(excessiveDepth),
        Domain::ErrorCodes::LimitExceeded);

    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{projectId},
                operationContext("artifact-streaming-status", fixture.now)))
                .recordCount == 1U);
}

void macOsGoldenArtifactCompatibility(const std::filesystem::path& fixtureDirectory)
{
    const auto goldenSource =
        fixtureDirectory / L"macos-0.9.0-project-memory-export-v1.json";
    const std::string goldenText = readText(goldenSource);
    REQUIRE(goldenText.size() == 928U);

    InfrastructureWindows::BCryptSha256Hasher hasher;
    REQUIRE(
        sha256Text(hasher, goldenText) ==
        parse<Domain::Sha256Digest>(
            "516427f69516bf12aae9b570eb2fc2a964847b93fa6f99d46e487edc5e8e8a11"));
    const Json golden = Json::parse(goldenText);
    REQUIRE(
        golden.at("checksum").get<std::string>() ==
        "acf8451b46ba9e628aca70dbcfde94ef06ac2391583afd411819d81b34ca6b95");
    const std::string canonicalRecords = foundationCompatibleJson(
        golden.at("records"));
    REQUIRE(
        sha256Text(hasher, canonicalRecords) ==
        parse<Domain::Sha256Digest>(
            "acf8451b46ba9e628aca70dbcfde94ef06ac2391583afd411819d81b34ca6b95"));

    Support::ScopedTestDirectory exportDirectory{
        L"project-memory-artifact-golden-export"};
    const auto goldenProjectId = parse<Domain::ProjectId>(
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    Fixture exportFixture{
        exportDirectory.path(),
        goldenProjectId,
        std::vector<Domain::Uuid>{
            parse<Domain::Uuid>("11111111-1111-4111-8111-111111111111"),
            parse<Domain::Uuid>("33333333-3333-4333-8333-333333333333")}};
    Domain::ProjectMemoryWrite goldenWrite{};
    goldenWrite.kind = "decision";
    goldenWrite.title = "Golden Release Path";
    goldenWrite.summary = "Use the stable Windows package channel.";
    goldenWrite.body = "Keep source-compatible export semantics.";
    goldenWrite.tags = {"parity", "release"};
    goldenWrite.importance = 0.625;
    goldenWrite.confidence = 0.875;
    goldenWrite.sourceKind = "external_integration";
    goldenWrite.sourceReference = "fixture://macos-0.9.0/project-memory";
    goldenWrite.sessionId = parse<Domain::SessionId>(
        "22222222-2222-4222-8222-222222222222");
    const auto goldenRemembered = take(exportFixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            goldenProjectId, std::move(goldenWrite)},
        operationContext("artifact-golden-export-remember", exportFixture.now)));
    REQUIRE(
        goldenRemembered.contentHash ==
        parse<Domain::Sha256Digest>(
            "44fc6a2d1df70278526682732713c279fc81eae6c1a0ecc36a0c77e93ea1da04"));
    const auto exportContext = operationContext(
        "artifact-golden-export", exportFixture.now);
    const auto capability = exportCapability(
        goldenProjectId,
        Support::pathText(exportDirectory.path()),
        exportContext,
        Domain::ToolEffect::Write);
    const auto windowsExport = take(exportFixture.repository->exportMemory(
        Domain::ExportProjectMemoryRequest{goldenProjectId},
        capability.authority,
        capability.authorization,
        exportContext));
    REQUIRE(windowsExport.recordCount == 1U);
    REQUIRE(
        windowsExport.checksum ==
        parse<Domain::Sha256Digest>(
            "acf8451b46ba9e628aca70dbcfde94ef06ac2391583afd411819d81b34ca6b95"));
    REQUIRE(readText(nativePath(windowsExport.artifact)) == goldenText);

    Support::ScopedTestDirectory directory{L"project-memory-artifact-golden"};
    const auto targetProjectId = parse<Domain::ProjectId>(
        "55555555-5555-4555-8555-555555555555");
    Fixture fixture{directory.path(), targetProjectId};
    const auto exports = directory.path() / L"exports";
    REQUIRE(std::filesystem::create_directory(exports));
    const auto importedPath = exports / L"macos-golden.json";
    REQUIRE(std::filesystem::copy_file(goldenSource, importedPath));
    const auto importedArtifact = Support::pathText(importedPath);

    const auto preview = take(fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            targetProjectId, importedArtifact, true, true},
        operationContext("artifact-golden-preview", fixture.now)));
    REQUIRE(preview.disposition == Domain::ImportDisposition::Preview);
    REQUIRE(preview.recordCount == 1U);
    REQUIRE(preview.imported.empty());
    REQUIRE(std::filesystem::exists(importedPath));
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{targetProjectId},
                operationContext("artifact-golden-preview-status", fixture.now)))
                .recordCount == 0U);

    const auto denied = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            targetProjectId, importedArtifact, false, false},
        operationContext("artifact-golden-cross-project-deny", fixture.now));
    REQUIRE(!denied);
    REQUIRE(denied.error().code == Domain::ErrorCodes::ProjectScopeMismatch);
    REQUIRE(std::filesystem::exists(importedPath));
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{targetProjectId},
                operationContext("artifact-golden-deny-status", fixture.now)))
                .recordCount == 0U);

    const auto merged = take(fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            targetProjectId, importedArtifact, false, true},
        operationContext("artifact-golden-merge", fixture.now)));
    REQUIRE(merged.disposition == Domain::ImportDisposition::Imported);
    REQUIRE(merged.recordCount == 1U);
    REQUIRE(merged.imported.size() == 1U);
    REQUIRE(
        merged.imported[0U].disposition ==
        Domain::MemoryWriteDisposition::Inserted);
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{targetProjectId},
                operationContext("artifact-golden-merge-status", fixture.now)))
                .recordCount == 1U);

    const auto retry = take(fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            targetProjectId, importedArtifact, false, true},
        operationContext("artifact-golden-retry", fixture.now)));
    REQUIRE(retry.imported.size() == 1U);
    REQUIRE(
        retry.imported[0U].disposition ==
        Domain::MemoryWriteDisposition::Deduplicated);
}

void exactSnapshotCeilings()
{
    constexpr std::size_t MaximumArtifactRecords = 10'000U;
    constexpr std::uintmax_t MaximumArtifactBytes = 32U * 1024U * 1024U;
    constexpr std::size_t MaximumBatchRecords = 50U;

    Support::ScopedTestDirectory directory{L"project-memory-artifact-ceilings"};
    const auto projectId = parse<Domain::ProjectId>(
        "44444444-4444-4444-8444-444444444444");
    Fixture fixture{directory.path(), projectId, 20'100U};

    for (std::size_t first = 0U;
         first < MaximumArtifactRecords;
         first += MaximumBatchRecords) {
        std::vector<Domain::ProjectMemoryWrite> writes;
        writes.reserve(MaximumBatchRecords);
        for (std::size_t offset = 0U; offset < MaximumBatchRecords; ++offset) {
            writes.push_back(memoryWrite(first + offset));
        }
        const auto outcome = take(fixture.repository->rememberBatch(
            Domain::RememberProjectMemoryBatchRequest{
                projectId, std::move(writes)},
            operationContext(
                "artifact-ceiling-batch-" + std::to_string(first),
                std::chrono::steady_clock::now())));
        REQUIRE(outcome.results.size() == MaximumBatchRecords);
    }
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{projectId},
                operationContext(
                    "artifact-ceiling-source-status",
                    std::chrono::steady_clock::now())))
                .recordCount == MaximumArtifactRecords);

    const auto exportContext = operationContext(
        "artifact-ceiling-export", std::chrono::steady_clock::now());
    const auto capability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        exportContext,
        Domain::ToolEffect::Write);
    const auto exported = take(fixture.repository->exportMemory(
        Domain::ExportProjectMemoryRequest{projectId},
        capability.authority,
        capability.authorization,
        exportContext));
    REQUIRE(exported.recordCount == MaximumArtifactRecords);
    const auto artifact = nativePath(exported.artifact);
    const auto exportsDirectory = artifact.parent_path();
    REQUIRE(std::filesystem::file_size(artifact) < MaximumArtifactBytes);
    const std::size_t filesBeforeRejectedExport =
        immediateRegularFileCount(exportsDirectory);
    REQUIRE(filesBeforeRejectedExport == 1U);

    static_cast<void>(take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, memoryWrite(MaximumArtifactRecords)},
        operationContext(
            "artifact-ceiling-extra-record",
            std::chrono::steady_clock::now()))));
    const auto rejectedExportContext = operationContext(
        "artifact-ceiling-export-over", std::chrono::steady_clock::now());
    const auto rejectedCapability = exportCapability(
        projectId,
        Support::pathText(directory.path()),
        rejectedExportContext,
        Domain::ToolEffect::Write);
    const auto rejectedExport = fixture.repository->exportMemory(
        Domain::ExportProjectMemoryRequest{projectId},
        rejectedCapability.authority,
        rejectedCapability.authorization,
        rejectedExportContext);
    REQUIRE(!rejectedExport);
    REQUIRE(rejectedExport.error().code == Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(
        immediateRegularFileCount(exportsDirectory) ==
        filesBeforeRejectedExport);

    const std::string resetToken = "RESET PROJECT MEMORY " + projectId.value();
    static_cast<void>(take(fixture.repository->resetMemory(
        Domain::DestructiveConfirmation{
            "reset_project_memory", projectId.value(), resetToken},
        operationContext(
            "artifact-ceiling-reset-before-import",
            std::chrono::steady_clock::now()))));
    const auto imported = take(fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, exported.artifact, false, false},
        operationContext(
            "artifact-ceiling-committed-import",
            std::chrono::steady_clock::now())));
    REQUIRE(imported.disposition == Domain::ImportDisposition::Imported);
    REQUIRE(imported.recordCount == MaximumArtifactRecords);
    REQUIRE(imported.importableCount == MaximumArtifactRecords);
    REQUIRE(imported.imported.size() == MaximumArtifactRecords);
    for (const auto& outcome : imported.imported) {
        REQUIRE(outcome.disposition == Domain::MemoryWriteDisposition::Inserted);
    }
    const auto committedStatus = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        operationContext(
            "artifact-ceiling-committed-status",
            std::chrono::steady_clock::now())));
    REQUIRE(committedStatus.recordCount == MaximumArtifactRecords);
    // The reset plus 10,000 import events exceeds the bounded journal by one;
    // retention prunes the oldest event and preserves the exact 10,000-row cap.
    REQUIRE(committedStatus.eventCount == MaximumArtifactRecords);

    static_cast<void>(take(fixture.repository->resetMemory(
        Domain::DestructiveConfirmation{
            "reset_project_memory", projectId.value(), resetToken},
        operationContext(
            "artifact-ceiling-reset-before-previews",
            std::chrono::steady_clock::now()))));
    const auto exactPath = exportsDirectory / L"snapshot-exact-32mib.json";
    writeJsonWithTrailingWhitespace(artifact, exactPath, MaximumArtifactBytes);
    const auto exactPreview = take(fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, Support::pathText(exactPath), true, false},
        operationContext(
            "artifact-ceiling-exact-byte-preview",
            std::chrono::steady_clock::now())));
    REQUIRE(exactPreview.disposition == Domain::ImportDisposition::Preview);
    REQUIRE(exactPreview.recordCount == MaximumArtifactRecords);
    REQUIRE(exactPreview.imported.empty());
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{projectId},
                operationContext(
                    "artifact-ceiling-exact-preview-status",
                    std::chrono::steady_clock::now())))
                .recordCount == 0U);

    const auto overPath = exportsDirectory / L"snapshot-over-32mib.json";
    writeJsonWithTrailingWhitespace(
        artifact, overPath, MaximumArtifactBytes + 1U);
    const auto overPreview = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, Support::pathText(overPath), true, false},
        operationContext(
            "artifact-ceiling-over-byte-preview",
            std::chrono::steady_clock::now()));
    REQUIRE(!overPreview);
    REQUIRE(overPreview.error().code == Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{projectId},
                operationContext(
                    "artifact-ceiling-over-preview-status",
                    std::chrono::steady_clock::now())))
                .recordCount == 0U);

    const auto committedOver = fixture.repository->importMemory(
        Domain::ImportProjectMemoryRequest{
            projectId, Support::pathText(overPath), false, false},
        operationContext(
            "artifact-ceiling-over-byte-commit",
            std::chrono::steady_clock::now()));
    REQUIRE(!committedOver);
    REQUIRE(committedOver.error().code == Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(!std::filesystem::exists(overPath));
    const auto quarantineDirectory = exportsDirectory / L"quarantine";
    REQUIRE(std::filesystem::is_directory(quarantineDirectory));
    REQUIRE(immediateRegularFileCount(quarantineDirectory) == 1U);
    const auto quarantinedArtifact =
        std::filesystem::directory_iterator{quarantineDirectory}->path();
    REQUIRE(
        std::filesystem::file_size(quarantinedArtifact) ==
        MaximumArtifactBytes + 1U);
    REQUIRE(take(fixture.repository->status(
                Domain::ProjectMemoryStatusRequest{projectId},
                operationContext(
                    "artifact-ceiling-over-commit-status",
                    std::chrono::steady_clock::now())))
                .recordCount == 0U);
}

} // namespace

int main(const int argc, const char* const* const argv)
{
    try {
        REQUIRE(argc == 2);
        REQUIRE(argv != nullptr);
        REQUIRE(argv[1] != nullptr);
        storeAdmissionCancellationAndQuarantine();
        std::cout << "PASS project_memory_artifact.store_boundary\n";
        oversizedQuarantineUsesRetainedHandle();
        std::cout << "PASS project_memory_artifact.oversized_retained_handle\n";
        ownedArtifactFileQuota();
        std::cout << "PASS project_memory_artifact.owned_file_quota\n";
        quarantineQuotaEdge();
        std::cout << "PASS project_memory_artifact.quarantine_quota_edge\n";
        snapshotRoundTripSecurityAndRollback();
        std::cout << "PASS project_memory_artifact.roundtrip_security_rollback\n";
        validArtifactSurvivesTargetDatabaseFailure();
        std::cout << "PASS project_memory_artifact.valid_artifact_database_failure\n";
        validArtifactSurvivesValidationDependencyFailure();
        std::cout << "PASS project_memory_artifact.dependency_failure_provenance\n";
        futureSchemaExtensionsRemainAvailable();
        std::cout << "PASS project_memory_artifact.future_schema_extensions\n";
        streamingSnapshotEncodingAndStrictParsing();
        std::cout << "PASS project_memory_artifact.streaming_strictness\n";
        macOsGoldenArtifactCompatibility(std::filesystem::path{argv[1]});
        std::cout << "PASS project_memory_artifact.macos_golden_compatibility\n";
        exactSnapshotCeilings();
        std::cout << "PASS project_memory_artifact.exact_snapshot_ceilings\n";
        std::cout << "SUMMARY passed=11 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
