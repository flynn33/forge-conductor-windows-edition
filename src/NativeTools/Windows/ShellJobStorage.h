#pragma once

#include "ForgeConductor/Domain/ShellJobModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Identifiers.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace ForgeConductor::NativeTools::Windows::Detail {

class ShellJobStorage final : public Domain::IProcessOutputObserver {
public:
    static constexpr std::size_t MaximumLogBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t MaximumPersistedJobs = 32U;
    [[nodiscard]] static Domain::Result<std::shared_ptr<ShellJobStorage>> create(
        const Domain::PathText& root, const Domain::ProjectId& project,
        const Domain::ShellJobSnapshot& initial, const Domain::OperationContext& context);
    void onStarted(std::uint32_t processId, std::uint64_t creationTime) noexcept override;
    void onOutput(std::string_view bytes, bool stderrStream) noexcept override;
    void persist(Domain::ShellJobSnapshot& snapshot);
    void captureFallback(const Domain::ProcessResult& result);
    [[nodiscard]] std::uint32_t processId() const noexcept { return processId_.load(); }
    [[nodiscard]] std::uint64_t creationTime() const noexcept { return creationTime_.load(); }
    void waitForStarted(std::chrono::milliseconds timeout);
    [[nodiscard]] static std::optional<bool> observeProcessAlive(
        std::uint32_t processId, std::uint64_t creationTime) noexcept;
    [[nodiscard]] static Domain::Result<Domain::ShellJobSnapshot> load(
        const Domain::PathText& root, const Domain::ProjectId& project, std::string_view id);
    [[nodiscard]] static Domain::Result<std::vector<Domain::ShellJobSnapshot>> list(
        const Domain::PathText& root, const Domain::ProjectId& project);
    [[nodiscard]] static Domain::Result<Domain::ShellJobLogPage> read(
        const Domain::ShellJobSnapshot& snapshot, bool stderrStream,
        std::optional<std::uint64_t> offset, std::size_t tailLines);

private:
    ShellJobStorage(Domain::ProjectId project, Domain::ShellJobSnapshot initial,
        std::filesystem::path directory);
    void writeReceipt(const Domain::ShellJobSnapshot& snapshot);
    const Domain::ProjectId project_;
    const Domain::ShellJobSnapshot initial_;
    const std::filesystem::path directory_;
    const std::filesystem::path stdoutPath_;
    const std::filesystem::path stderrPath_;
    const std::filesystem::path receiptPath_;
    std::mutex mutex_;
    std::condition_variable started_;
    std::ofstream stdout_;
    std::ofstream stderr_;
    std::uint64_t stdoutBytes_{};
    std::uint64_t stderrBytes_{};
    bool truncated_{};
    bool writeFailed_{};
    bool finished_{};
    bool published_{};
    std::atomic<std::uint32_t> processId_{};
    std::atomic<std::uint64_t> creationTime_{};
};

} // namespace ForgeConductor::NativeTools::Windows::Detail
