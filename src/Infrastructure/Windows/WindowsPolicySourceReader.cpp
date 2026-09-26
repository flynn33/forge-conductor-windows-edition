#include "ForgeConductor/Infrastructure/Windows/WindowsPolicySourceReader.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace ForgeConductor::Infrastructure::Windows {
namespace {
constexpr std::size_t DerivedTextBytesMaximum = 128U * 1024U;

template<class T>
T take(Domain::Result<T> result)
{
    if (!result) throw std::runtime_error{result.error().message};
    return std::move(result).value();
}

void check(const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) throw std::runtime_error{"Policy import cancelled."};
    if (context.isExpired(std::chrono::steady_clock::now())) {
        throw std::runtime_error{"Policy import timed out. Retry the import."};
    }
}

std::string pathText(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path nativePath(const std::string& text)
{
    return std::filesystem::path{std::u8string_view{
        reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

Domain::PathText nativePathText(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return take(Domain::PathText::create(std::string{
        reinterpret_cast<const char*>(text.data()), text.size()}));
}

std::string normalizedText(std::string_view content)
{
    std::string result;
    result.reserve(content.size());
    for (std::size_t index{}; index < content.size(); ++index) {
        if (content[index] == '\r') {
            if (index + 1U < content.size() && content[index + 1U] == '\n') {
                ++index;
            }
            result.push_back('\n');
        } else {
            result.push_back(content[index]);
        }
    }
    return result;
}

class Algorithm final {
public:
    Algorithm()
    {
        if (BCryptOpenAlgorithmProvider(&value_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            throw std::runtime_error{"Cannot initialize policy hashing."};
        }
    }
    ~Algorithm() { if (value_) BCryptCloseAlgorithmProvider(value_, 0); }
    BCRYPT_ALG_HANDLE get() const noexcept { return value_; }
private:
    BCRYPT_ALG_HANDLE value_{};
};

std::string hashFile(const std::filesystem::path& path,
                     const Domain::OperationContext& context)
{
    Algorithm algorithm;
    DWORD objectBytes{}, resultBytes{};
    if (BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
            &resultBytes, 0) != 0 || objectBytes == 0) {
        throw std::runtime_error{"Cannot allocate policy hashing state."};
    }
    std::vector<unsigned char> object(objectBytes);
    BCRYPT_HASH_HANDLE raw{};
    if (BCryptCreateHash(algorithm.get(), &raw, object.data(),
            static_cast<ULONG>(object.size()), nullptr, 0, 0) != 0) {
        throw std::runtime_error{"Cannot create policy hash."};
    }
    struct HashCloser final {
        BCRYPT_HASH_HANDLE value{};
        ~HashCloser() { if (value) BCryptDestroyHash(value); }
    } closer{raw};
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"Cannot read policy entry."};
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        check(context);
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(raw,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count), 0) != 0) {
            throw std::runtime_error{"Cannot hash policy entry."};
        }
    }
    if (!input.eof()) throw std::runtime_error{"Policy entry changed while hashing."};
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(raw, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
        throw std::runtime_error{"Cannot finish policy hash."};
    }
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const auto byte : digest) encoded << std::setw(2) << +byte;
    return encoded.str();
}

Contracts::PolicySourceFile inspectFile(
    const std::filesystem::path& path,
    std::string relative,
    const Domain::OperationContext& context)
{
    Contracts::PolicySourceFile entry;
    entry.path = std::move(relative);
    entry.kind = "file";
    std::error_code error;
    entry.byteLength = std::filesystem::file_size(path, error);
    if (error) {
        entry.interpretation = "unavailable";
        entry.coverageDetail = "File metadata was unavailable during this revision.";
        return entry;
    }
    try {
        entry.contentHash = hashFile(path, context);
        if (entry.byteLength > DerivedTextBytesMaximum) {
            entry.interpretation = "opaque";
            entry.coverageDetail = "Content remains at source and is covered by its streamed SHA-256; derived text was not materialized.";
            return entry;
        }
        std::ifstream input{path, std::ios::binary};
        if (!input) throw std::runtime_error{"Cannot open policy entry."};
        std::string content(static_cast<std::size_t>(entry.byteLength), '\0');
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (input.gcount() != static_cast<std::streamsize>(content.size()) ||
            input.peek() != std::char_traits<char>::eof()) {
            throw std::runtime_error{"Policy entry changed during import."};
        }
        if (content.starts_with("\xEF\xBB\xBF")) content.erase(0, 3);
        if (Domain::isValidUtf8(content) && content.find('\0') == std::string::npos) {
            entry.content = normalizedText(content);
            entry.interpretation = "interpreted";
        } else {
            entry.interpretation = "opaque";
            entry.coverageDetail = "Binary or non-UTF-8 content is accepted and covered by its streamed SHA-256.";
        }
    } catch (const std::exception& failure) {
        entry.interpretation = "unavailable";
        entry.coverageDetail = failure.what();
    }
    return entry;
}

Contracts::PolicySourceBundle enumerateFolder(
    const std::filesystem::path& requestedRoot,
    std::string source,
    std::string commit,
    const Domain::OperationContext& context)
{
    const auto root = std::filesystem::canonical(requestedRoot);
    if (!std::filesystem::is_directory(root)) {
        throw std::runtime_error{"Choose an existing policy folder."};
    }
    Contracts::PolicySourceBundle bundle{std::move(source), std::move(commit), {}};
    std::error_code iteratorError;
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::skip_permission_denied, iteratorError};
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        check(context);
        const auto current = iterator->path();
        const auto relative = pathText(current.lexically_relative(root));
        std::error_code statusError;
        const auto status = iterator->symlink_status(statusError);
        if (current.filename() == L".git" && !statusError &&
            std::filesystem::is_directory(status)) {
            iterator.disable_recursion_pending();
        } else if (statusError) {
            bundle.files.push_back({relative, "unavailable", 0U, std::nullopt,
                std::nullopt, "unavailable",
                "Entry metadata was unavailable during this revision."});
        } else if (std::filesystem::is_symlink(status) || std::filesystem::is_other(status)) {
            if (std::filesystem::is_directory(status)) iterator.disable_recursion_pending();
            bundle.files.push_back({relative, "reparse", 0U, std::nullopt,
                std::nullopt, "opaque",
                "Reparse targets are not traversed; the entry remains visible as a coverage gap."});
        } else if (std::filesystem::is_directory(status)) {
            bundle.files.push_back({relative, "directory", 0U, std::nullopt,
                std::nullopt, "inventory", std::nullopt});
        } else if (std::filesystem::is_regular_file(status)) {
            bundle.files.push_back(inspectFile(current, relative, context));
        } else {
            bundle.files.push_back({relative, "unavailable", 0U, std::nullopt,
                std::nullopt, "unavailable",
                "The filesystem entry type could not be inventoried."});
        }
        iterator.increment(iteratorError);
        if (iteratorError) iteratorError.clear();
    }
    std::sort(bundle.files.begin(), bundle.files.end(),
        [](const auto& left, const auto& right) { return left.path < right.path; });
    return bundle;
}

Contracts::PolicySourceBundle cloneRepository(
    const std::string& source,
    const Domain::OperationContext& context)
{
    std::array<wchar_t, 32768> search{}, executable{};
    const auto length = GetEnvironmentVariableW(L"PATH", search.data(), static_cast<DWORD>(search.size()));
    const auto found = length && length < search.size()
        ? SearchPathW(search.data(), L"git.exe", nullptr,
            static_cast<DWORD>(executable.size()), executable.data(), nullptr) : 0;
    if (!found || found >= executable.size()) {
        throw std::runtime_error{"Git is required to bind a remote development-policy repository."};
    }
    std::filesystem::path git{executable.data()};
    const auto launcher = git.parent_path().parent_path() / L"bin" / L"git.exe";
    if (git.parent_path().filename() == L"cmd" && std::filesystem::is_regular_file(launcher)) git = launcher;

    WindowsUuidGenerator ids;
    const auto identity = take(ids.next()).value();
    const auto temporaryParent = std::filesystem::canonical(std::filesystem::temp_directory_path());
    const auto temporary = temporaryParent / ("ForgeConductor-policy-" + identity);
    if (temporary.parent_path() != temporaryParent || !std::filesystem::create_directory(temporary)) {
        throw std::runtime_error{"Cannot create an isolated policy import directory."};
    }
    struct Temporary final {
        std::filesystem::path root;
        std::filesystem::path parent;
        ~Temporary()
        {
            std::error_code ignored;
            if (root.is_absolute() && root.parent_path() == parent &&
                pathText(root.filename()).starts_with("ForgeConductor-policy-")) {
                std::filesystem::remove_all(root, ignored);
            }
        }
    } cleanup{temporary, temporaryParent};

    auto budgets = Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB);
    budgets.toolStdoutBytesMaximum = 64U * 1024U;
    budgets.toolStderrBytesMaximum = 64U * 1024U;
    SystemClock clock;
    auto diagnostics = std::make_shared<WindowsRuntimeDiagnostics>(clock, budgets);
    WindowsProcessSupervisor processes{budgets, diagnostics};
    const auto project = take(Domain::ProjectId::parse(identity));
    const auto executablePath = nativePathText(git);
    WindowsWorkspaceAuthority authority{{WindowsWorkspaceAuthorityPolicy{
        take(Domain::AuthorityId::parse(identity)), project,
        take(Domain::ClientId::parse("policy-import")),
        {nativePathText(temporary), nativePathText(git.parent_path())},
        Domain::FileAccess::Execute,
        {Domain::FileAccess::Read, Domain::FileAccess::Write,
         Domain::FileAccess::Create, Domain::FileAccess::Execute}, {}, true, 1U}}};
    const auto scope = take(authority.authorityFor(project, context));
    const auto run = [&](std::vector<std::string> arguments,
                         const std::filesystem::path& working) {
        check(context);
        arguments.insert(arguments.begin(), {"-c", "core.hooksPath=NUL",
            "-c", "core.fsmonitor=false", "-c", "core.quotepath=false"});
        Domain::ProcessRequest request{executablePath, std::move(arguments),
            nativePathText(working), {{"GIT_TERMINAL_PROMPT", "0"},
            {"GCM_INTERACTIVE", "Never"}}, true,
            (std::min)(std::chrono::milliseconds{120000},
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    context.deadline - std::chrono::steady_clock::now())),
            64U * 1024U, 64U * 1024U};
        auto output = take(processes.run(request, scope, context));
        if (output.cancelled) throw std::runtime_error{"Policy import was cancelled."};
        if (output.timedOut) throw std::runtime_error{"Policy import exceeded its time limit."};
        if (output.exitCode || !output.terminationConfirmed) {
            throw std::runtime_error{"Git could not import the policy repository using the current sign-in."};
        }
        return output.stdoutUtf8;
    };

    const auto repository = temporary / "repository";
    (void)run({"clone", "--quiet", "--depth=1", "--no-recurse-submodules",
        "--", source, pathText(repository)}, temporary);
    auto commit = run({"rev-parse", "HEAD"}, repository);
    while (!commit.empty() && (commit.back() == '\n' || commit.back() == '\r')) commit.pop_back();
    if (commit.empty()) throw std::runtime_error{"Git did not return an immutable policy revision."};
    return enumerateFolder(repository, source, commit, context);
}
} // namespace

Domain::Result<Contracts::PolicySourceBundle> WindowsPolicySourceReader::read(
    const std::string& source,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (source.empty()) throw std::runtime_error{"Select a development-policy source."};
        Contracts::PolicySourceBundle bundle;
        if (source.starts_with("https://") || source.starts_with("ssh://") || source.starts_with("git@")) {
            bundle = cloneRepository(source, context);
        } else {
            if (source.find("://") != std::string::npos) {
                throw std::runtime_error{"Use a local folder or a Git-compatible HTTPS or SSH repository URL."};
            }
            check(context);
            const auto root = std::filesystem::canonical(nativePath(source));
            bundle = enumerateFolder(root, pathText(root), {}, context);
        }
        return Domain::Result<Contracts::PolicySourceBundle>::success(std::move(bundle));
    } catch (const std::exception& failure) {
        return Domain::Result<Contracts::PolicySourceBundle>::failure(
            Domain::makeError(context.isCancellationRequested()
                ? Domain::ErrorCodes::Cancelled : Domain::ErrorCodes::InvalidRequest,
                failure.what()));
    } catch (...) {
        return Domain::Result<Contracts::PolicySourceBundle>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                "Development-policy import failed without replacing the active revision."));
    }
}
} // namespace ForgeConductor::Infrastructure::Windows
