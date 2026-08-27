#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "BoundaryFakeSupport.h"
#include "DeterministicResult.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

enum class FileSystemCall {
    ReadFile,
    WriteFile,
    List,
    CreateDirectory,
    Remove,
    Move
};

struct FileSystemCapture final {
    FileSystemCall call;
    Contracts::AuthorizedPath primary;
    std::optional<Contracts::AuthorizedPath> secondary;
    std::size_t requestedBound{};
    std::size_t requestedBytes{};
    std::vector<std::byte> capturedContent;
    bool recursive{};
};

class RecordingFileSystemFake final
    : public Contracts::IFileSystem {
public:
    explicit RecordingFileSystemFake(
        const std::size_t captureBytesMaximum =
            DefaultBoundaryCaptureBytesMaximum,
        const std::size_t captureItemsMaximum =
            DefaultBoundaryCaptureItemsMaximum) noexcept
        : captureBytesMaximum_{captureBytesMaximum},
          captureItemsMaximum_{captureItemsMaximum}
    {
    }

    DeterministicResult<std::vector<std::byte>> readFileResult;
    DeterministicResult<void> writeFileResult;
    DeterministicResult<std::vector<Domain::PathText>> listResult;
    bool listResultTruncated{};
    DeterministicResult<void> createDirectoryResult;
    DeterministicResult<void> removeResult;
    DeterministicResult<void> moveResult;

    [[nodiscard]] Domain::Result<std::vector<std::byte>> readFile(
        const Contracts::AuthorizedPath& path,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    std::move(gate).error());
            }
            capture(FileSystemCall::ReadFile, path, std::nullopt, maximumBytes);
            auto result = readFileResult.get();
            if (result &&
                (result.value().size() > maximumBytes ||
                 result.value().size() > captureBytesMaximum_)) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    boundaryPayloadTooLarge(
                        "The scripted file read exceeds its output bound."));
            }
            return result;
        } catch (...) {
            return byteFailure("The deterministic file read could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<void> writeFile(
        const Contracts::AuthorizedPath& path,
        const std::span<const std::byte> content,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return gate;
            }
            const auto capturedCount =
                (std::min)(content.size(), captureBytesMaximum_);
            capture(
                FileSystemCall::WriteFile,
                path,
                std::nullopt,
                captureBytesMaximum_,
                content.size(),
                std::vector<std::byte>{
                    content.begin(),
                    content.begin() + capturedCount});
            if (content.size() > captureBytesMaximum_) {
                return Domain::Result<void>::failure(boundaryPayloadTooLarge(
                    "The file write exceeds the capture bound."));
            }
            if (nextWriteFailure_) {
                auto error = std::move(*nextWriteFailure_);
                nextWriteFailure_.reset();
                return Domain::Result<void>::failure(std::move(error));
            }
            return writeFileResult.get();
        } catch (...) {
            return voidFailure("The deterministic file write could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::DirectoryListing> list(
        const Contracts::AuthorizedPath& directory,
        const std::size_t maximumEntries,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::DirectoryListing>::failure(
                    std::move(gate).error());
            }
            capture(FileSystemCall::List, directory, std::nullopt, maximumEntries);
            auto result = listResult.get();
            if (!result) {
                return Domain::Result<Domain::DirectoryListing>::failure(
                    std::move(result).error());
            }
            if (result.value().size() > captureItemsMaximum_) {
                return Domain::Result<Domain::DirectoryListing>::failure(
                    boundaryLimitExceeded(
                        "The scripted directory listing exceeds its entry bound."));
            }
            auto entries = std::move(result).value();
            const bool truncated =
                listResultTruncated || entries.size() > maximumEntries;
            if (entries.size() > maximumEntries) {
                entries.erase(
                    entries.begin() + static_cast<std::ptrdiff_t>(maximumEntries),
                    entries.end());
            }
            return Domain::Result<Domain::DirectoryListing>::success(
                Domain::DirectoryListing{std::move(entries), truncated});
        } catch (...) {
            return Domain::Result<Domain::DirectoryListing>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The deterministic directory listing could not be captured."));
        }
    }

    [[nodiscard]] Domain::Result<void> createDirectory(
        const Contracts::AuthorizedPath& directory,
        const Domain::OperationContext& context) noexcept override
    {
        return simpleCall(
            FileSystemCall::CreateDirectory,
            directory,
            std::nullopt,
            false,
            createDirectoryResult,
            context);
    }

    [[nodiscard]] Domain::Result<void> remove(
        const Contracts::AuthorizedPath& path,
        const bool recursive,
        const Domain::OperationContext& context) noexcept override
    {
        return simpleCall(
            FileSystemCall::Remove,
            path,
            std::nullopt,
            recursive,
            removeResult,
            context);
    }

    [[nodiscard]] Domain::Result<void> move(
        const Contracts::AuthorizedPath& source,
        const Contracts::AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept override
    {
        return simpleCall(
            FileSystemCall::Move,
            source,
            std::optional<Contracts::AuthorizedPath>{destination},
            false,
            moveResult,
            context);
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }
    [[nodiscard]] std::size_t captureBytesMaximum() const noexcept
    {
        return captureBytesMaximum_;
    }
    [[nodiscard]] std::size_t captureItemsMaximum() const noexcept
    {
        return captureItemsMaximum_;
    }

    [[nodiscard]] const std::optional<FileSystemCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

    void failNextWrite(Domain::Error error)
    {
        nextWriteFailure_.emplace(std::move(error));
    }

private:
    void capture(
        const FileSystemCall call,
        const Contracts::AuthorizedPath& primary,
        std::optional<Contracts::AuthorizedPath> secondary,
        const std::size_t requestedBound = 0,
        const std::size_t requestedBytes = 0,
        std::vector<std::byte> capturedContent = {},
        const bool recursive = false)
    {
        lastCapture_.emplace(FileSystemCapture{
            call,
            primary,
            std::move(secondary),
            requestedBound,
            requestedBytes,
            std::move(capturedContent),
            recursive});
    }

    [[nodiscard]] Domain::Result<void> simpleCall(
        const FileSystemCall call,
        const Contracts::AuthorizedPath& primary,
        std::optional<Contracts::AuthorizedPath> secondary,
        const bool recursive,
        const DeterministicResult<void>& scripted,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return gate;
            }
            capture(
                call,
                primary,
                std::move(secondary),
                0,
                0,
                {},
                recursive);
            return scripted.get();
        } catch (...) {
            return voidFailure(
                "The deterministic filesystem operation could not be captured.");
        }
    }

    [[nodiscard]] static Domain::Result<std::vector<std::byte>> byteFailure(
        const char* message)
    {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, message));
    }

    [[nodiscard]] static Domain::Result<void> voidFailure(
        const char* message)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, message));
    }

    DeterministicBoundaryState state_;
    std::size_t captureBytesMaximum_;
    std::size_t captureItemsMaximum_;
    std::optional<FileSystemCapture> lastCapture_;
    std::optional<Domain::Error> nextWriteFailure_;
};

} // namespace ForgeConductor::Tests::Fakes
