#include "ShellJobStorage.h"

#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>

namespace ForgeConductor::NativeTools::Windows::Detail {
namespace {
using Json = nlohmann::json;
namespace InfrastructureDetail = ForgeConductor::Infrastructure::Windows::Detail;
struct AdmissionFailure final { Domain::Error error; };

void checkAdmission(const Domain::OperationContext& context)
{
    auto valid = InfrastructureDetail::validateOperationContext(context,
        std::chrono::steady_clock::now(), "admit persisted process job");
    if (!valid) throw AdmissionFailure{std::move(valid).error()};
}

class AdmissionLock final {
public:
    explicit AdmissionLock(const std::filesystem::path& directory,
        const Domain::OperationContext* context = nullptr)
    {
        // File identity makes every path spelling of this project share one
        // admission lock. Keep the directory from being replaced while held.
        directory_.reset(::CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION identity{};
        if (!directory_ || !::GetFileInformationByHandle(directory_.get(), &identity) ||
            (identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            throw AdmissionFailure{Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                "The process job directory identity cannot be verified.")};
        const auto name = L"Global\\ForgeConductor.ProcessAdmission." +
            std::to_wstring(identity.dwVolumeSerialNumber) + L"." +
            std::to_wstring(identity.nFileIndexHigh) + L"." + std::to_wstring(identity.nFileIndexLow);
        mutex_.reset(::CreateMutexW(nullptr, FALSE, name.c_str()));
        if (!mutex_) throw AdmissionFailure{Domain::makeError(Domain::ErrorCodes::StorageFull,
            "The process job admission lock cannot be opened.")};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        for (;;) {
            if (context) checkAdmission(*context);
            else if (std::chrono::steady_clock::now() >= deadline)
                throw AdmissionFailure{Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,
                    "The process receipt publication lock timed out.", true)};
            const auto waited = ::WaitForSingleObject(mutex_.get(), 200U);
            if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) { acquired_ = true; break; }
            if (waited != WAIT_TIMEOUT) throw AdmissionFailure{Domain::makeError(Domain::ErrorCodes::StorageFull,
                "The process job admission lock cannot be acquired.")};
        }
    }
    ~AdmissionLock() { if (acquired_) static_cast<void>(::ReleaseMutex(mutex_.get())); }
    AdmissionLock(const AdmissionLock&) = delete;
    AdmissionLock& operator=(const AdmissionLock&) = delete;
private:
    InfrastructureDetail::UniqueHandle directory_;
    InfrastructureDetail::UniqueHandle mutex_;
    bool acquired_{};
};

[[nodiscard]] std::filesystem::path native(const std::string_view value)
{
    return std::filesystem::path{std::u8string{
        reinterpret_cast<const char8_t*>(value.data()), value.size()}};
}

[[nodiscard]] std::string utf8(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return std::string{reinterpret_cast<const char*>(value.data()), value.size()};
}

void rejectReparse(const std::filesystem::path& path)
{
    const auto attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        throw std::runtime_error{"A process evidence path cannot be a reparse point."};
    }
}

[[nodiscard]] std::string bytes(const std::filesystem::path& path, const std::size_t maximum)
{
    rejectReparse(path);
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"The process evidence file cannot be opened."};
    const auto size = std::filesystem::file_size(path);
    if (size > maximum) throw std::runtime_error{"The process evidence file exceeds its storage limit."};
    std::string result(static_cast<std::size_t>(size), '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (input.gcount() != static_cast<std::streamsize>(result.size())) {
        throw std::runtime_error{"The process evidence file could not be read completely."};
    }
    return result;
}

[[nodiscard]] std::string digest(const std::string_view value)
{
    Infrastructure::Windows::BCryptSha256Hasher hasher;
    auto hashed = hasher.sha256(std::as_bytes(std::span{value.data(), value.size()}));
    if (!hashed) throw std::runtime_error{hashed.error().message};
    return hashed.value().value();
}

[[nodiscard]] std::string logDigest(const std::filesystem::path& stdoutPath,
    const std::filesystem::path& stderrPath)
{
    const auto stdoutHash = digest(bytes(stdoutPath, ShellJobStorage::MaximumLogBytes));
    const auto stderrHash = digest(bytes(stderrPath, ShellJobStorage::MaximumLogBytes));
    return digest(stdoutHash + "\n" + stderrHash);
}

void atomicText(const std::filesystem::path& path, const std::string_view value)
{
    rejectReparse(path);
    auto temporary = path;
    temporary += L".tmp";
    rejectReparse(temporary);
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        output.flush();
        if (!output) throw std::runtime_error{"The process receipt could not be written."};
    }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error{"The process receipt could not be published atomically."};
    }
}

[[nodiscard]] Json receipt(const Domain::ProjectId& project, const Domain::ShellJobSnapshot& snapshot)
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    static_cast<void>(::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user));
    const auto ownerCreation = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) |
        creation.dwLowDateTime;
    Json value{{"job_host_pid", ::GetCurrentProcessId()}, {"job_host_creation_time", ownerCreation},
        {"schema_version", 1U}, {"project_id", project.value()}, {"job_id", snapshot.jobId},
        {"state", static_cast<int>(snapshot.state)}, {"command", snapshot.command}, {"cwd", snapshot.cwd},
        {"timeout_sec", snapshot.timeoutSeconds}, {"elapsed_ms", snapshot.elapsed.count()},
        {"pid", snapshot.processId}, {"pid_creation_time", snapshot.processCreationTime},
        {"stdout_path", snapshot.stdoutPath}, {"stderr_path", snapshot.stderrPath},
        {"receipt_path", snapshot.receiptPath}, {"log_hash", snapshot.logHash},
        {"log_truncated", snapshot.logTruncated}, {"arguments", snapshot.arguments},
        {"memory_attached", snapshot.memoryAttached}};
    if (snapshot.result) {
        const auto& result = *snapshot.result;
        value["result"] = Json{{"exit_code", result.exitCode}, {"stdout", result.stdoutUtf8},
            {"stderr", result.stderrUtf8}, {"timed_out", result.timedOut}, {"cancelled", result.cancelled},
            {"stdout_truncated", result.stdoutTruncated}, {"stderr_truncated", result.stderrTruncated},
            {"termination_confirmed", result.terminationConfirmed}, {"elapsed_ms", result.elapsed.count()}};
    }
    if (snapshot.error) {
        value["error"] = Json{{"code", snapshot.error->code}, {"message", snapshot.error->message},
            {"retryable", snapshot.error->retryable}};
    }
    if (snapshot.memoryAttachError) {
        const auto& error = *snapshot.memoryAttachError;
        value["memory_attach_error"] = Json{{"code", error.code}, {"message", error.message},
            {"retryable", error.retryable}};
    }
    return value;
}

void validateId(const std::string_view id)
{
    if (!Domain::Uuid::parse(id)) throw std::runtime_error{"The process job identifier is not a UUID."};
}

[[nodiscard]] std::filesystem::path directory(const Domain::PathText& root,
    const Domain::ProjectId& project)
{
    const auto base = native(root.value());
    rejectReparse(base);
    const auto projectPath = base / project.value();
    rejectReparse(projectPath);
    return projectPath;
}

[[nodiscard]] bool exactProcessAlive(const std::uint32_t pid, const std::uint64_t expectedCreation)
{
    if (pid == 0U) return false;
    const auto process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        if (::GetLastError() == ERROR_INVALID_PARAMETER) return false;
        throw std::runtime_error{"The persisted process identity cannot be inspected."};
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    const bool measured = ::GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
    const auto actualCreation = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) |
        creation.dwLowDateTime;
    const bool alive = ::WaitForSingleObject(process, 0U) == WAIT_TIMEOUT;
    ::CloseHandle(process);
    if (!measured || expectedCreation == 0U) {
        throw std::runtime_error{"The persisted process creation time cannot be verified."};
    }
    return alive && actualCreation == expectedCreation;
}

} // namespace

ShellJobStorage::ShellJobStorage(Domain::ProjectId project, Domain::ShellJobSnapshot initial,
    std::filesystem::path directory)
    : project_{std::move(project)}, initial_{std::move(initial)}, directory_{std::move(directory)},
      stdoutPath_{directory_ / (initial_.jobId + ".stdout.log")},
      stderrPath_{directory_ / (initial_.jobId + ".stderr.log")},
      receiptPath_{directory_ / (initial_.jobId + ".json")}
{
    rejectReparse(stdoutPath_); rejectReparse(stderrPath_); rejectReparse(receiptPath_);
    stdout_.open(stdoutPath_, std::ios::binary | std::ios::trunc);
    stderr_.open(stderrPath_, std::ios::binary | std::ios::trunc);
    if (!stdout_ || !stderr_) throw std::runtime_error{"The process log files cannot be created."};
}

Domain::Result<std::shared_ptr<ShellJobStorage>> ShellJobStorage::create(const Domain::PathText& root,
    const Domain::ProjectId& project, const Domain::ShellJobSnapshot& initial,
    const Domain::OperationContext& context)
{
    using Outcome = Domain::Result<std::shared_ptr<ShellJobStorage>>;
    try {
        checkAdmission(context);
        validateId(initial.jobId);
        const auto path = directory(root, project);
        std::filesystem::create_directories(path);
        rejectReparse(path);
        AdmissionLock admission{path, &context};
        if (std::filesystem::exists(path / (initial.jobId + ".json")))
            return Outcome::failure(Domain::makeError(Domain::ErrorCodes::Conflict,
                "The process job identifier already has a persisted receipt."));
        std::vector<std::filesystem::directory_entry> completed;
        std::size_t occupied{};
        for (const auto& entry : std::filesystem::directory_iterator{path}) {
            checkAdmission(context);
            if (entry.path().extension() != L".json") continue;
            bool evictable{};
            try {
                const auto id = utf8(entry.path().stem());
                validateId(id);
                auto recovered = load(root, project, id);
                evictable = recovered && recovered.value().state != Domain::ShellJobState::Running;
            } catch (...) {
                // An unverified receipt occupies a slot and is preserved.
            }
            if (evictable) completed.push_back(entry);
            else ++occupied;
        }
        std::sort(completed.begin(), completed.end(), [](const auto& left, const auto& right) {
            return left.last_write_time() < right.last_write_time();
        });
        while (!completed.empty() && completed.size() + occupied >= MaximumPersistedJobs) {
            checkAdmission(context);
            const auto id = utf8(completed.front().path().stem());
            for (const auto* suffix : {".json.sha256", ".stdout.log", ".stderr.log", ".json"}) {
                const auto stale = path / (id + suffix);
                rejectReparse(stale);
                std::error_code error;
                static_cast<void>(std::filesystem::remove(stale, error));
                if (error) throw AdmissionFailure{Domain::makeError(Domain::ErrorCodes::StorageFull,
                    "An expired process job could not be evicted; no new job was admitted.")};
            }
            completed.erase(completed.begin());
        }
        checkAdmission(context);
        if (occupied >= MaximumPersistedJobs)
            return Outcome::failure(Domain::makeError(Domain::ErrorCodes::RateLimited,
                "The 32 persisted process jobs per project limit has been reached across job owners. "
                "Poll or cancel existing jobs; unverified receipts also occupy slots.", true));
        auto storage = std::shared_ptr<ShellJobStorage>{new ShellJobStorage{project, initial, path}};
        auto snapshot = initial;
        storage->persist(snapshot);
        return Outcome::success(std::move(storage));
    } catch (AdmissionFailure& failure) {
        return Outcome::failure(std::move(failure.error));
    } catch (const std::exception& failure) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::StorageFull,
            std::string{"The process job evidence could not be admitted: "} + failure.what()));
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::StorageFull,
            "The process job evidence could not be admitted."));
    }
}

void ShellJobStorage::writeReceipt(const Domain::ShellJobSnapshot& snapshot)
{
    const auto payload = receipt(project_, snapshot);
    const auto encoded = Json{{"payload", payload}, {"sha256", digest(payload.dump())}}.dump();
    atomicText(receiptPath_, encoded);
    auto hashPath = receiptPath_;
    hashPath += L".sha256";
    atomicText(hashPath, digest(encoded));
    published_ = true;
}

void ShellJobStorage::onStarted(const std::uint32_t pid, const std::uint64_t created) noexcept
{
    processId_.store(pid); creationTime_.store(created);
    try {
        auto snapshot = initial_;
        persist(snapshot);
    } catch (...) {
        try { std::scoped_lock lock{mutex_}; writeFailed_ = true; } catch (...) {}
    }
    started_.notify_all();
}

void ShellJobStorage::onOutput(const std::string_view value, const bool stderrStream) noexcept
{
    try {
        std::scoped_lock lock{mutex_};
        auto& saved = stderrStream ? stderrBytes_ : stdoutBytes_;
        auto& output = stderrStream ? stderr_ : stdout_;
        const auto retained = (std::min)(value.size(), MaximumLogBytes - static_cast<std::size_t>(saved));
        output.write(value.data(), static_cast<std::streamsize>(retained)); output.flush();
        saved += retained;
        truncated_ = truncated_ || retained < value.size();
        writeFailed_ = writeFailed_ || !output;
    } catch (...) {
    }
}

void ShellJobStorage::captureFallback(const Domain::ProcessResult& result)
{
    bool missingStdout{}, missingStderr{};
    {
        std::scoped_lock lock{mutex_};
        missingStdout = stdoutBytes_ == 0U; missingStderr = stderrBytes_ == 0U;
    }
    if (missingStdout && !result.stdoutUtf8.empty()) onOutput(result.stdoutUtf8, false);
    if (missingStderr && !result.stderrUtf8.empty()) onOutput(result.stderrUtf8, true);
}

void ShellJobStorage::persist(Domain::ShellJobSnapshot& snapshot)
{
    std::scoped_lock lock{mutex_};
    AdmissionLock admission{directory_};
    if (published_ && !std::filesystem::exists(receiptPath_))
        throw std::runtime_error{"The process receipt was evicted and cannot be republished."};
    if (stdout_.is_open()) stdout_.flush();
    if (stderr_.is_open()) stderr_.flush();
    snapshot.processId = processId_.load(); snapshot.processCreationTime = creationTime_.load();
    snapshot.stdoutPath = utf8(stdoutPath_); snapshot.stderrPath = utf8(stderrPath_);
    snapshot.receiptPath = utf8(receiptPath_); snapshot.logTruncated = truncated_;
    if (snapshot.state != Domain::ShellJobState::Running) {
        if (stdout_.is_open()) { stdout_.close(); writeFailed_ = writeFailed_ || !stdout_; }
        if (stderr_.is_open()) { stderr_.close(); writeFailed_ = writeFailed_ || !stderr_; }
        snapshot.logHash = logDigest(stdoutPath_, stderrPath_);
        finished_ = true;
        started_.notify_all();
    }
    if (writeFailed_) {
        snapshot.error = Domain::makeError(Domain::ErrorCodes::StorageFull,
            "The process evidence log could not be written completely.");
    }
    writeReceipt(snapshot);
}

std::optional<bool> ShellJobStorage::observeProcessAlive(
    const std::uint32_t processId, const std::uint64_t expectedCreation) noexcept
{
    if (processId == 0U || expectedCreation == 0U) return std::nullopt;
    const auto process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId);
    if (!process) {
        return ::GetLastError() == ERROR_INVALID_PARAMETER
            ? std::optional<bool>{false} : std::nullopt;
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    const bool measured = ::GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
    const auto actualCreation = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) |
        creation.dwLowDateTime;
    const auto wait = ::WaitForSingleObject(process, 0U);
    ::CloseHandle(process);
    if (!measured) return std::nullopt;
    if (actualCreation != expectedCreation) return false;
    if (wait == WAIT_OBJECT_0) return false;
    if (wait == WAIT_TIMEOUT) return true;
    return std::nullopt;
}

void ShellJobStorage::waitForStarted(const std::chrono::milliseconds timeout)
{
    std::unique_lock lock{mutex_};
    static_cast<void>(started_.wait_for(lock, timeout, [this] { return processId_.load() != 0U || finished_; }));
}

Domain::Result<Domain::ShellJobSnapshot> ShellJobStorage::load(const Domain::PathText& root,
    const Domain::ProjectId& project, const std::string_view id)
{
    using Outcome = Domain::Result<Domain::ShellJobSnapshot>;
    try {
        validateId(id);
        const auto path = directory(root, project) / (std::string{id} + ".json");
        if (!std::filesystem::exists(path)) return Outcome::failure(Domain::makeError(
            Domain::ErrorCodes::RecordNotFound, "The retained process receipt was not found."));
        const auto encoded = bytes(path, 1U * 1024U * 1024U);
        const auto envelope = Json::parse(encoded);
        const auto& value = envelope.at("payload");
        if (digest(value.dump()) != envelope.at("sha256").get<std::string>()) {
            throw std::runtime_error{"Process receipt hash mismatch."};
        }
        if (value.at("schema_version") != 1U || value.at("project_id") != project.value() ||
            value.at("job_id").get<std::string>() != id) throw std::runtime_error{"Process receipt ownership mismatch."};
        const auto state = value.at("state").get<int>();
        if (state < 0 || state > static_cast<int>(Domain::ShellJobState::TimedOut)) {
            throw std::runtime_error{"Process receipt state is invalid."};
        }
        Domain::ShellJobSnapshot snapshot;
        snapshot.jobId = id; snapshot.state = static_cast<Domain::ShellJobState>(state);
        snapshot.command = value.at("command").get<std::string>(); snapshot.cwd = value.at("cwd").get<std::string>();
        snapshot.timeoutSeconds = value.at("timeout_sec").get<std::uint32_t>();
        snapshot.elapsed = std::chrono::milliseconds{value.at("elapsed_ms").get<std::int64_t>()};
        snapshot.processId = value.at("pid").get<std::uint32_t>();
        snapshot.processCreationTime = value.at("pid_creation_time").get<std::uint64_t>();
        snapshot.stdoutPath = value.at("stdout_path").get<std::string>();
        snapshot.stderrPath = value.at("stderr_path").get<std::string>();
        snapshot.receiptPath = value.at("receipt_path").get<std::string>();
        snapshot.logHash = value.at("log_hash").get<std::string>(); snapshot.logTruncated = value.at("log_truncated").get<bool>();
        snapshot.arguments = value.value("arguments", std::vector<std::string>{});
        snapshot.memoryAttached = value.value("memory_attached", false);
        if (value.contains("memory_attach_error")) {
            const auto& error = value.at("memory_attach_error");
            snapshot.memoryAttachError = Domain::makeError(error.at("code").get<std::string>(),
                error.at("message").get<std::string>(), error.at("retryable").get<bool>());
        }
        const auto folder = directory(root, project);
        if (snapshot.stdoutPath != utf8(folder / (std::string{id} + ".stdout.log")) ||
            snapshot.stderrPath != utf8(folder / (std::string{id} + ".stderr.log")) ||
            snapshot.receiptPath != utf8(path)) throw std::runtime_error{"Process receipt evidence path mismatch."};
        if (value.contains("result")) {
            const auto& result = value.at("result");
            snapshot.result = Domain::ProcessResult{result.at("exit_code").get<std::int32_t>(),
                result.at("stdout").get<std::string>(), result.at("stderr").get<std::string>(),
                result.at("timed_out").get<bool>(), result.at("cancelled").get<bool>(),
                result.at("stdout_truncated").get<bool>(), result.at("stderr_truncated").get<bool>(),
                result.at("termination_confirmed").get<bool>(),
                std::chrono::milliseconds{result.at("elapsed_ms").get<std::int64_t>()}};
        }
        if (value.contains("error")) {
            const auto& error = value.at("error");
            snapshot.error = Domain::makeError(error.at("code").get<std::string>(),
                error.at("message").get<std::string>(), error.at("retryable").get<bool>());
        }
        if (snapshot.state == Domain::ShellJobState::Running) {
            if (exactProcessAlive(value.at("job_host_pid").get<std::uint32_t>(),
                    value.at("job_host_creation_time").get<std::uint64_t>()) ||
                exactProcessAlive(snapshot.processId, snapshot.processCreationTime)) {
                return Outcome::failure(Domain::makeError(Domain::ErrorCodes::OwnershipConflict,
                    "The live process is owned by another job host. Use that host to poll or cancel it."));
            }
            snapshot.state = Domain::ShellJobState::Failed;
            snapshot.error = Domain::makeError(Domain::ErrorCodes::ProcessTerminationUnconfirmed,
                "The job host ended without a final process receipt. The exit code is unknown.");
            snapshot.logHash = logDigest(native(snapshot.stdoutPath), native(snapshot.stderrPath));
        } else if (snapshot.logHash != logDigest(native(snapshot.stdoutPath), native(snapshot.stderrPath))) {
            throw std::runtime_error{"Process evidence log hash mismatch."};
        }
        snapshot.processAlive = observeProcessAlive(snapshot.processId, snapshot.processCreationTime);
        return Outcome::success(std::move(snapshot));
    } catch (const std::exception& failure) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure, failure.what()));
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
            "The persisted process receipt could not be verified."));
    }
}

Domain::Result<std::vector<Domain::ShellJobSnapshot>> ShellJobStorage::list(
    const Domain::PathText& root, const Domain::ProjectId& project)
{
    using Outcome = Domain::Result<std::vector<Domain::ShellJobSnapshot>>;
    try {
        const auto path = directory(root, project);
        std::vector<Domain::ShellJobSnapshot> results;
        if (!std::filesystem::exists(path)) return Outcome::success(std::move(results));
        std::vector<std::filesystem::directory_entry> receipts;
        for (const auto& entry : std::filesystem::directory_iterator{path}) {
            if (entry.path().extension() == L".json") receipts.push_back(entry);
        }
        std::sort(receipts.begin(), receipts.end(), [](const auto& left, const auto& right) {
            return left.last_write_time() > right.last_write_time();
        });
        for (const auto& entry : receipts) {
            auto loaded = load(root, project, utf8(entry.path().stem()));
            if (loaded) results.push_back(std::move(loaded).value());
            else if (loaded.error().code != Domain::ErrorCodes::OwnershipConflict) {
                return Outcome::failure(std::move(loaded).error());
            }
            if (results.size() >= MaximumPersistedJobs) break;
        }
        return Outcome::success(std::move(results));
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
            "The persisted process receipts could not be listed."));
    }
}

Domain::Result<Domain::ShellJobLogPage> ShellJobStorage::read(const Domain::ShellJobSnapshot& snapshot,
    const bool stderrStream, const std::optional<std::uint64_t> offset, const std::size_t tailLines)
{
    using Outcome = Domain::Result<Domain::ShellJobLogPage>;
    try {
        constexpr std::size_t MaximumPageBytes = 32U * 1024U;
        const auto& path = stderrStream ? snapshot.stderrPath : snapshot.stdoutPath;
        if (path.empty()) return Outcome::failure(Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
            "This process has no durable log storage."));
        const auto value = bytes(native(path), MaximumLogBytes);
        std::size_t start{};
        if (offset) start = static_cast<std::size_t>((std::min)(*offset, static_cast<std::uint64_t>(value.size())));
        else {
            const auto count = (std::min)(tailLines == 0U ? 100U : tailLines, std::size_t{1'000});
            start = value.size();
            auto lines = std::size_t{};
            if (start > 0U && value[start - 1U] == '\n') --start;
            while (start > 0U) {
                --start;
                if (value[start] == '\n' && ++lines >= count) { ++start; break; }
            }
            if (value.size() - start > MaximumPageBytes) start = value.size() - MaximumPageBytes;
        }
        const auto unalignedStart = start;
        while (start < value.size() && (static_cast<unsigned char>(value[start]) & 0xc0U) == 0x80U) ++start;
        auto end = (std::min)(value.size(), start + MaximumPageBytes);
        while (end > start && end < value.size() && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) --end;
        if (end == start && start < value.size()) {
            // Malformed continuation runs have no character boundary in this
            // page. Consume a bounded raw window so nextOffset still advances.
            end = (std::min)(value.size(), start + MaximumPageBytes);
        }
        auto text = value.substr(start, end - start);
        bool textLossy = start != unalignedStart;
        if (!Domain::isValidUtf8(text)) {
            textLossy = true;
            for (auto& character : text) if (static_cast<unsigned char>(character) >= 0x80U) character = '?';
        }
        for (auto& character : text) {
            if (character == '\0') {
                character = '?';
                textLossy = true;
            }
        }
        return Outcome::success({std::move(text), path, start, end, value.size(), end < value.size(), textLossy});
    } catch (...) {
        return Outcome::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
            "The process evidence log could not be read."));
    }
}

} // namespace ForgeConductor::NativeTools::Windows::Detail
