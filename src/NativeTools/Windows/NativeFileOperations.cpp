#include "NativeFileOperations.h"

#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/RelativeFileOperations.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Infrastructure/Windows/Detail/Win32Error.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace ForgeConductor::NativeTools::Windows::Detail {
namespace {

constexpr std::size_t DirectoryBufferBytes = 64U * 1024U;
constexpr std::size_t IoChunkBytes = 64U * 1024U;
constexpr std::size_t MaximumNativePathCharacters = 32U * 1024U;

[[nodiscard]] std::wstring leafName(const std::wstring_view path) {
  const auto separator = path.find_last_of(L'\\');
  if (separator == std::wstring_view::npos || separator + 1U >= path.size()) {
    return {};
  }
  return std::wstring{path.substr(separator + 1U)};
}

[[nodiscard]] std::wstring withoutExtendedPrefix(std::wstring value) {
  if (value.starts_with(L"\\\\?\\UNC\\")) {
    value.erase(0U, 7U);
    value.insert(value.begin(), L'\\');
  } else if (value.starts_with(L"\\\\?\\")) {
    value.erase(0U, 4U);
  }
  return value;
}

[[nodiscard]] Domain::Result<DWORD>
verifyOpenedObject(const HANDLE handle, const std::wstring_view expectedPath,
                   const std::optional<bool> expectedDirectory) noexcept {
  try {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag,
                                       sizeof(tag)) == FALSE) {
      return Domain::Result<DWORD>::failure(
          nativeFileError("Inspect a native tool path", ::GetLastError()));
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return Domain::Result<DWORD>::failure(Domain::makeError(
          Domain::ErrorCodes::PathOutsideAuthority,
          "Native filesystem tools cannot traverse a reparse point."));
    }
    const bool directory =
        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if (expectedDirectory.has_value() && directory != *expectedDirectory) {
      return Domain::Result<DWORD>::failure(Domain::makeError(
          Domain::ErrorCodes::InvalidRequest,
          "The native tool path has an unexpected filesystem-object type."));
    }
    if (directory) {
      FILE_CASE_SENSITIVE_INFO caseInfo{};
      if (::GetFileInformationByHandleEx(handle, FileCaseSensitiveInfo,
                                         &caseInfo,
                                         sizeof(caseInfo)) == FALSE) {
        return Domain::Result<DWORD>::failure(
            nativeFileError("Inspect native tool directory case sensitivity",
                            ::GetLastError()));
      }
      auto supported = InfrastructureDetail::WindowsPathResolver::
          validateDirectoryCaseSensitivityFlags(caseInfo.Flags);
      if (!supported) {
        return Domain::Result<DWORD>::failure(std::move(supported).error());
      }
    }

    const DWORD required = ::GetFinalPathNameByHandleW(
        handle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0U || required > MaximumNativePathCharacters + 4U) {
      return Domain::Result<DWORD>::failure(
          nativeFileError("Resolve a native tool path", ::GetLastError()));
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
    const DWORD written = ::GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0U || written >= buffer.size()) {
      return Domain::Result<DWORD>::failure(
          nativeFileError("Resolve a native tool path", ::GetLastError()));
    }
    const auto actual = withoutExtendedPrefix(
        std::wstring{buffer.data(), static_cast<std::size_t>(written)});
    if (!samePath(actual, expectedPath)) {
      return Domain::Result<DWORD>::failure(Domain::makeError(
          Domain::ErrorCodes::PathOutsideAuthority,
          "An opened native tool path escaped its authorized canonical path."));
    }
    return Domain::Result<DWORD>::success(tag.FileAttributes);
  } catch (...) {
    return Domain::Result<DWORD>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The opened native tool path could not be verified."));
  }
}

[[nodiscard]] InfrastructureDetail::RelativeOpenResult
openRelativeAny(const HANDLE parent, const std::wstring_view name,
                const ACCESS_MASK desiredAccess, const DWORD shareAccess,
                bool &openedDirectory) noexcept {
  InfrastructureDetail::RelativeOpenOptions options{};
  options.desiredAccess = desiredAccess;
  options.shareAccess = shareAccess;
  options.disposition =
      InfrastructureDetail::RelativeOpenDisposition::OpenExisting;
  options.objectType = InfrastructureDetail::RelativeObjectType::File;
  options.sequentialAccess = true;
  auto opened = InfrastructureDetail::openRelative(parent, name, options);
  if (opened) {
    openedDirectory = false;
    return opened;
  }
  if (opened.win32Error != ERROR_DIRECTORY &&
      opened.win32Error != ERROR_ACCESS_DENIED) {
    return opened;
  }
  options.objectType = InfrastructureDetail::RelativeObjectType::Directory;
  options.sequentialAccess = false;
  opened = InfrastructureDetail::openRelative(parent, name, options);
  if (opened) {
    openedDirectory = true;
  }
  return opened;
}

[[nodiscard]] bool isDotEntry(const std::wstring_view name) noexcept {
  return name == L"." || name == L"..";
}

[[nodiscard]] bool entryLess(const NativeDirectoryEntry &left,
                             const NativeDirectoryEntry &right) noexcept {
  const int folded = ::CompareStringOrdinal(
      left.name.data(), static_cast<int>(left.name.size()), right.name.data(),
      static_cast<int>(right.name.size()), TRUE);
  if (folded != CSTR_EQUAL) {
    return folded == CSTR_LESS_THAN;
  }
  return ::CompareStringOrdinal(
             left.name.data(), static_cast<int>(left.name.size()),
             right.name.data(), static_cast<int>(right.name.size()),
             FALSE) == CSTR_LESS_THAN;
}

[[nodiscard]] Domain::Result<bool>
walkDirectory(const HANDLE directory,
              const std::wstring_view canonicalDirectory,
              const std::wstring_view relativeDirectory,
              const std::size_t depth, const NativeWalkOptions &options,
              std::size_t &visited, const WalkVisitor &visitor,
              const Domain::OperationContext &context) noexcept {
  try {
    if (depth > options.maximumDepth) {
      return Domain::Result<bool>::failure(Domain::makeError(
          Domain::ErrorCodes::LimitExceeded,
          "The native directory walk exceeded its depth bound."));
    }
    const auto remaining = options.maximumVisitedEntries - visited;
    auto entries = enumerateDirectory(directory, remaining + 1U, context);
    if (!entries) {
      return Domain::Result<bool>::failure(std::move(entries).error());
    }
    if (entries.value().size() > remaining) {
      return Domain::Result<bool>::failure(Domain::makeError(
          Domain::ErrorCodes::LimitExceeded,
          "The native directory walk exceeded its entry bound."));
    }

    for (const auto &entry : entries.value()) {
      auto valid = InfrastructureDetail::validateOperationContext(
          context, std::chrono::steady_clock::now(),
          "walk an authorized directory");
      if (!valid) {
        return Domain::Result<bool>::failure(std::move(valid).error());
      }
      ++visited;
      if (entry.isReparsePoint()) {
        continue;
      }
      std::wstring canonical{canonicalDirectory};
      canonical.push_back(L'\\');
      canonical.append(entry.name);
      std::wstring relative{relativeDirectory};
      if (!relative.empty()) {
        relative.push_back(L'\\');
      }
      relative.append(entry.name);

      auto visit =
          visitor(directory, entry, canonical, relative, depth, context);
      if (!visit) {
        return Domain::Result<bool>::failure(std::move(visit).error());
      }
      if (visit.value()) {
        return Domain::Result<bool>::success(true);
      }
      if (!entry.isDirectory()) {
        continue;
      }
      if (options.excludeGitAndNodeModules &&
          (sameName(entry.name, L".git") ||
           sameName(entry.name, L"node_modules"))) {
        continue;
      }
      auto child = openChildObject(
          directory, entry.name, canonical,
          FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, context);
      if (!child) {
        return Domain::Result<bool>::failure(std::move(child).error());
      }
      if (!child.value().isDirectory()) {
        return Domain::Result<bool>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "A directory entry changed type during the native walk.", true));
      }
      auto nested =
          walkDirectory(child.value().handle.get(), canonical, relative,
                        depth + 1U, options, visited, visitor, context);
      if (!nested) {
        return nested;
      }
      if (nested.value()) {
        return nested;
      }
    }
    return Domain::Result<bool>::success(false);
  } catch (...) {
    return Domain::Result<bool>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The bounded native directory walk failed."));
  }
}

} // namespace

Domain::Error nativeFileError(const std::string_view action,
                              const DWORD nativeCode) noexcept {
  switch (nativeCode) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_NO_MORE_FILES:
    return InfrastructureDetail::makeWin32Error(
        action, nativeCode, Domain::ErrorCodes::RecordNotFound);
  case ERROR_ACCESS_DENIED:
  case ERROR_PRIVILEGE_NOT_HELD:
    return InfrastructureDetail::makeWin32Error(
        action, nativeCode, Domain::ErrorCodes::Unauthorized);
  case ERROR_DISK_FULL:
  case ERROR_HANDLE_DISK_FULL:
    return InfrastructureDetail::makeWin32Error(
        action, nativeCode, Domain::ErrorCodes::StorageFull);
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
  case ERROR_SHARING_VIOLATION:
  case ERROR_LOCK_VIOLATION:
  case ERROR_DIR_NOT_EMPTY:
  case ERROR_NOT_SAME_DEVICE:
    return InfrastructureDetail::makeWin32Error(
        action, nativeCode, Domain::ErrorCodes::Conflict, true);
  default:
    return InfrastructureDetail::makeWin32Error(action, nativeCode);
  }
}

bool samePath(const std::wstring_view left,
              const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
         ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

bool sameName(const std::wstring_view left,
              const std::wstring_view right) noexcept {
  return samePath(left, right);
}

Domain::Result<OpenedNativeObject> openAuthorizedObject(
    const Contracts::AuthorizedPath &path,
    const Domain::FileAccess requiredAccess,
    const InfrastructureDetail::MissingPathPolicy missingPolicy,
    const Domain::OperationContext &context, const ACCESS_MASK desiredAccess,
    const DWORD shareAccess) noexcept {
  try {
    auto valid = InfrastructureDetail::validateOperationContext(
        context, std::chrono::steady_clock::now(),
        "open an authorized native tool path");
    if (!valid) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(valid).error());
    }
    auto anchored = InfrastructureDetail::WindowsPathResolver::
        resolveAnchoredAuthorizedPath(path, requiredAccess, missingPolicy,
            InfrastructureDetail::AnchorSharePolicy::AllowConcurrentWrite);
    if (!anchored) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(anchored).error());
    }
    const auto name = leafName(anchored.value().canonicalPath());
    if (name.empty()) {
      return Domain::Result<OpenedNativeObject>::failure(Domain::makeError(
          Domain::ErrorCodes::InvalidRequest,
          "The authorized native tool path has no leaf component."));
    }
    valid = InfrastructureDetail::validateOperationContext(
        context, std::chrono::steady_clock::now(),
        "open an authorized native tool path");
    if (!valid) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(valid).error());
    }
    auto anchorsValid = anchored.value().revalidateDirectoryAnchors();
    if (!anchorsValid) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(anchorsValid).error());
    }
    bool directory{};
    auto opened = openRelativeAny(anchored.value().parentDirectoryHandle(),
                                  name, desiredAccess, shareAccess, directory);
    if (!opened) {
      return Domain::Result<OpenedNativeObject>::failure(nativeFileError(
          "Open an authorized native tool path", opened.win32Error));
    }
    auto attributes = verifyOpenedObject(
        opened.handle.get(), anchored.value().canonicalPath(), std::nullopt);
    if (!attributes) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(attributes).error());
    }
    std::wstring canonicalPath{anchored.value().canonicalPath()};
    auto owner = std::optional<InfrastructureDetail::AnchoredAuthorizedPath>{
        std::move(anchored).value()};
    return Domain::Result<OpenedNativeObject>::success(OpenedNativeObject{
        std::move(opened.handle), std::move(canonicalPath), attributes.value(),
        std::move(owner)});
  } catch (...) {
    return Domain::Result<OpenedNativeObject>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The authorized native tool path could not be opened."));
  }
}

Domain::Result<OpenedNativeObject>
openAuthorizedDirectory(const Contracts::AuthorizedPath &path,
                        const Domain::OperationContext &context) noexcept {
  auto opened = openAuthorizedObject(
      path, Domain::FileAccess::Read,
      InfrastructureDetail::MissingPathPolicy::Reject, context);
  if (!opened) {
    return opened;
  }
  if (!opened.value().isDirectory()) {
    return Domain::Result<OpenedNativeObject>::failure(Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        "The authorized native tool root is not a directory."));
  }
  return opened;
}

Domain::Result<OpenedNativeObject>
openChildObject(const HANDLE parentDirectory, const std::wstring_view childName,
                const std::wstring_view expectedPath,
                const ACCESS_MASK desiredAccess, const DWORD shareAccess,
                const Domain::OperationContext &context) noexcept {
  try {
    auto valid = InfrastructureDetail::validateOperationContext(
        context, std::chrono::steady_clock::now(),
        "open a bounded native directory entry");
    if (!valid) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(valid).error());
    }
    bool directory{};
    auto opened = openRelativeAny(parentDirectory, childName, desiredAccess,
                                  shareAccess, directory);
    if (!opened) {
      return Domain::Result<OpenedNativeObject>::failure(nativeFileError(
          "Open a bounded native directory entry", opened.win32Error));
    }
    auto attributes =
        verifyOpenedObject(opened.handle.get(), expectedPath, std::nullopt);
    if (!attributes) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(attributes).error());
    }
    return Domain::Result<OpenedNativeObject>::success(
        OpenedNativeObject{std::move(opened.handle), std::wstring{expectedPath},
                           attributes.value()});
  } catch (...) {
    return Domain::Result<OpenedNativeObject>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The native directory entry could not be opened."));
  }
}

Domain::Result<OpenedNativeObject>
openCanonicalDirectory(const std::wstring_view canonicalPath,
                       const ACCESS_MASK desiredAccess, const DWORD shareAccess,
                       const Domain::OperationContext &context) noexcept {
  try {
    auto valid = InfrastructureDetail::validateOperationContext(
        context, std::chrono::steady_clock::now(),
        "open an authority root directory");
    if (!valid) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(valid).error());
    }
    std::wstring native{L"\\\\?\\"};
    native.append(canonicalPath);
    InfrastructureDetail::UniqueHandle handle{::CreateFileW(
        native.c_str(), desiredAccess, shareAccess, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!handle) {
      return Domain::Result<OpenedNativeObject>::failure(nativeFileError(
          "Open an authority root directory", ::GetLastError()));
    }
    auto attributes = verifyOpenedObject(handle.get(), canonicalPath, true);
    if (!attributes) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(attributes).error());
    }
    return Domain::Result<OpenedNativeObject>::success(OpenedNativeObject{
        std::move(handle), std::wstring{canonicalPath}, attributes.value()});
  } catch (...) {
    return Domain::Result<OpenedNativeObject>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The authority root directory could not be opened."));
  }
}

Domain::Result<OpenedNativeObject>
openOrCreateChildDirectory(const HANDLE parentDirectory,
                           const std::wstring_view childName,
                           const std::wstring_view expectedPath,
                           const Domain::OperationContext &context) noexcept {
  try {
    auto valid = InfrastructureDetail::validateOperationContext(
        context, std::chrono::steady_clock::now(),
        "create an authorized directory");
    if (!valid) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(valid).error());
    }
    InfrastructureDetail::RelativeOpenOptions options{};
    options.desiredAccess = FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                            FILE_READ_ATTRIBUTES | FILE_ADD_SUBDIRECTORY |
                            FILE_ADD_FILE | FILE_DELETE_CHILD;
    options.shareAccess = FILE_SHARE_READ;
    options.disposition =
        InfrastructureDetail::RelativeOpenDisposition::OpenOrCreate;
    options.objectType = InfrastructureDetail::RelativeObjectType::Directory;
    auto opened =
        InfrastructureDetail::openRelative(parentDirectory, childName, options);
    if (!opened) {
      return Domain::Result<OpenedNativeObject>::failure(
          nativeFileError("Create an authorized directory", opened.win32Error));
    }
    auto attributes =
        verifyOpenedObject(opened.handle.get(), expectedPath, true);
    if (!attributes) {
      return Domain::Result<OpenedNativeObject>::failure(
          std::move(attributes).error());
    }
    return Domain::Result<OpenedNativeObject>::success(
        OpenedNativeObject{std::move(opened.handle), std::wstring{expectedPath},
                           attributes.value()});
  } catch (...) {
    return Domain::Result<OpenedNativeObject>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The authorized directory could not be created."));
  }
}

Domain::Result<void> ensureAuthorizedParentDirectories(
    const Contracts::AuthorizedPath &path,
    const Domain::OperationContext &context) noexcept {
  try {
    if (path.access() != Domain::FileAccess::Create &&
        path.access() != Domain::FileAccess::Write) {
      return Domain::Result<void>::failure(Domain::makeError(
          Domain::ErrorCodes::Unauthorized,
          "Creating parent directories requires create or write access."));
    }
    auto active = InfrastructureDetail::validateOperationContext(
        context, std::chrono::steady_clock::now(),
        "create authorized parent directories");
    if (!active) {
      return active;
    }
    if (path.access() == Domain::FileAccess::Write) {
      return Domain::Result<void>::success();
    }
    auto target = InfrastructureDetail::WindowsPathResolver::resolveAuthorizedPath(
        path, path.access(),
        InfrastructureDetail::MissingPathPolicy::AllowDescendants);
    if (!target) {
      return Domain::Result<void>::failure(std::move(target).error());
    }
    auto root = InfrastructureDetail::WindowsPathResolver::resolveAppOwnedRoot(
        path.authorityRoot().value());
    if (!root) {
      return Domain::Result<void>::failure(std::move(root).error());
    }
    const auto separator = target.value().find_last_of(L'\\');
    if (separator == std::wstring::npos || separator < root.value().size()) {
      return Domain::Result<void>::failure(Domain::makeError(
          Domain::ErrorCodes::PathOutsideAuthority,
          "The destination must name a child below its authority root."));
    }
    const std::wstring_view parentPath{target.value().data(), separator};
    if (samePath(parentPath, root.value())) {
      return Domain::Result<void>::success();
    }
    if (parentPath.size() <= root.value().size() ||
        parentPath[root.value().size()] != L'\\') {
      return Domain::Result<void>::failure(Domain::makeError(
          Domain::ErrorCodes::PathOutsideAuthority,
          "The destination parent is outside its authority root."));
    }
    std::vector<OpenedNativeObject> anchors;
    auto rootDirectory = openCanonicalDirectory(
        root.value(),
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES |
            FILE_ADD_SUBDIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD,
        FILE_SHARE_READ, context);
    if (!rootDirectory) {
      return Domain::Result<void>::failure(std::move(rootDirectory).error());
    }
    anchors.push_back(std::move(rootDirectory).value());
    std::size_t start = root.value().size() + 1U;
    std::wstring currentPath{root.value()};
    while (start < parentPath.size()) {
      const auto nextSeparator = parentPath.find(L'\\', start);
      const auto end = nextSeparator == std::wstring_view::npos
                           ? parentPath.size()
                           : nextSeparator;
      const auto component = parentPath.substr(start, end - start);
      currentPath.push_back(L'\\');
      currentPath.append(component);
      auto next = openOrCreateChildDirectory(
          anchors.back().handle.get(), component, currentPath, context);
      if (!next) {
        return Domain::Result<void>::failure(std::move(next).error());
      }
      anchors.push_back(std::move(next).value());
      if (nextSeparator == std::wstring_view::npos) {
        break;
      }
      start = nextSeparator + 1U;
    }
    return Domain::Result<void>::success();
  } catch (...) {
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The authorized destination parents could not be created."));
  }
}

Domain::Result<std::vector<NativeDirectoryEntry>>
enumerateDirectory(const HANDLE directory, const std::size_t maximumEntries,
                   const Domain::OperationContext &context) noexcept {
  try {
    if (maximumEntries == 0U) {
      return Domain::Result<std::vector<NativeDirectoryEntry>>::failure(
          Domain::makeError(
              Domain::ErrorCodes::InvalidRequest,
              "Directory enumeration requires a positive entry bound."));
    }
    std::vector<NativeDirectoryEntry> entries;
    entries.reserve((std::min)(maximumEntries, std::size_t{1'000U}));
    std::array<std::uint64_t, DirectoryBufferBytes / sizeof(std::uint64_t)>
        buffer{};
    bool restart = true;
    for (;;) {
      auto valid = InfrastructureDetail::validateOperationContext(
          context, std::chrono::steady_clock::now(),
          "enumerate an authorized directory");
      if (!valid) {
        return Domain::Result<std::vector<NativeDirectoryEntry>>::failure(
            std::move(valid).error());
      }
      const auto informationClass =
          restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
      restart = false;
      if (::GetFileInformationByHandleEx(
              directory, informationClass, buffer.data(),
              static_cast<DWORD>(DirectoryBufferBytes)) == FALSE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_NO_MORE_FILES) {
          break;
        }
        return Domain::Result<std::vector<NativeDirectoryEntry>>::failure(
            nativeFileError("Enumerate an authorized directory", error));
      }
      auto *item = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(buffer.data());
      for (;;) {
        if (item->FileNameLength % sizeof(wchar_t) != 0U) {
          return Domain::Result<std::vector<NativeDirectoryEntry>>::failure(
              Domain::makeError(
                  Domain::ErrorCodes::IntegrityFailure,
                  "Windows returned a malformed directory entry name."));
        }
        const std::wstring_view name{
            item->FileName,
            static_cast<std::size_t>(item->FileNameLength) / sizeof(wchar_t)};
        if (!isDotEntry(name)) {
          if (entries.size() >= maximumEntries) {
            return Domain::Result<std::vector<NativeDirectoryEntry>>::success(
                std::move(entries));
          }
          entries.push_back(
              NativeDirectoryEntry{std::wstring{name}, item->FileAttributes});
        }
        if (item->NextEntryOffset == 0U) {
          break;
        }
        item = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(
            reinterpret_cast<std::byte *>(item) + item->NextEntryOffset);
      }
    }
    std::sort(entries.begin(), entries.end(), entryLess);
    return Domain::Result<std::vector<NativeDirectoryEntry>>::success(
        std::move(entries));
  } catch (...) {
    return Domain::Result<std::vector<NativeDirectoryEntry>>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The authorized directory could not be enumerated."));
  }
}

Domain::Result<void>
walkAuthorizedDirectory(const Contracts::AuthorizedPath &root,
                        const NativeWalkOptions &options,
                        const WalkVisitor &visitor,
                        const Domain::OperationContext &context) noexcept {
  try {
    if (options.maximumVisitedEntries == 0U ||
        options.maximumVisitedEntries > 100'000U ||
        options.maximumDepth == 0U || options.maximumDepth > 128U || !visitor) {
      return Domain::Result<void>::failure(
          Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                            "The native directory walk bounds are invalid."));
    }
    auto opened = openAuthorizedDirectory(root, context);
    if (!opened) {
      return Domain::Result<void>::failure(std::move(opened).error());
    }
    std::size_t visited{};
    auto walked =
        walkDirectory(opened.value().handle.get(), opened.value().canonicalPath,
                      L"", 1U, options, visited, visitor, context);
    if (!walked) {
      return Domain::Result<void>::failure(std::move(walked).error());
    }
    return Domain::Result<void>::success();
  } catch (...) {
    return Domain::Result<void>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The authorized native directory walk failed."));
  }
}

Domain::Result<void>
validateUtf8Text(const std::string_view text,
                 const std::string_view description) noexcept {
  try {
    if (text.find('\0') != std::string_view::npos) {
      return Domain::Result<void>::failure(
          Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                            std::string{description} + " contains NUL."));
    }
    auto converted = InfrastructureDetail::strictUtf8ToUtf16(text);
    if (!converted) {
      return Domain::Result<void>::failure(std::move(converted).error());
    }
    return Domain::Result<void>::success();
  } catch (...) {
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure, "UTF-8 text validation failed."));
  }
}

Domain::Result<std::vector<std::byte>>
readOpenedFile(const HANDLE file, const std::size_t maximumBytes,
               const Domain::OperationContext &context) noexcept {
  try {
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) == FALSE) {
      return Domain::Result<std::vector<std::byte>>::failure(
          nativeFileError("Size a native text file", ::GetLastError()));
    }
    if (size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > maximumBytes) {
      return Domain::Result<std::vector<std::byte>>::failure(
          Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                            "The native text file exceeds its byte bound."));
    }
    LARGE_INTEGER beginning{};
    if (::SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) == FALSE) {
      return Domain::Result<std::vector<std::byte>>::failure(
          nativeFileError("Seek a native text file", ::GetLastError()));
    }
    std::vector<std::byte> content;
    content.reserve(static_cast<std::size_t>(size.QuadPart));
    std::array<std::byte, IoChunkBytes> buffer{};
    for (;;) {
      auto valid = InfrastructureDetail::validateOperationContext(
          context, std::chrono::steady_clock::now(), "read a native text file");
      if (!valid) {
        return Domain::Result<std::vector<std::byte>>::failure(
            std::move(valid).error());
      }
      const auto remaining = maximumBytes - content.size();
      const auto request =
          (std::min)(buffer.size(), remaining == 0U ? 1U : remaining);
      DWORD read{};
      if (::ReadFile(file, buffer.data(), static_cast<DWORD>(request), &read,
                     nullptr) == FALSE) {
        return Domain::Result<std::vector<std::byte>>::failure(
            nativeFileError("Read a native text file", ::GetLastError()));
      }
      if (read == 0U) {
        break;
      }
      if (read > remaining) {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The native text file grew beyond its byte bound."));
      }
      content.insert(content.end(), buffer.begin(), buffer.begin() + read);
    }
    return Domain::Result<std::vector<std::byte>>::success(std::move(content));
  } catch (...) {
    return Domain::Result<std::vector<std::byte>>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The native text file could not be read."));
  }
}

Domain::Result<void>
deleteOpenedObject(const HANDLE object,
                   const Domain::OperationContext &context) noexcept {
  auto valid = InfrastructureDetail::validateOperationContext(
      context, std::chrono::steady_clock::now(),
      "delete an authorized filesystem object");
  if (!valid) {
    return valid;
  }
  FILE_DISPOSITION_INFO_EX disposition{};
  disposition.Flags = FILE_DISPOSITION_FLAG_DELETE |
                      FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                      FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
  if (::SetFileInformationByHandle(object, FileDispositionInfoEx, &disposition,
                                   sizeof(disposition)) != FALSE) {
    return Domain::Result<void>::success();
  }
  const DWORD extendedError = ::GetLastError();
  if (extendedError != ERROR_INVALID_PARAMETER &&
      extendedError != ERROR_NOT_SUPPORTED) {
    return Domain::Result<void>::failure(nativeFileError(
        "Delete an authorized filesystem object", extendedError));
  }
  FILE_DISPOSITION_INFO fallback{};
  fallback.DeleteFile = TRUE;
  if (::SetFileInformationByHandle(object, FileDispositionInfo, &fallback,
                                   sizeof(fallback)) == FALSE) {
    return Domain::Result<void>::failure(nativeFileError(
        "Delete an authorized filesystem object", ::GetLastError()));
  }
  return Domain::Result<void>::success();
}

} // namespace ForgeConductor::NativeTools::Windows::Detail
