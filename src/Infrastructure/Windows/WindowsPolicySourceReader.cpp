#include "ForgeConductor/Infrastructure/Windows/WindowsPolicySourceReader.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include <Windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sstream>

namespace ForgeConductor::Infrastructure::Windows {
namespace {
using Json = nlohmann::json;
constexpr std::size_t MaximumFileBytes = 2U * 1024U * 1024U;
constexpr std::size_t MaximumBundleBytes = 16U * 1024U * 1024U;
constexpr std::size_t MaximumFiles = 2048U;
class GitHubUnavailable final : public std::runtime_error { public: using std::runtime_error::runtime_error; };
template<class T> T take(Domain::Result<T> result)
{ if (!result) throw std::runtime_error{result.error().message}; return std::move(result).value(); }

class Internet final {
public:
    explicit Internet(HINTERNET value) : value_{value} {}
    ~Internet() { close(); }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    HINTERNET get() const noexcept { return value_.load(); }
    void close() noexcept { if (const auto value = value_.exchange(nullptr)) WinHttpCloseHandle(value); }
private:
    std::atomic<HINTERNET> value_;
};

void check(const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) throw std::runtime_error{"Policy import cancelled."};
    if (context.isExpired(std::chrono::steady_clock::now())) throw std::runtime_error{"Policy import timed out. Retry the import."};
}

std::string pathText(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path nativePath(const std::string& text)
{ return std::filesystem::path{std::u8string_view{reinterpret_cast<const char8_t*>(text.data()), text.size()}}; }

Domain::PathText nativePathText(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return take(Domain::PathText::create(std::string{reinterpret_cast<const char*>(text.data()), text.size()}));
}

bool supported(const std::string& name)
{
    auto extension = pathText(nativePath(name).extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return extension == ".md" || extension == ".txt" || extension == ".json" ||
        extension == ".yaml" || extension == ".yml" || extension == ".toml" ||
        extension == ".py" || extension == ".ps1" || extension == ".rst" ||
        extension == ".csv" || name == "LICENSE" || name == "NOTICE";
}

void append(Contracts::PolicySourceBundle& bundle, std::string path,
    std::string content, std::size_t& bytes)
{
    if (content.size() > MaximumFileBytes || content.size() > MaximumBundleBytes - bytes ||
        bundle.files.size() >= MaximumFiles)
        throw std::runtime_error{"Policy source exceeds the complete snapshot limit (2048 text files, 2 MiB per file, 16 MiB total). No policy was adopted."};
    if (content.starts_with("\xEF\xBB\xBF")) content.erase(0, 3);
    if (!Domain::isValidUtf8(content) || content.find('\0') != std::string::npos)
        throw std::runtime_error{"Policy text must be valid UTF-8 without NUL bytes: " + path};
    bytes += content.size();
    bundle.files.push_back({std::move(path), std::move(content)});
}

std::string encodedPath(std::string_view path)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : path) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.') result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4U]; result += hex[c & 15U]; }
    }
    return result;
}

std::string get(const wchar_t* host, const std::string& path,
    std::size_t limit, const Domain::OperationContext& context)
{
    check(context);
    Internet session{WinHttpOpen(L"ForgeConductor-Policy-Import", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.get()) throw std::runtime_error{"Cannot initialize policy download."};
    Internet connection{WinHttpConnect(session.get(), host, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    const std::wstring nativePath{path.begin(), path.end()};
    Internet request{connection.get() ? WinHttpOpenRequest(connection.get(), L"GET", nativePath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr};
    if (!request.get()) throw std::runtime_error{"Cannot open policy download."};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(context.deadline - std::chrono::steady_clock::now()).count();
    const int timeout = static_cast<int>(std::clamp<std::int64_t>(remaining, 1, 15000));
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetTimeouts(request.get(), timeout, timeout, timeout, timeout) ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)))
        throw std::runtime_error{"Cannot configure bounded policy download."};
    std::stop_callback cancelled{context.cancellation, [&request]() noexcept { request.close(); }};
    if (!WinHttpSendRequest(request.get(), L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1L),
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
        check(context);
        throw GitHubUnavailable{"GitHub could not be reached. Retry or import a local policy folder."};
    }
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
        throw GitHubUnavailable{"Policy download returned HTTP " + std::to_string(status) +
            ". Use a public GitHub repository URL or a local checkout; rate limits may require retrying later."};
    std::string result;
    for (;;) {
        check(context);
        std::array<char, 16384> buffer{};
        DWORD read{};
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read))
            throw std::runtime_error{"Policy download was interrupted."};
        if (!read) break;
        if (read > limit - result.size()) throw std::runtime_error{"Policy response exceeds its complete download limit."};
        result.append(buffer.data(), read);
    }
    return result;
}

Contracts::PolicySourceBundle authenticatedGit(const std::string& name, const Domain::OperationContext& context)
{
    // Use the user's existing Git credential helper through Git itself. No
    // credential is read, copied into an argument, or returned to the caller.
    std::array<wchar_t, 32768> search{}, executable{};
    const auto length = GetEnvironmentVariableW(L"PATH", search.data(), static_cast<DWORD>(search.size()));
    const auto found = length && length < search.size()
        ? SearchPathW(search.data(), L"git.exe", nullptr, static_cast<DWORD>(executable.size()), executable.data(), nullptr) : 0;
    if (!found || found >= executable.size()) throw std::runtime_error{"This policy repository needs Git access. Install Git and sign in, or import an existing local checkout."};
    std::filesystem::path git{executable.data()};
    // Git for Windows' PATH shim can be hard-linked by its installer. Its
    // sibling bin launcher preserves installation discovery and satisfies the
    // supervisor's independent executable ownership check.
    const auto launcher = git.parent_path().parent_path() / L"bin" / L"git.exe";
    if (git.parent_path().filename() == L"cmd" && std::filesystem::is_regular_file(launcher)) git = launcher;
    WindowsUuidGenerator ids;
    const auto identity = take(ids.next()).value();
    const auto temporaryParent = std::filesystem::canonical(std::filesystem::temp_directory_path());
    const auto temporary = temporaryParent / ("ForgeConductor-policy-" + identity);
    if (temporary.parent_path() != temporaryParent || !std::filesystem::create_directory(temporary))
        throw std::runtime_error{"Cannot create an isolated policy import directory."};
    struct Temporary final {
        std::filesystem::path root;
        std::filesystem::path parent;
        ~Temporary() {
            std::error_code ignored;
            if (root.is_absolute() && root.parent_path() == parent && pathText(root.filename()).starts_with("ForgeConductor-policy-"))
                std::filesystem::remove_all(root, ignored);
        }
    } cleanup{temporary, temporaryParent};
    auto budgets = Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB);
    budgets.toolStdoutBytesMaximum = MaximumFileBytes;
    budgets.toolStderrBytesMaximum = 32768;
    SystemClock clock;
    auto diagnostics = std::make_shared<WindowsRuntimeDiagnostics>(clock, budgets);
    WindowsProcessSupervisor processes{budgets, diagnostics};
    const auto project = take(Domain::ProjectId::parse(identity));
    const auto temporaryPath = nativePathText(temporary);
    const auto executablePath = nativePathText(git);
    WindowsWorkspaceAuthority authority{{WindowsWorkspaceAuthorityPolicy{
        take(Domain::AuthorityId::parse(identity)), project, take(Domain::ClientId::parse("policy-import")),
        {temporaryPath, nativePathText(git.parent_path())}, Domain::FileAccess::Execute,
        {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create, Domain::FileAccess::Execute}, {}, true, 1U}}};
    const auto scope = take(authority.authorityFor(project, context));
    const auto run = [&](std::vector<std::string> arguments, const std::filesystem::path& working) {
        check(context);
        arguments.insert(arguments.begin(), {"-c", "core.hooksPath=NUL", "-c", "core.fsmonitor=false", "-c", "core.quotepath=false"});
        Domain::ProcessRequest request{executablePath, std::move(arguments), nativePathText(working),
            {{"GIT_TERMINAL_PROMPT", "0"}, {"GCM_INTERACTIVE", "Never"}}, true,
            std::min(std::chrono::milliseconds{120000}, std::chrono::duration_cast<std::chrono::milliseconds>(context.deadline - std::chrono::steady_clock::now())),
            MaximumFileBytes, 32768};
        auto output = take(processes.run(request, scope, context));
        if (output.exitCode || output.cancelled || output.timedOut || !output.terminationConfirmed)
            throw std::runtime_error{"Git could not import the policy using the current sign-in. Sign in to Git for this repository or import a local checkout, then retry."};
        if (output.stdoutTruncated || output.stderrTruncated) throw std::runtime_error{"Git policy output exceeded the complete import limit."};
        return output.stdoutUtf8;
    };
    const auto repository = temporary / "repository";
    (void)run({"clone", "--quiet", "--depth=1", "--filter=blob:none", "--no-checkout", "--", "https://github.com/" + name + ".git", pathText(repository)}, temporary);
    auto commit = run({"rev-parse", "HEAD"}, repository);
    while (!commit.empty() && (commit.back() == '\n' || commit.back() == '\r')) commit.pop_back();
    if (commit.size() != 40 || !std::all_of(commit.begin(), commit.end(), [](char c) { return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'); }))
        throw std::runtime_error{"Git did not return an immutable policy commit."};
    Contracts::PolicySourceBundle bundle{"https://github.com/" + name, commit, {}, {}};
    std::istringstream tree{run({"ls-tree", "-r", "--full-tree", commit}, repository)};
    std::string line;
    std::size_t bytes{};
    while (std::getline(tree, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        if (tab == std::string::npos) throw std::runtime_error{"Git returned an invalid policy tree."};
        std::istringstream metadata{line.substr(0, tab)};
        std::string mode, type, blob;
        metadata >> mode >> type >> blob;
        if (type != "blob" || mode == "120000" || blob.size() != 40 ||
            !std::all_of(blob.begin(), blob.end(), [](char c) { return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'); }))
            throw std::runtime_error{"Policy import rejects submodules, links and invalid blob identities."};
        auto path = line.substr(tab + 1);
        if (path.starts_with('"')) path = Json::parse(path).get<std::string>();
        if (path.empty() || path.find_first_of("\r\n\t\\:") != std::string::npos || path.starts_with('/') || path.find("../") != std::string::npos)
            throw std::runtime_error{"Git returned an unsupported policy path."};
        if (!supported(path)) {
            if (bundle.excludedFiles.size() >= MaximumFiles) throw std::runtime_error{"Too many excluded policy source files."};
            bundle.excludedFiles.push_back(path); continue;
        }
        append(bundle, path, run({"cat-file", "blob", blob}, repository), bytes);
    }
    return bundle;
}

Contracts::PolicySourceBundle github(const std::string& source, const Domain::OperationContext& context)
{
    auto name = source.substr(std::string{"https://github.com/"}.size());
    if (name.ends_with('/')) name.pop_back();
    if (name.ends_with(".git")) name.resize(name.size() - 4);
    const auto slash = name.find('/');
    if (slash == std::string::npos || slash == 0 || slash == name.size() - 1 ||
        name.find('/', slash + 1) != std::string::npos || name.find("..") != std::string::npos ||
        !std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/';
        })) throw std::runtime_error{"Enter the repository URL as https://github.com/owner/repository."};
    Json identity;
    try { identity = Json::parse(get(L"api.github.com", "/repos/" + name + "/commits/HEAD", MaximumBundleBytes, context)); }
    catch (const GitHubUnavailable&) { return authenticatedGit(name, context); }
    const auto sha = identity.at("sha").get<std::string>();
    if (sha.size() != 40 || !std::all_of(sha.begin(), sha.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::runtime_error{"GitHub did not return an immutable policy commit."};
    const auto tree = Json::parse(get(L"api.github.com", "/repos/" + name + "/git/trees/" + sha + "?recursive=1", MaximumBundleBytes, context));
    if (tree.value("truncated", true)) throw std::runtime_error{"GitHub returned an incomplete policy tree. Import a local checkout instead."};
    Contracts::PolicySourceBundle bundle{"https://github.com/" + name, sha, {}, {}};
    std::size_t bytes{};
    for (const auto& entry : tree.at("tree")) {
        check(context);
        const auto type = entry.at("type").get<std::string>();
        if (type == "tree") continue;
        const auto path = entry.at("path").get<std::string>();
        if (path.empty() || path.starts_with('/') || path.find('\\') != std::string::npos || path.find(':') != std::string::npos || path.find("../") != std::string::npos)
            throw std::runtime_error{"GitHub returned an invalid policy path."};
        if (type != "blob" || entry.value("mode", "") == "120000")
            throw std::runtime_error{"Policy snapshots cannot contain links or submodules: " + path};
        if (!supported(path)) {
            if (bundle.excludedFiles.size() >= MaximumFiles) throw std::runtime_error{"Too many non-text policy files to review."};
            bundle.excludedFiles.push_back(path);
            continue;
        }
        if (entry.at("size").get<std::uint64_t>() > MaximumFileBytes) throw std::runtime_error{"Policy file is too large: " + path};
        append(bundle, path, get(L"raw.githubusercontent.com", "/" + name + "/" + sha + "/" + encodedPath(path), MaximumFileBytes, context), bytes);
    }
    return bundle;
}
}

Domain::Result<Contracts::PolicySourceBundle> WindowsPolicySourceReader::read(
    const std::string& source, const Domain::OperationContext& context) noexcept
{
    try {
        Contracts::PolicySourceBundle bundle;
        if (source.starts_with("https://github.com/")) bundle = github(source, context);
        else {
            if (source.find("://") != std::string::npos) throw std::runtime_error{"Use a local folder or an HTTPS GitHub repository URL."};
            check(context);
            const auto root = std::filesystem::canonical(nativePath(source));
            if (!std::filesystem::is_directory(root)) throw std::runtime_error{"Choose an existing policy folder."};
            bundle.source = pathText(root);
            std::size_t bytes{};
            for (std::filesystem::recursive_directory_iterator iterator{root}, end; iterator != end; ++iterator) {
                check(context);
                if (iterator->path().filename() == L".git") { if (iterator->is_directory()) iterator.disable_recursion_pending(); continue; }
                const auto attributes = GetFileAttributesW(iterator->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    throw std::runtime_error{"Policy snapshots cannot include unreadable entries, links or reparse points."};
                if (!iterator->is_regular_file()) continue;
                const auto path = pathText(iterator->path().lexically_relative(root));
                if (!supported(path)) {
                    if (bundle.excludedFiles.size() >= MaximumFiles) throw std::runtime_error{"Too many non-text policy files to review."};
                    bundle.excludedFiles.push_back(path); continue;
                }
                const auto length = iterator->file_size();
                if (length > MaximumFileBytes) throw std::runtime_error{"Policy file is too large: " + path};
                std::ifstream file{iterator->path(), std::ios::binary};
                if (!file) throw std::runtime_error{"Cannot read policy file: " + path};
                std::string content(static_cast<std::size_t>(length), '\0');
                file.read(content.data(), static_cast<std::streamsize>(length));
                if (file.gcount() != static_cast<std::streamsize>(length) || file.peek() != std::char_traits<char>::eof())
                    throw std::runtime_error{"Policy file changed during import: " + path};
                append(bundle, path, std::move(content), bytes);
            }
        }
        if (bundle.files.empty()) throw std::runtime_error{"No supported policy text was found."};
        std::sort(bundle.files.begin(), bundle.files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
        std::sort(bundle.excludedFiles.begin(), bundle.excludedFiles.end());
        return Domain::Result<Contracts::PolicySourceBundle>::success(std::move(bundle));
    } catch (const std::exception& error) {
        return Domain::Result<Contracts::PolicySourceBundle>::failure(Domain::makeError(
            context.isCancellationRequested() ? Domain::ErrorCodes::Cancelled : Domain::ErrorCodes::InvalidRequest, error.what()));
    } catch (...) {
        return Domain::Result<Contracts::PolicySourceBundle>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Policy import failed without adoption."));
    }
}
} // namespace ForgeConductor::Infrastructure::Windows
