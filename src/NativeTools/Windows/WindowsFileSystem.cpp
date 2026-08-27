#include "ForgeConductor/NativeTools/Windows/WindowsFileSystem.h"

#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"
#include "NativeFileOperations.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {
namespace {

namespace InfrastructureDetail =
    ForgeConductor::Infrastructure::Windows::Detail;

constexpr std::size_t MaximumEnumeratedEntries = 100'000U;

[[nodiscard]] Domain::Result<void>
validateBound(const std::size_t requested, const std::size_t maximum,
              const char *const subject) noexcept {
  try {
    if (requested == 0U || requested > maximum) {
      return Domain::Result<void>::failure(Domain::makeError(
          Domain::ErrorCodes::LimitExceeded,
          std::string{subject} + " exceeds its native tool bound."));
    }
    return Domain::Result<void>::success();
  } catch (...) {
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The native filesystem bound could not be validated."));
  }
}

[[nodiscard]] std::string_view
byteText(const std::vector<std::byte> &bytes) noexcept {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::string_view
byteText(const std::span<const std::byte> bytes) noexcept {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

[[nodiscard]] bool
sameAuthorizedObject(const Contracts::AuthorizedPath &left,
                     const Contracts::AuthorizedPath &right) noexcept {
  return left.authorityId() == right.authorityId() &&
         left.authorityRoot() == right.authorityRoot() &&
         left.canonicalPath() == right.canonicalPath();
}

[[nodiscard]] Domain::Result<void>
rejectAuthorityRoot(const std::wstring_view target,
                    const Domain::PathText &authorityRoot) noexcept {
  auto root = InfrastructureDetail::WindowsPathResolver::resolveAppOwnedRoot(
      authorityRoot.value());
  if (!root) {
    return Domain::Result<void>::failure(std::move(root).error());
  }
  if (Detail::samePath(target, root.value())) {
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        "Native filesystem tools cannot mutate an authority root."));
  }
  return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void>
validateTree(const HANDLE directory, const std::wstring_view canonicalDirectory,
             const std::size_t depth, std::size_t &visited,
             const Domain::OperationContext &context) noexcept {
  if (depth > 128U || visited > 100'000U) {
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::LimitExceeded,
        "Recursive deletion exceeded its bounded tree limits."));
  }
  auto entries =
      Detail::enumerateDirectory(directory, 100'001U - visited, context);
  if (!entries) {
    return Domain::Result<void>::failure(std::move(entries).error());
  }
  for (const auto &entry : entries.value()) {
    if (++visited > 100'000U) {
      return Domain::Result<void>::failure(
          Domain::makeError(Domain::ErrorCodes::LimitExceeded,
                            "Recursive deletion exceeded its entry bound."));
    }
    if (entry.isReparsePoint()) {
      return Domain::Result<void>::failure(Domain::makeError(
          Domain::ErrorCodes::PathOutsideAuthority,
          "Recursive deletion refuses a tree containing a reparse point."));
    }
    if (!entry.isDirectory()) {
      continue;
    }
    std::wstring childPath{canonicalDirectory};
    childPath.push_back(L'\\');
    childPath.append(entry.name);
    auto child = Detail::openChildObject(
        directory, entry.name, childPath,
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, context);
    if (!child) {
      return Domain::Result<void>::failure(std::move(child).error());
    }
    if (!child.value().isDirectory()) {
      return Domain::Result<void>::failure(Domain::makeError(
          Domain::ErrorCodes::Conflict,
          "A recursive-delete directory changed type during validation.",
          true));
    }
    auto nested = validateTree(child.value().handle.get(), childPath,
                               depth + 1U, visited, context);
    if (!nested) {
      return nested;
    }
  }
  return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void>
removeTree(const HANDLE directory, const std::wstring_view canonicalDirectory,
           const std::size_t depth, std::size_t &visited,
           const Domain::OperationContext &context) noexcept {
  if (depth > 128U || visited > 100'000U) {
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::LimitExceeded,
        "Recursive deletion exceeded its bounded tree limits."));
  }
  auto entries =
      Detail::enumerateDirectory(directory, 100'001U - visited, context);
  if (!entries) {
    return Domain::Result<void>::failure(std::move(entries).error());
  }
  for (const auto &entry : entries.value()) {
    if (++visited > 100'000U) {
      return Domain::Result<void>::failure(
          Domain::makeError(Domain::ErrorCodes::LimitExceeded,
                            "Recursive deletion exceeded its entry bound."));
    }
    if (entry.isReparsePoint()) {
      return Domain::Result<void>::failure(
          Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                            "Recursive deletion refuses a reparse point."));
    }
    std::wstring childPath{canonicalDirectory};
    childPath.push_back(L'\\');
    childPath.append(entry.name);
    auto child = Detail::openChildObject(
        directory, entry.name, childPath,
        DELETE | FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, context);
    if (!child) {
      return Domain::Result<void>::failure(std::move(child).error());
    }
    if (child.value().isDirectory()) {
      auto nested = removeTree(child.value().handle.get(), childPath,
                               depth + 1U, visited, context);
      if (!nested) {
        return nested;
      }
    }
    auto removed =
        Detail::deleteOpenedObject(child.value().handle.get(), context);
    if (!removed) {
      return removed;
    }
  }
  return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void>
renameOpenedObject(const HANDLE source, const HANDLE destinationDirectory,
                   const std::wstring_view destinationName,
                   const bool sameDirectory,
                   const Domain::OperationContext &context) noexcept {
  try {
    struct NativeIoStatusBlock final {
      union {
        LONG status;
        void *pointer;
      } result{};
      ULONG_PTR information{};
    };
    using NtSetInformationFileFunction =
        LONG(NTAPI *)(HANDLE, NativeIoStatusBlock *, void *, ULONG, ULONG);
    using RtlNtStatusToDosErrorFunction = ULONG(WINAPI *)(LONG);
    constexpr ULONG NativeFileRenameInformationEx = 65U;
    constexpr ULONG NativeFileRenameInformation = 10U;

    auto valid = InfrastructureDetail::validateOperationContext(
        context, std::chrono::steady_clock::now(),
        "move an authorized filesystem object");
    if (!valid) {
      return valid;
    }
    if (destinationName.empty() ||
        destinationName.size() >
            (std::numeric_limits<DWORD>::max)() / sizeof(wchar_t)) {
      return Domain::Result<void>::failure(
          Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                            "The move destination has an invalid leaf name."));
    }
    const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
    const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
    std::vector<std::uint64_t> storage(
        (informationBytes + sizeof(std::uint64_t) - 1U) /
        sizeof(std::uint64_t));
    auto *information = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    std::memset(information, 0, informationBytes);
    information->Flags = 0U;
    information->RootDirectory = sameDirectory ? nullptr : destinationDirectory;
    information->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(information->FileName, destinationName.data(), nameBytes);
    information->FileName[destinationName.size()] = L'\0';
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto ntSetInformationFile =
        ntdll == nullptr ? nullptr
                         : reinterpret_cast<NtSetInformationFileFunction>(
                               ::GetProcAddress(ntdll, "NtSetInformationFile"));
    const auto rtlNtStatusToDosError =
        ntdll == nullptr
            ? nullptr
            : reinterpret_cast<RtlNtStatusToDosErrorFunction>(
                  ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (ntSetInformationFile == nullptr || rtlNtStatusToDosError == nullptr) {
      return Domain::Result<void>::failure(Detail::nativeFileError(
          "Load native handle-relative move support", ERROR_PROC_NOT_FOUND));
    }
    NativeIoStatusBlock ioStatus{};
    const LONG status = ntSetInformationFile(
        source, &ioStatus, information, static_cast<ULONG>(informationBytes),
        sameDirectory ? NativeFileRenameInformationEx
                      : NativeFileRenameInformation);
    if (status < 0) {
      return Domain::Result<void>::failure(Detail::nativeFileError(
          "Move an authorized filesystem object",
          static_cast<DWORD>(rtlNtStatusToDosError(status))));
    }
    return Domain::Result<void>::success();
  } catch (...) {
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The authorized filesystem object could not be moved."));
  }
}

struct MoveDestinationParent final {
  std::vector<Detail::OpenedNativeObject> directoryAnchors;

  [[nodiscard]] HANDLE handle() const noexcept {
    return directoryAnchors.empty() ? nullptr
                                    : directoryAnchors.back().handle.get();
  }
};

[[nodiscard]] Domain::Result<MoveDestinationParent>
openMoveDestinationParent(const std::wstring_view destinationPath,
                          const Domain::PathText &authorityRoot,
                          const Domain::OperationContext &context) noexcept {
  try {
    auto root = InfrastructureDetail::WindowsPathResolver::resolveAppOwnedRoot(
        authorityRoot.value());
    if (!root) {
      return Domain::Result<MoveDestinationParent>::failure(
          std::move(root).error());
    }
    const auto finalSeparator = destinationPath.find_last_of(L'\\');
    if (finalSeparator == std::wstring_view::npos ||
        finalSeparator < root.value().size()) {
      return Domain::Result<MoveDestinationParent>::failure(
          Domain::makeError(
              Domain::ErrorCodes::PathOutsideAuthority,
              "The move destination has no authorized parent directory."));
    }
    const auto parentPath = destinationPath.substr(0U, finalSeparator);
    auto current = Detail::openCanonicalDirectory(
        root.value(),
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, context);
    if (!current) {
      return Domain::Result<MoveDestinationParent>::failure(
          std::move(current).error());
    }
    MoveDestinationParent result;
    result.directoryAnchors.push_back(std::move(current).value());
    if (Detail::samePath(parentPath, root.value())) {
      return Domain::Result<MoveDestinationParent>::success(
          std::move(result));
    }
    std::size_t start = root.value().size() + 1U;
    std::wstring currentPath{root.value()};
    while (start < parentPath.size()) {
      const auto separator = parentPath.find(L'\\', start);
      const auto end =
          separator == std::wstring_view::npos ? parentPath.size() : separator;
      const auto component = parentPath.substr(start, end - start);
      currentPath.push_back(L'\\');
      currentPath.append(component);
      auto next = Detail::openChildObject(
          result.handle(), component, currentPath,
          FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, context);
      if (!next) {
        return Domain::Result<MoveDestinationParent>::failure(
            std::move(next).error());
      }
      if (!next.value().isDirectory()) {
        return Domain::Result<MoveDestinationParent>::failure(
            Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "The move destination parent changed filesystem-object type.",
                true));
      }
      result.directoryAnchors.push_back(std::move(next).value());
      if (separator == std::wstring_view::npos) {
        break;
      }
      start = separator + 1U;
    }
    return Domain::Result<MoveDestinationParent>::success(std::move(result));
  } catch (...) {
    return Domain::Result<MoveDestinationParent>::failure(
        Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The move destination parent could not be anchored."));
  }
}

} // namespace

class WindowsFileSystem::Impl final {
public:
  explicit Impl(
      std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore) noexcept
      : atomicFileStore{std::move(atomicFileStore)} {}

  std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore;
};

WindowsFileSystem::WindowsFileSystem(
    std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore)
    : implementation_{std::make_unique<Impl>(std::move(atomicFileStore))} {}

WindowsFileSystem::~WindowsFileSystem() = default;

Domain::Result<std::vector<std::byte>>
WindowsFileSystem::readFile(const Contracts::AuthorizedPath &path,
                            const std::size_t maximumBytes,
                            const Domain::OperationContext &context) noexcept {
  try {
    auto bound = validateBound(maximumBytes, MaximumTextFileBytes,
                               "The file read bound");
    if (!bound) {
      return Domain::Result<std::vector<std::byte>>::failure(
          std::move(bound).error());
    }
    if (!implementation_->atomicFileStore) {
      return Domain::Result<std::vector<std::byte>>::failure(
          Domain::makeError(
              Domain::ErrorCodes::InvalidRequest,
              "The native filesystem requires an atomic content store owner."));
    }
    auto content = implementation_->atomicFileStore->read(
        path, maximumBytes, context);
    if (!content) {
      return content;
    }
    auto utf8 =
        Detail::validateUtf8Text(byteText(content.value()), "The file content");
    if (!utf8) {
      return Domain::Result<std::vector<std::byte>>::failure(
          std::move(utf8).error());
    }
    return content;
  } catch (...) {
    return Domain::Result<std::vector<std::byte>>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The native text file could not be read."));
  }
}

Domain::Result<void>
WindowsFileSystem::writeFile(const Contracts::AuthorizedPath &path,
                             const std::span<const std::byte> content,
                             const Domain::OperationContext &context) noexcept {
  try {
    if (content.size() > MaximumTextFileBytes) {
      return Domain::Result<void>::failure(
          Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                            "Native text writes are limited to 2 MiB."));
    }
    auto utf8 = Detail::validateUtf8Text(byteText(content), "The file content");
    if (!utf8) {
      return utf8;
    }
    if (!implementation_->atomicFileStore) {
      return Domain::Result<void>::failure(Domain::makeError(
          Domain::ErrorCodes::InvalidRequest,
          "The native filesystem requires an atomic content store owner."));
    }
    auto parents = Detail::ensureAuthorizedParentDirectories(path, context);
    if (!parents) {
      return parents;
    }
    return implementation_->atomicFileStore->replace(
        path, content, false, context);
  } catch (...) {
    return Domain::Result<void>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The native text file could not be written."));
  }
}

Domain::Result<Domain::DirectoryListing>
WindowsFileSystem::list(const Contracts::AuthorizedPath &directory,
                        const std::size_t maximumEntries,
                        const Domain::OperationContext &context) noexcept {
  try {
    auto bound = validateBound(maximumEntries, MaximumListEntries,
                               "The directory listing bound");
    if (!bound) {
      return Domain::Result<Domain::DirectoryListing>::failure(
          std::move(bound).error());
    }
    auto opened = Detail::openAuthorizedDirectory(directory, context);
    if (!opened) {
      return Domain::Result<Domain::DirectoryListing>::failure(
          std::move(opened).error());
    }
    auto entries = Detail::enumerateDirectory(opened.value().handle.get(),
                                               MaximumEnumeratedEntries + 1U,
                                               context);
    if (!entries) {
      return Domain::Result<Domain::DirectoryListing>::failure(
          std::move(entries).error());
    }
    if (entries.value().size() > MaximumEnumeratedEntries) {
      return Domain::Result<Domain::DirectoryListing>::failure(
          Domain::makeError(
              Domain::ErrorCodes::LimitExceeded,
              "The directory exceeds the native enumeration safety bound."));
    }
    const bool truncated = entries.value().size() > maximumEntries;
    if (truncated) {
      entries.value().resize(maximumEntries);
    }
    std::vector<Domain::PathText> result;
    result.reserve(entries.value().size());
    for (const auto &entry : entries.value()) {
      auto valid = InfrastructureDetail::validateOperationContext(
          context, std::chrono::steady_clock::now(),
          "materialize a bounded directory listing");
      if (!valid) {
        return Domain::Result<Domain::DirectoryListing>::failure(
            std::move(valid).error());
      }
      std::wstring path{opened.value().canonicalPath};
      path.push_back(L'\\');
      path.append(entry.name);
      auto converted =
          InfrastructureDetail::WindowsPathResolver::toPathText(path);
      if (!converted) {
        return Domain::Result<Domain::DirectoryListing>::failure(
            std::move(converted).error());
      }
      result.push_back(std::move(converted).value());
    }
    return Domain::Result<Domain::DirectoryListing>::success(
        Domain::DirectoryListing{std::move(result), truncated});
  } catch (...) {
    return Domain::Result<Domain::DirectoryListing>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The authorized directory could not be listed."));
  }
}

Domain::Result<void> WindowsFileSystem::createDirectory(
    const Contracts::AuthorizedPath &directory,
    const Domain::OperationContext &context) noexcept {
  try {
    auto target =
        InfrastructureDetail::WindowsPathResolver::resolveAuthorizedPath(
            directory, Domain::FileAccess::Create,
            InfrastructureDetail::MissingPathPolicy::AllowDescendants);
    if (!target) {
      return Domain::Result<void>::failure(std::move(target).error());
    }
    auto root = InfrastructureDetail::WindowsPathResolver::resolveAppOwnedRoot(
        directory.authorityRoot().value());
    if (!root) {
      return Domain::Result<void>::failure(std::move(root).error());
    }
    if (Detail::samePath(target.value(), root.value())) {
      return Domain::Result<void>::success();
    }
    auto current = Detail::openCanonicalDirectory(
        root.value(),
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES |
            FILE_ADD_SUBDIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD,
        FILE_SHARE_READ, context);
    if (!current) {
      return Domain::Result<void>::failure(std::move(current).error());
    }
    const std::size_t relativeStart = root.value().size() + 1U;
    if (relativeStart >= target.value().size()) {
      return Domain::Result<void>::failure(Domain::makeError(
          Domain::ErrorCodes::PathOutsideAuthority,
          "The directory target is not below its authority root."));
    }
    std::size_t start = relativeStart;
    std::wstring currentPath{root.value()};
    while (start < target.value().size()) {
      const auto separator = target.value().find(L'\\', start);
      const auto end =
          separator == std::wstring::npos ? target.value().size() : separator;
      const auto component =
          std::wstring_view{target.value()}.substr(start, end - start);
      currentPath.push_back(L'\\');
      currentPath.append(component);
      auto next = Detail::openOrCreateChildDirectory(
          current.value().handle.get(), component, currentPath, context);
      if (!next) {
        return Domain::Result<void>::failure(std::move(next).error());
      }
      current = std::move(next);
      if (separator == std::wstring::npos) {
        break;
      }
      start = separator + 1U;
    }
    return Domain::Result<void>::success();
  } catch (...) {
    return Domain::Result<void>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The authorized directory could not be created."));
  }
}

Domain::Result<void>
WindowsFileSystem::remove(const Contracts::AuthorizedPath &path,
                          const bool recursive,
                          const Domain::OperationContext &context) noexcept {
  try {
    auto resolved =
        InfrastructureDetail::WindowsPathResolver::resolveAuthorizedPath(
            path, Domain::FileAccess::Delete,
            InfrastructureDetail::MissingPathPolicy::Reject);
    if (!resolved) {
      return Domain::Result<void>::failure(std::move(resolved).error());
    }
    auto protectedRoot =
        rejectAuthorityRoot(resolved.value(), path.authorityRoot());
    if (!protectedRoot) {
      return protectedRoot;
    }
    auto opened = Detail::openAuthorizedObject(
        path, Domain::FileAccess::Delete,
        InfrastructureDetail::MissingPathPolicy::Reject, context,
        DELETE | FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (!opened) {
      return Domain::Result<void>::failure(std::move(opened).error());
    }
    if (opened.value().isDirectory() && recursive) {
      std::size_t validated{};
      auto safeTree =
          validateTree(opened.value().handle.get(),
                       opened.value().canonicalPath, 1U, validated, context);
      if (!safeTree) {
        return safeTree;
      }
      std::size_t removed{};
      auto emptied =
          removeTree(opened.value().handle.get(), opened.value().canonicalPath,
                     1U, removed, context);
      if (!emptied) {
        return emptied;
      }
    }
    return Detail::deleteOpenedObject(opened.value().handle.get(), context);
  } catch (...) {
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The authorized filesystem object could not be removed."));
  }
}

Domain::Result<void>
WindowsFileSystem::move(const Contracts::AuthorizedPath &source,
                        const Contracts::AuthorizedPath &destination,
                        const Domain::OperationContext &context) noexcept {
  try {
    if (source.authorityId() != destination.authorityId() ||
        source.authorityRoot() != destination.authorityRoot() ||
        source.access() != Domain::FileAccess::Delete ||
        destination.access() != Domain::FileAccess::Create) {
      return Domain::Result<void>::failure(
          Domain::makeError(Domain::ErrorCodes::Unauthorized,
                            "A native move requires delete/create paths from "
                            "one authority root."));
    }
    auto sourcePath =
        InfrastructureDetail::WindowsPathResolver::resolveAuthorizedPath(
            source, Domain::FileAccess::Delete,
            InfrastructureDetail::MissingPathPolicy::Reject);
    if (!sourcePath) {
      return Domain::Result<void>::failure(std::move(sourcePath).error());
    }
    auto destinationPath =
        InfrastructureDetail::WindowsPathResolver::resolveAuthorizedPath(
            destination, Domain::FileAccess::Create,
            InfrastructureDetail::MissingPathPolicy::AllowDescendants);
    if (!destinationPath) {
      return Domain::Result<void>::failure(std::move(destinationPath).error());
    }
    auto sourceRoot =
        rejectAuthorityRoot(sourcePath.value(), source.authorityRoot());
    if (!sourceRoot) {
      return sourceRoot;
    }
    auto destinationRoot = rejectAuthorityRoot(destinationPath.value(),
                                               destination.authorityRoot());
    if (!destinationRoot) {
      return destinationRoot;
    }
    if (Detail::samePath(sourcePath.value(), destinationPath.value())) {
      return Domain::Result<void>::failure(Domain::makeError(
          Domain::ErrorCodes::Conflict,
          "The move source and destination are the same path."));
    }
    auto openedSource = Detail::openAuthorizedObject(
        source, Domain::FileAccess::Delete,
        InfrastructureDetail::MissingPathPolicy::Reject, context,
        DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (!openedSource) {
      return Domain::Result<void>::failure(std::move(openedSource).error());
    }
    auto parents =
        Detail::ensureAuthorizedParentDirectories(destination, context);
    if (!parents) {
      return parents;
    }
    destinationPath =
        InfrastructureDetail::WindowsPathResolver::resolveAuthorizedPath(
            destination, Domain::FileAccess::Create,
            InfrastructureDetail::MissingPathPolicy::AllowLeaf);
    if (!destinationPath) {
      return Domain::Result<void>::failure(std::move(destinationPath).error());
    }
    if (!openedSource.value().authorizedPathOwner.has_value()) {
      return Domain::Result<void>::failure(Domain::makeError(
          Domain::ErrorCodes::IntegrityFailure,
          "The move source did not retain its authorized ancestry."));
    }
    auto destinationParent = openMoveDestinationParent(
        destinationPath.value(), destination.authorityRoot(), context);
    if (!destinationParent) {
      return Domain::Result<void>::failure(
          std::move(destinationParent).error());
    }
    const auto separator = destinationPath.value().find_last_of(L'\\');
    if (separator == std::wstring::npos ||
        separator + 1U >= destinationPath.value().size()) {
      return Domain::Result<void>::failure(
          Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                            "The move destination has no leaf component."));
    }
    return renameOpenedObject(
        openedSource.value().handle.get(),
        destinationParent.value().handle(),
        std::wstring_view{destinationPath.value()}.substr(separator + 1U),
        Detail::samePath(
            std::wstring_view{sourcePath.value()}.substr(
                0U, sourcePath.value().find_last_of(L'\\')),
            std::wstring_view{destinationPath.value()}.substr(0U, separator)),
        context);
  } catch (...) {
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The authorized filesystem object could not be moved."));
  }
}

Domain::Result<Contracts::TextFileEditReport> WindowsFileSystem::replaceAll(
    const Contracts::AuthorizedPath &readablePath,
    const Contracts::AuthorizedPath &writablePath,
    const std::string_view oldText, const std::string_view replacementText,
    const Domain::OperationContext &context) noexcept {
  try {
    if (!sameAuthorizedObject(readablePath, writablePath) ||
        readablePath.access() != Domain::FileAccess::Read ||
        writablePath.access() != Domain::FileAccess::Write) {
      return Domain::Result<Contracts::TextFileEditReport>::failure(
          Domain::makeError(
              Domain::ErrorCodes::Unauthorized,
              "Text editing requires matching read and write authorities."));
    }
    if (oldText.empty()) {
      return Domain::Result<Contracts::TextFileEditReport>::failure(
          Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                            "The text-edit search value cannot be empty."));
    }
    if (oldText.size() > MaximumTextFileBytes ||
        replacementText.size() > MaximumTextFileBytes) {
      return Domain::Result<Contracts::TextFileEditReport>::failure(
          Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                            "Text-edit values are limited to 2 MiB."));
    }
    auto validOld = Detail::validateUtf8Text(oldText, "The old text");
    if (!validOld) {
      return Domain::Result<Contracts::TextFileEditReport>::failure(
          std::move(validOld).error());
    }
    auto validNew =
        Detail::validateUtf8Text(replacementText, "The replacement text");
    if (!validNew) {
      return Domain::Result<Contracts::TextFileEditReport>::failure(
          std::move(validNew).error());
    }
    auto bytes = readFile(readablePath, MaximumTextFileBytes, context);
    if (!bytes) {
      return Domain::Result<Contracts::TextFileEditReport>::failure(
          std::move(bytes).error());
    }
    const auto originalView = byteText(bytes.value());
    std::size_t replacements{};
    std::size_t position{};
    std::size_t outputBytes{};
    while (position <= originalView.size()) {
      const auto match = originalView.find(oldText, position);
      if (match == std::string_view::npos) {
        outputBytes += originalView.size() - position;
        break;
      }
      ++replacements;
      const auto prefix = match - position;
      if (prefix > MaximumTextFileBytes - outputBytes ||
          replacementText.size() >
              MaximumTextFileBytes - outputBytes - prefix) {
        return Domain::Result<Contracts::TextFileEditReport>::failure(
            Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                              "The edited text would exceed 2 MiB."));
      }
      outputBytes += prefix + replacementText.size();
      position = match + oldText.size();
    }
    if (replacements == 0U) {
      return Domain::Result<Contracts::TextFileEditReport>::failure(
          Domain::makeError(Domain::ErrorCodes::RecordNotFound,
                            "The text-edit search value was not found."));
    }
    if (outputBytes > MaximumTextFileBytes) {
      return Domain::Result<Contracts::TextFileEditReport>::failure(
          Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                            "The edited text would exceed 2 MiB."));
    }
    std::string edited;
    edited.reserve(outputBytes);
    position = 0U;
    for (;;) {
      const auto match = originalView.find(oldText, position);
      if (match == std::string_view::npos) {
        edited.append(originalView.substr(position));
        break;
      }
      edited.append(originalView.substr(position, match - position));
      edited.append(replacementText);
      position = match + oldText.size();
    }
    const std::span<const std::byte> editedBytes{
        reinterpret_cast<const std::byte *>(edited.data()), edited.size()};
    auto written = writeFile(writablePath, editedBytes, context);
    if (!written) {
      return Domain::Result<Contracts::TextFileEditReport>::failure(
          std::move(written).error());
    }
    return Domain::Result<Contracts::TextFileEditReport>::success(
        Contracts::TextFileEditReport{replacements, edited.size()});
  } catch (...) {
    return Domain::Result<Contracts::TextFileEditReport>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "The native text edit failed."));
  }
}

} // namespace ForgeConductor::NativeTools::Windows
