#include "ForgeConductor/NativeTools/Windows/WindowsTextSearchService.h"

#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "NativeFileOperations.h"

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {
namespace {

namespace InfrastructureDetail =
    ForgeConductor::Infrastructure::Windows::Detail;

constexpr std::size_t MaximumQueryBytes = 4U * 1024U;
constexpr std::size_t MaximumScannedBytes = 64U * 1024U * 1024U;

[[nodiscard]] std::string_view
byteText(const std::vector<std::byte> &bytes) noexcept {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

[[nodiscard]] Domain::Result<bool>
appendFileMatches(const std::string_view path, const std::string_view content,
                  const std::regex &expression,
                  const std::size_t maximumMatches,
                  const std::size_t maximumResponseBytes,
                  std::size_t &responseBytes, std::vector<std::string> &matches,
                  const Domain::OperationContext &context) noexcept {
  try {
    std::size_t lineNumber{1U};
    std::size_t start{};
    while (start <= content.size()) {
      auto valid = InfrastructureDetail::validateOperationContext(
          context, std::chrono::steady_clock::now(),
          "scan a bounded UTF-8 text file");
      if (!valid) {
        return Domain::Result<bool>::failure(std::move(valid).error());
      }
      const auto newline = content.find('\n', start);
      const auto end =
          newline == std::string_view::npos ? content.size() : newline;
      auto line = content.substr(start, end - start);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1U);
      }
      if (std::regex_search(line.begin(), line.end(), expression)) {
        std::string match;
        match.reserve(path.size() + line.size() + 32U);
        match.append(path);
        match.push_back(':');
        match.append(std::to_string(lineNumber));
        match.push_back(':');
        match.append(line);
        if (match.size() > maximumResponseBytes - responseBytes) {
          return Domain::Result<bool>::success(true);
        }
        responseBytes += match.size();
        matches.push_back(std::move(match));
        if (matches.size() >= maximumMatches) {
          return Domain::Result<bool>::success(true);
        }
      }
      if (newline == std::string_view::npos) {
        break;
      }
      start = newline + 1U;
      ++lineNumber;
    }
    return Domain::Result<bool>::success(false);
  } catch (...) {
    return Domain::Result<bool>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "A native text-search result could not be materialized."));
  }
}

} // namespace

Domain::Result<std::vector<std::string>> WindowsTextSearchService::search(
    const Contracts::AuthorizedPath &root, const std::string_view query,
    const std::size_t maximumMatches, const std::size_t maximumResponseBytes,
    const Domain::OperationContext &context) noexcept {
  try {
    if (query.empty() || query.size() > MaximumQueryBytes ||
        query.find('\0') != std::string_view::npos) {
      return Domain::Result<std::vector<std::string>>::failure(
          Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                            "A text-search query must contain 1 through 4096 "
                            "UTF-8 bytes and no NUL."));
    }
    if (maximumMatches == 0U || maximumMatches > MaximumMatches ||
        maximumResponseBytes == 0U ||
        maximumResponseBytes > MaximumResponseBytes) {
      return Domain::Result<std::vector<std::string>>::failure(
          Domain::makeError(
              Domain::ErrorCodes::LimitExceeded,
              "Text-search result bounds exceed the native tool limits."));
    }
    auto validQuery = Detail::validateUtf8Text(query, "The text-search query");
    if (!validQuery) {
      return Domain::Result<std::vector<std::string>>::failure(
          std::move(validQuery).error());
    }

    std::regex expression;
    try {
      expression.assign(query.begin(), query.end(),
                        std::regex_constants::basic |
                            std::regex_constants::optimize);
    } catch (const std::regex_error &) {
      return Domain::Result<std::vector<std::string>>::failure(
          Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                            "The text-search pattern is not a valid POSIX "
                            "basic regular expression."));
    }

    std::vector<std::string> matches;
    matches.reserve(maximumMatches);
    std::size_t responseBytes{};
    std::size_t scannedBytes{};
    const Detail::NativeWalkOptions options{100'000U, 128U, true};
    const auto visitor = [&](const HANDLE parentDirectory,
                             const Detail::NativeDirectoryEntry &entry,
                             const std::wstring_view canonicalPath,
                             const std::wstring_view, const std::size_t,
                             const Domain::OperationContext &operationContext)
        -> Domain::Result<bool> {
      if (entry.isDirectory()) {
        return Domain::Result<bool>::success(false);
      }
      auto opened = Detail::openChildObject(
          parentDirectory, entry.name, canonicalPath,
          FILE_READ_DATA | FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE,
          operationContext);
      if (!opened) {
        return Domain::Result<bool>::failure(std::move(opened).error());
      }
      LARGE_INTEGER size{};
      if (::GetFileSizeEx(opened.value().handle.get(), &size) == FALSE) {
        return Domain::Result<bool>::failure(Detail::nativeFileError(
            "Size a text-search file", ::GetLastError()));
      }
      if (size.QuadPart < 0) {
        return Domain::Result<bool>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                              "A text-search file reported a negative size."));
      }
      const auto fileBytes = static_cast<std::uint64_t>(size.QuadPart);
      if (fileBytes > MaximumFileBytes) {
        return Domain::Result<bool>::success(false);
      }
      if (fileBytes > MaximumScannedBytes - scannedBytes) {
        return Domain::Result<bool>::failure(Domain::makeError(
            Domain::ErrorCodes::LimitExceeded,
            "Text search exceeded its 64 MiB scanned-byte bound."));
      }
      scannedBytes += static_cast<std::size_t>(fileBytes);
      auto content = Detail::readOpenedFile(opened.value().handle.get(),
                                            MaximumFileBytes, operationContext);
      if (!content) {
        return Domain::Result<bool>::failure(std::move(content).error());
      }
      const auto text = byteText(content.value());
      auto validText = Detail::validateUtf8Text(text, "A text-search file");
      if (!validText) {
        if (validText.error().code == Domain::ErrorCodes::InvalidRequest) {
          return Domain::Result<bool>::success(false);
        }
        return Domain::Result<bool>::failure(std::move(validText).error());
      }
      auto utf8Path = InfrastructureDetail::strictUtf16ToUtf8(canonicalPath);
      if (!utf8Path) {
        return Domain::Result<bool>::failure(std::move(utf8Path).error());
      }
      return appendFileMatches(utf8Path.value(), text, expression,
                               maximumMatches, maximumResponseBytes,
                               responseBytes, matches, operationContext);
    };
    auto walked =
        Detail::walkAuthorizedDirectory(root, options, visitor, context);
    if (!walked) {
      return Domain::Result<std::vector<std::string>>::failure(
          std::move(walked).error());
    }
    return Domain::Result<std::vector<std::string>>::success(
        std::move(matches));
  } catch (...) {
    return Domain::Result<std::vector<std::string>>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The bounded native text search failed."));
  }
}

} // namespace ForgeConductor::NativeTools::Windows
