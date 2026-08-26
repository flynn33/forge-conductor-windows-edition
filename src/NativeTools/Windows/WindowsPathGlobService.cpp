#include "ForgeConductor/NativeTools/Windows/WindowsPathGlobService.h"

#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"
#include "NativeFileOperations.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {
namespace {

namespace InfrastructureDetail =
    ForgeConductor::Infrastructure::Windows::Detail;

struct GlobPattern final {
  std::wstring storage;
  std::vector<std::wstring> segments;
};

[[nodiscard]] bool equalCharacter(const wchar_t left,
                                  const wchar_t right) noexcept {
  return ::CompareStringOrdinal(&left, 1, &right, 1, TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool matchSegment(const std::wstring_view pattern,
                                const std::wstring_view value) {
  std::vector<std::uint8_t> previous(value.size() + 1U, 0U);
  std::vector<std::uint8_t> current(value.size() + 1U, 0U);
  previous[0] = 1U;
  for (const wchar_t token : pattern) {
    std::fill(current.begin(), current.end(), std::uint8_t{0U});
    if (token == L'*') {
      current[0] = previous[0];
      for (std::size_t index = 1U; index <= value.size(); ++index) {
        current[index] = static_cast<std::uint8_t>(previous[index] != 0U ||
                                                   current[index - 1U] != 0U);
      }
    } else {
      for (std::size_t index = 1U; index <= value.size(); ++index) {
        current[index] = static_cast<std::uint8_t>(
            previous[index - 1U] != 0U &&
            (token == L'?' || equalCharacter(token, value[index - 1U])));
      }
    }
    previous.swap(current);
  }
  return previous[value.size()] != 0U;
}

[[nodiscard]] std::vector<std::wstring_view>
splitPath(const std::wstring_view path) {
  std::vector<std::wstring_view> segments;
  std::size_t start{};
  while (start < path.size()) {
    const auto separator = path.find(L'\\', start);
    const auto end =
        separator == std::wstring_view::npos ? path.size() : separator;
    segments.push_back(path.substr(start, end - start));
    if (separator == std::wstring_view::npos) {
      break;
    }
    start = separator + 1U;
  }
  return segments;
}

[[nodiscard]] bool matchPath(const std::vector<std::wstring> &pattern,
                             const std::vector<std::wstring_view> &path) {
  const std::size_t columns = path.size() + 1U;
  std::vector<std::uint8_t> table((pattern.size() + 1U) * columns, 0U);
  const auto cell = [&](const std::size_t patternIndex,
                        const std::size_t pathIndex) -> std::uint8_t & {
    return table[patternIndex * columns + pathIndex];
  };
  cell(0U, 0U) = 1U;
  for (std::size_t patternIndex = 1U; patternIndex <= pattern.size();
       ++patternIndex) {
    if (pattern[patternIndex - 1U] == L"**") {
      cell(patternIndex, 0U) = cell(patternIndex - 1U, 0U);
      for (std::size_t pathIndex = 1U; pathIndex <= path.size(); ++pathIndex) {
        cell(patternIndex, pathIndex) = static_cast<std::uint8_t>(
            cell(patternIndex - 1U, pathIndex) != 0U ||
            cell(patternIndex, pathIndex - 1U) != 0U);
      }
    } else {
      for (std::size_t pathIndex = 1U; pathIndex <= path.size(); ++pathIndex) {
        cell(patternIndex, pathIndex) = static_cast<std::uint8_t>(
            cell(patternIndex - 1U, pathIndex - 1U) != 0U &&
            matchSegment(pattern[patternIndex - 1U], path[pathIndex - 1U]));
      }
    }
  }
  return cell(pattern.size(), path.size()) != 0U;
}

[[nodiscard]] Domain::Result<GlobPattern>
parsePattern(const std::string_view pattern) noexcept {
  try {
    if (pattern.empty() || pattern.size() > 4U * 1024U ||
        pattern.find('\0') != std::string_view::npos) {
      return Domain::Result<GlobPattern>::failure(
          Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                            "A glob pattern must contain 1 through 4096 UTF-8 "
                            "bytes and no NUL."));
    }
    auto converted = InfrastructureDetail::strictUtf8ToUtf16(pattern);
    if (!converted) {
      return Domain::Result<GlobPattern>::failure(std::move(converted).error());
    }
    GlobPattern parsed{std::move(converted).value(), {}};
    for (auto &character : parsed.storage) {
      if (character == L'/') {
        character = L'\\';
      }
    }
    if (parsed.storage.empty() || parsed.storage.front() == L'\\' ||
        parsed.storage.back() == L'\\' ||
        parsed.storage.find(L':') != std::wstring::npos) {
      return Domain::Result<GlobPattern>::failure(Domain::makeError(
          Domain::ErrorCodes::PathOutsideAuthority,
          "Glob patterns must be relative to their authorized root."));
    }
    std::size_t start{};
    while (start < parsed.storage.size()) {
      const auto separator = parsed.storage.find(L'\\', start);
      const auto end =
          separator == std::wstring::npos ? parsed.storage.size() : separator;
      const auto segment =
          std::wstring_view{parsed.storage}.substr(start, end - start);
      if (segment.empty() || segment == L"." || segment == L".." ||
          (segment.find(L"**") != std::wstring_view::npos &&
           segment != L"**") ||
          std::any_of(segment.begin(), segment.end(),
                      [](const wchar_t value) noexcept {
                        return value < 0x20 || value == L'<' || value == L'>' ||
                               value == L'"' || value == L'|';
                      })) {
        return Domain::Result<GlobPattern>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The glob pattern contains an unsafe or ambiguous segment."));
      }
      parsed.segments.emplace_back(segment);
      if (separator == std::wstring::npos) {
        break;
      }
      start = separator + 1U;
    }
    return Domain::Result<GlobPattern>::success(std::move(parsed));
  } catch (...) {
    return Domain::Result<GlobPattern>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The glob pattern could not be parsed."));
  }
}

} // namespace

Domain::Result<std::vector<Domain::PathText>> WindowsPathGlobService::glob(
    const Contracts::AuthorizedPath &root, const std::string_view pattern,
    const std::size_t maximumMatches, const std::size_t maximumResponseBytes,
    const Domain::OperationContext &context) noexcept {
  try {
    if (maximumMatches == 0U || maximumMatches > MaximumMatches ||
        maximumResponseBytes == 0U ||
        maximumResponseBytes > MaximumResponseBytes) {
      return Domain::Result<std::vector<Domain::PathText>>::failure(
          Domain::makeError(
              Domain::ErrorCodes::LimitExceeded,
              "Glob result bounds exceed the native tool limits."));
    }
    auto parsed = parsePattern(pattern);
    if (!parsed) {
      return Domain::Result<std::vector<Domain::PathText>>::failure(
          std::move(parsed).error());
    }
    std::vector<Domain::PathText> matches;
    matches.reserve(maximumMatches);
    std::size_t responseBytes{};
    const Detail::NativeWalkOptions options{100'000U, 128U, false};
    const auto visitor =
        [&](const HANDLE, const Detail::NativeDirectoryEntry &,
            const std::wstring_view canonicalPath,
            const std::wstring_view relativePath, const std::size_t,
            const Domain::OperationContext &) -> Domain::Result<bool> {
      const auto pathSegments = splitPath(relativePath);
      if (!matchPath(parsed.value().segments, pathSegments)) {
        return Domain::Result<bool>::success(false);
      }
      auto converted =
          InfrastructureDetail::WindowsPathResolver::toPathText(canonicalPath);
      if (!converted) {
        return Domain::Result<bool>::failure(std::move(converted).error());
      }
      if (converted.value().value().size() >
          maximumResponseBytes - responseBytes) {
        return Domain::Result<bool>::success(true);
      }
      responseBytes += converted.value().value().size();
      matches.push_back(std::move(converted).value());
      return Domain::Result<bool>::success(matches.size() >= maximumMatches);
    };
    auto walked =
        Detail::walkAuthorizedDirectory(root, options, visitor, context);
    if (!walked) {
      return Domain::Result<std::vector<Domain::PathText>>::failure(
          std::move(walked).error());
    }
    return Domain::Result<std::vector<Domain::PathText>>::success(
        std::move(matches));
  } catch (...) {
    return Domain::Result<std::vector<Domain::PathText>>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The bounded native glob failed."));
  }
}

} // namespace ForgeConductor::NativeTools::Windows
