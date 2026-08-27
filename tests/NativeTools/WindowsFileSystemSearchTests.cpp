#include "Infrastructure/TestSupport.h"

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/NativeTools/Windows/WindowsFileSystem.h"
#include "ForgeConductor/NativeTools/Windows/WindowsPathGlobService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsTextSearchService.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "NativeTools/Windows/NativeFileOperations.h"

#include <Windows.h>
#include <winioctl.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <semaphore>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

namespace NativeWindows = ForgeConductor::NativeTools::Windows;
namespace InfrastructureDetail =
    ForgeConductor::Infrastructure::Windows::Detail;
namespace NativeDetail = ForgeConductor::NativeTools::Windows::Detail;

using NativeWindows::WindowsFileSystem;
using NativeWindows::WindowsPathGlobService;
using NativeWindows::WindowsTextSearchService;

static_assert(std::is_final_v<WindowsFileSystem>);
static_assert(std::is_final_v<WindowsPathGlobService>);
static_assert(std::is_final_v<WindowsTextSearchService>);
static_assert(!std::is_copy_constructible_v<WindowsFileSystem>);

[[nodiscard]] Domain::PathText pathText(const std::filesystem::path &path) {
  return take(Domain::PathText::create(
      take(InfrastructureDetail::strictUtf16ToUtf8(path.native()))));
}

class ScopedDirectory final {
public:
  ScopedDirectory() {
    std::wstring temporary(32U * 1024U, L'\0');
    const DWORD length =
        ::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
    require(length != 0U && length < temporary.size(),
            "GetTempPathW failed for the native-tool test root");
    temporary.resize(length);
    path_ = std::filesystem::path{temporary} /
            (L"ForgeConductor-P13-Filesystem-" +
             std::to_wstring(::GetCurrentProcessId()) + L"-" +
             std::to_wstring(::GetTickCount64()));
    require(std::filesystem::create_directories(path_),
            "the native-tool test root could not be created");
    path_ = std::filesystem::canonical(path_);
  }

  ~ScopedDirectory() noexcept {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove_all(path_, ignored));
  }

  ScopedDirectory(const ScopedDirectory &) = delete;
  ScopedDirectory &operator=(const ScopedDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class TestAuthority final : public Contracts::IWorkspaceAuthority {
public:
  explicit TestAuthority(const Domain::PathText &root)
      : root_{root},
        authority_{take(issueAuthority(
            parse<Domain::AuthorityId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
            parse<Domain::ProjectId>("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
            parse<Domain::ClientId>("p13-filesystem-client"), {root_},
            Domain::FileAccess::Read,
            {Domain::FileAccess::Read, Domain::FileAccess::Write,
             Domain::FileAccess::Create, Domain::FileAccess::Delete},
            {}, false, 1U))} {}

  [[nodiscard]] Contracts::AuthorizedPath
  path(const std::filesystem::path &value,
       const Domain::FileAccess access) const {
    return take(
        issueAuthorizedPath(authority_, pathText(value), root_, access));
  }

  [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority>
  authorityFor(const Domain::ProjectId &,
               const Domain::OperationContext &) noexcept override {
    return Domain::Result<Contracts::WorkspaceAuthority>::success(authority_);
  }

  [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority>
  narrow(const Contracts::WorkspaceAuthority &,
         const std::vector<Domain::PathText> &,
         const std::vector<Domain::FileAccess> &, bool, std::uint64_t,
         const Domain::OperationContext &) noexcept override {
    return Domain::Result<Contracts::WorkspaceAuthority>::failure(
        Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                          "The test issuer does not narrow authority."));
  }

  [[nodiscard]] Domain::Result<Contracts::AuthorizedPath>
  authorize(const Contracts::WorkspaceAuthority &authority,
            const Domain::PathAuthorizationRequest &request,
            const Domain::OperationContext &) noexcept override {
    return issueAuthorizedPath(authority, request.requestedPath, root_,
                               request.access);
  }

private:
  Domain::PathText root_;
  Contracts::WorkspaceAuthority authority_;
};

[[nodiscard]] Domain::OperationContext
activeContext(const std::stop_token cancellation = {}) {
  return Domain::OperationContext{
      parse<Domain::OperationId>("11111111-1111-4111-8111-111111111111"),
      std::chrono::steady_clock::now() + std::chrono::minutes{5}, cancellation,
      parse<Domain::CorrelationId>("p13-filesystem-search")};
}

[[nodiscard]] Domain::OperationContext expiredContext() {
  auto context = activeContext();
  context.deadline = std::chrono::steady_clock::now();
  return context;
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value) {
  std::vector<std::byte> result(value.size());
  std::memcpy(result.data(), value.data(), value.size());
  return result;
}

[[nodiscard]] std::string text(const std::vector<std::byte> &value) {
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

void writeFixture(const std::filesystem::path &path,
                  const std::string_view content) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  require(static_cast<bool>(output), "a native-tool fixture could not open");
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  require(static_cast<bool>(output), "a native-tool fixture could not write");
}

struct MountPointReparseData final {
  DWORD tag{};
  WORD dataLength{};
  WORD reserved{};
  WORD substituteNameOffset{};
  WORD substituteNameLength{};
  WORD printNameOffset{};
  WORD printNameLength{};
  wchar_t pathBuffer[1]{};
};

void makeJunction(const std::filesystem::path &junction,
                  const std::filesystem::path &target) {
  require(std::filesystem::create_directory(junction),
          "the native-tool junction placeholder could not be created");
  InfrastructureDetail::UniqueHandle handle{::CreateFileW(
      junction.c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  require(static_cast<bool>(handle),
          "the native-tool junction placeholder could not be opened");
  const std::wstring substitute =
      L"\\??\\" + std::filesystem::canonical(target).native();
  const std::wstring printName = std::filesystem::canonical(target).native();
  const std::size_t substituteBytes = substitute.size() * sizeof(wchar_t);
  const std::size_t printBytes = printName.size() * sizeof(wchar_t);
  const std::size_t pathBytes =
      substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
  const std::size_t totalBytes =
      offsetof(MountPointReparseData, pathBuffer) + pathBytes;
  require(totalBytes <= (std::numeric_limits<DWORD>::max)() &&
              pathBytes + 8U <= (std::numeric_limits<WORD>::max)(),
          "the native-tool junction payload exceeds Windows bounds");
  std::vector<std::uint64_t> storage((totalBytes + sizeof(std::uint64_t) - 1U) /
                                     sizeof(std::uint64_t));
  auto *data = reinterpret_cast<MountPointReparseData *>(storage.data());
  data->tag = IO_REPARSE_TAG_MOUNT_POINT;
  data->dataLength = static_cast<WORD>(pathBytes + 8U);
  data->substituteNameLength = static_cast<WORD>(substituteBytes);
  data->printNameOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
  data->printNameLength = static_cast<WORD>(printBytes);
  std::memcpy(data->pathBuffer, substitute.data(), substituteBytes);
  data->pathBuffer[substitute.size()] = L'\0';
  std::memcpy(reinterpret_cast<std::byte *>(data->pathBuffer) +
                  data->printNameOffset,
              printName.data(), printBytes);
  data->pathBuffer[data->printNameOffset / sizeof(wchar_t) + printName.size()] =
      L'\0';
  DWORD returned{};
  require(::DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, data,
                            static_cast<DWORD>(totalBytes), nullptr, 0U,
                            &returned, nullptr) != FALSE,
          "the native-tool directory junction could not be created");
}

void filesystemRoundTripAndMutations() {
  ScopedDirectory temporary;
  const auto root = temporary.path() / L"workspace";
  require(std::filesystem::create_directory(root),
          "the filesystem workspace could not be created");
  TestAuthority authority{pathText(root)};
  WindowsFileSystem fileSystem{
      std::make_shared<Infrastructure::Windows::WindowsAtomicFileStore>()};
  const auto context = activeContext();

  const auto implicitlyNested =
      root / L"write-created" / L"parents" / L"document.txt";
  require(fileSystem
              .writeFile(
                  authority.path(implicitlyNested, Domain::FileAccess::Create),
                  bytes("parents created\n"), context)
              .hasValue(),
          "native file creation did not create missing parent directories");
  require(std::filesystem::is_regular_file(implicitlyNested),
          "native file creation did not publish through nested parents");

  const auto nested = root / L"nested" / L"deeper";
  require(fileSystem
              .createDirectory(
                  authority.path(nested, Domain::FileAccess::Create), context)
              .hasValue(),
          "recursive directory creation failed");
  require(std::filesystem::is_directory(nested),
          "recursive directory creation did not materialize its target");

  const auto document = nested / L"document.txt";
  const auto initial = bytes("alpha beta alpha\nsecond line\n");
  require(fileSystem
              .writeFile(authority.path(document, Domain::FileAccess::Create),
                         initial, context)
              .hasValue(),
          "bounded UTF-8 file creation failed");
  const auto loaded = take(
      fileSystem.readFile(authority.path(document, Domain::FileAccess::Read),
                          WindowsFileSystem::MaximumTextFileBytes, context));
  require(text(loaded) == "alpha beta alpha\nsecond line\n",
          "native file read/write did not round-trip UTF-8 content");

  const auto edit = take(
      fileSystem.replaceAll(authority.path(document, Domain::FileAccess::Read),
                            authority.path(document, Domain::FileAccess::Write),
                            "alpha", "omega", context));
  require(edit.replacements == 2U &&
              text(take(fileSystem.readFile(
                  authority.path(document, Domain::FileAccess::Read),
                  WindowsFileSystem::MaximumTextFileBytes, context))) ==
                  "omega beta omega\nsecond line\n",
          "native replace-all did not report and publish both replacements");
  requireError(
      fileSystem.replaceAll(authority.path(document, Domain::FileAccess::Read),
                            authority.path(document, Domain::FileAccess::Write),
                            "missing", "value", context),
      Domain::ErrorCodes::RecordNotFound,
      "native replace-all accepted a missing search value");

  const auto moved = nested / L"renamed.txt";
  const auto moveResult = fileSystem.move(
      authority.path(document, Domain::FileAccess::Delete),
      authority.path(moved, Domain::FileAccess::Create), context);
  require(moveResult.hasValue(),
          moveResult.hasValue()
              ? "handle-relative file move failed"
              : "handle-relative file move failed: " + moveResult.error().code +
                    ": " + moveResult.error().message);
  require(!std::filesystem::exists(document) &&
              std::filesystem::is_regular_file(moved),
          "handle-relative move did not preserve exactly one live path");

  const auto listing = take(fileSystem.list(
      authority.path(nested, Domain::FileAccess::Read), 10U, context));
  require(!listing.truncated && listing.entries.size() == 1U &&
              listing.entries.front() == pathText(moved),
          "bounded directory listing returned the wrong canonical entry");

  const auto nestedMove =
      root / L"move-created" / L"parents" / L"renamed.txt";
  require(fileSystem
              .move(authority.path(moved, Domain::FileAccess::Delete),
                    authority.path(nestedMove, Domain::FileAccess::Create),
                    context)
              .hasValue(),
          "native move did not create missing destination parents");
  require(std::filesystem::is_regular_file(nestedMove) &&
              !std::filesystem::exists(moved),
          "native move did not publish through nested destination parents");
  require(fileSystem
              .move(authority.path(nestedMove, Domain::FileAccess::Delete),
                    authority.path(moved, Domain::FileAccess::Create), context)
              .hasValue(),
          "native move could not return from an implicitly created parent");

  const auto crossDirectory = root / L"cross-directory.txt";
  const auto crossMove = fileSystem.move(
      authority.path(moved, Domain::FileAccess::Delete),
      authority.path(crossDirectory, Domain::FileAccess::Create), context);
  require(crossMove.hasValue(),
          crossMove.hasValue()
              ? "cross-directory handle-relative move failed"
              : "cross-directory handle-relative move failed: " +
                    crossMove.error().code + ": " + crossMove.error().message);
  require(fileSystem
              .move(authority.path(crossDirectory, Domain::FileAccess::Delete),
                    authority.path(moved, Domain::FileAccess::Create), context)
              .hasValue(),
          "reverse cross-directory handle-relative move failed");

  const auto directorySource = root / L"directory-source";
  const auto directoryChild = directorySource / L"child.txt";
  require(fileSystem
              .writeFile(
                  authority.path(directoryChild, Domain::FileAccess::Create),
                  bytes("directory move\n"), context)
              .hasValue(),
          "directory-move fixture creation failed");
  const auto directoryDestination = nested / L"moved-directory";
  const auto directoryMove = fileSystem.move(
      authority.path(directorySource, Domain::FileAccess::Delete),
      authority.path(directoryDestination, Domain::FileAccess::Create), context);
  require(directoryMove.hasValue(),
          directoryMove.hasValue()
              ? "cross-directory directory move failed"
              : "cross-directory directory move failed: " +
                    directoryMove.error().code + ": " +
                    directoryMove.error().message);
  require(!std::filesystem::exists(directorySource) &&
              std::filesystem::is_regular_file(
                  directoryDestination / L"child.txt"),
          "cross-directory directory move did not preserve its subtree");

  const auto disposable = root / L"disposable";
  const auto child = disposable / L"child";
  require(fileSystem
              .createDirectory(
                  authority.path(child, Domain::FileAccess::Create), context)
              .hasValue(),
          "recursive-delete fixture directory creation failed");
  const auto childFile = child / L"value.txt";
  require(fileSystem
              .writeFile(authority.path(childFile, Domain::FileAccess::Create),
                         bytes("value"), context)
              .hasValue(),
          "recursive-delete fixture write failed");
  requireError(
      fileSystem.remove(authority.path(disposable, Domain::FileAccess::Delete),
                        false, context),
      Domain::ErrorCodes::Conflict,
      "non-recursive removal deleted a non-empty directory");
  require(fileSystem
              .remove(authority.path(disposable, Domain::FileAccess::Delete),
                      true, context)
              .hasValue(),
          "bounded recursive directory removal failed");
  require(!std::filesystem::exists(disposable),
          "recursive directory removal left its root behind");
  require(fileSystem
              .remove(authority.path(moved, Domain::FileAccess::Delete), false,
                      context)
              .hasValue(),
          "single-file removal failed");
}

void globAndTextSearchAreBoundedAndDeterministic() {
  ScopedDirectory temporary;
  const auto root = temporary.path() / L"workspace";
  const auto source = root / L"src";
  const auto nested = source / L"nested";
  const auto git = root / L".git";
  const auto modules = root / L"node_modules";
  require(std::filesystem::create_directories(nested) &&
              std::filesystem::create_directory(git) &&
              std::filesystem::create_directory(modules),
          "the search fixture tree could not be created");
  writeFixture(root / L"top.txt", "needle at top\n");
  writeFixture(source / L"alpha.txt", "first\nneedle alpha\n");
  writeFixture(nested / L"beta.txt", "needle beta\n");
  writeFixture(source / L"ignore.bin", "not relevant\n");
  writeFixture(git / L"hidden.txt", "needle hidden\n");
  writeFixture(modules / L"dependency.txt", "needle dependency\n");

  TestAuthority authority{pathText(root)};
  const auto rootRead = authority.path(root, Domain::FileAccess::Read);
  const auto context = activeContext();
  WindowsPathGlobService glob;
  WindowsTextSearchService search;

  const auto matches =
      take(glob.glob(rootRead, "**/*.txt", 20U,
                     WindowsPathGlobService::MaximumResponseBytes, context));
  require(matches.size() == 5U,
          "segment-aware recursive glob did not include every TXT path");
  require(matches.front() == pathText(git / L"hidden.txt") &&
              matches.back() == pathText(root / L"top.txt"),
          "native glob results are not deterministically sorted");
  const auto basenameMatches =
      take(glob.glob(rootRead, "*.txt", 20U,
                     WindowsPathGlobService::MaximumResponseBytes, context));
  require(basenameMatches == matches,
          "separator-free glob did not recursively match nested basenames");
  const auto defaultMatches =
      take(glob.glob(rootRead, "*", 20U,
                     WindowsPathGlobService::MaximumResponseBytes, context));
  const std::vector<Domain::PathText> expectedDefaultMatches{
      pathText(git),
      pathText(git / L"hidden.txt"),
      pathText(modules),
      pathText(modules / L"dependency.txt"),
      pathText(source),
      pathText(source / L"alpha.txt"),
      pathText(source / L"ignore.bin"),
      pathText(nested),
      pathText(nested / L"beta.txt"),
      pathText(root / L"top.txt"),
  };
  require(defaultMatches == expectedDefaultMatches,
          "default glob did not recursively match every basename in order");
  const auto relativePathMatches =
      take(glob.glob(rootRead, "src/*.txt", 20U,
                     WindowsPathGlobService::MaximumResponseBytes, context));
  require(relativePathMatches.size() == 1U &&
              relativePathMatches.front() == pathText(source / L"alpha.txt"),
          "separator glob no longer used root-relative path semantics");
  const auto limited =
      take(glob.glob(rootRead, "**/*.txt", 2U,
                     WindowsPathGlobService::MaximumResponseBytes, context));
  require(limited.size() == 2U && limited[0] == matches[0] &&
              limited[1] == matches[1],
          "glob match capping changed deterministic prefix order");
  requireError(glob.glob(rootRead, "../*.txt", 10U, 1'024U, context),
               Domain::ErrorCodes::InvalidRequest,
               "glob accepted a parent-traversal pattern");
  requireError(
      glob.glob(rootRead, "*", WindowsPathGlobService::MaximumMatches + 1U,
                1'024U, context),
      Domain::ErrorCodes::LimitExceeded, "glob accepted a match cap above 500");

  const auto found = take(
      search.search(rootRead, "needle", 20U,
                    WindowsTextSearchService::MaximumResponseBytes, context));
  require(found.size() == 3U,
          "text search did not exclude .git and node_modules exactly");
  require(
      found[0].find("alpha.txt:2:needle alpha") != std::string::npos &&
          found[1].find("beta.txt:1:needle beta") != std::string::npos &&
          found[2].find("top.txt:1:needle at top") != std::string::npos,
      "text-search output or deterministic ordering differs from grep form");
  const auto regexFound = take(
      search.search(rootRead, "^needle b.*", 20U,
                    WindowsTextSearchService::MaximumResponseBytes, context));
  require(regexFound.size() == 1U &&
              regexFound.front().find("beta.txt:1:needle beta") !=
                  std::string::npos,
          "text search did not preserve POSIX basic-regex behavior");
  requireError(search.search(rootRead, "[", 20U, 1'024U, context),
               Domain::ErrorCodes::InvalidRequest,
               "text search accepted an invalid basic regular expression");
  const auto one = take(
      search.search(rootRead, "needle", 1U,
                    WindowsTextSearchService::MaximumResponseBytes, context));
  require(one.size() == 1U && one.front() == found.front(),
          "text-search match capping changed deterministic prefix order");
  requireError(search.search(rootRead, "needle", 201U, 1'024U, context),
               Domain::ErrorCodes::LimitExceeded,
               "text search accepted a match cap above 200");
}

void authorityReparseAndRootProtections() {
  ScopedDirectory temporary;
  const auto root = temporary.path() / L"workspace";
  const auto outside = temporary.path() / L"outside";
  require(std::filesystem::create_directory(root) &&
              std::filesystem::create_directory(outside),
          "the authority fixture roots could not be created");
  writeFixture(outside / L"secret.txt", "secret\n");
  TestAuthority authority{pathText(root)};
  WindowsFileSystem fileSystem{
      std::make_shared<Infrastructure::Windows::WindowsAtomicFileStore>()};
  WindowsPathGlobService glob;
  WindowsTextSearchService search;
  const auto context = activeContext();

  requireError(fileSystem.readFile(authority.path(outside / L"secret.txt",
                                                  Domain::FileAccess::Read),
                                   1'024U, context),
               Domain::ErrorCodes::PathOutsideAuthority,
               "filesystem read escaped its trusted authority root");
  requireError(
      fileSystem.remove(authority.path(root, Domain::FileAccess::Delete), true,
                        context),
      Domain::ErrorCodes::Unauthorized,
      "recursive removal accepted the authority root itself");
  require(std::filesystem::is_directory(root),
          "authority-root removal changed the protected root");

  const auto tree = root / L"tree";
  require(std::filesystem::create_directory(tree),
          "the reparse-protection tree could not be created");
  const auto junction = tree / L"redirect";
  makeJunction(junction, outside);
  requireError(
      fileSystem.list(authority.path(junction, Domain::FileAccess::Read), 10U,
                      context),
      Domain::ErrorCodes::PathOutsideAuthority,
      "directory listing traversed a final-component junction");
  requireError(
      fileSystem.remove(authority.path(tree, Domain::FileAccess::Delete), true,
                        context),
      Domain::ErrorCodes::PathOutsideAuthority,
      "recursive removal accepted a tree containing a junction");
  require(std::filesystem::is_regular_file(outside / L"secret.txt"),
          "reparse rejection failed to protect the junction target");
  require(take(glob.glob(authority.path(root, Domain::FileAccess::Read),
                         "**/*.txt", 20U, 16U * 1'024U, context))
              .empty(),
          "glob traversal followed a directory junction");
  require(take(search.search(authority.path(root, Domain::FileAccess::Read),
                             "secret", 20U, 16U * 1'024U, context))
              .empty(),
          "text search followed a directory junction");
}

void boundsUtf8CancellationAndDeadline() {
  ScopedDirectory temporary;
  const auto root = temporary.path() / L"workspace";
  require(std::filesystem::create_directory(root),
          "the bounds fixture root could not be created");
  const auto file = root / L"value.txt";
  writeFixture(file, "value\n");
  TestAuthority authority{pathText(root)};
  WindowsFileSystem fileSystem{
      std::make_shared<Infrastructure::Windows::WindowsAtomicFileStore>()};
  WindowsPathGlobService glob;
  WindowsTextSearchService search;
  const auto context = activeContext();

  requireError(fileSystem.readFile(
                   authority.path(file, Domain::FileAccess::Read),
                   WindowsFileSystem::MaximumTextFileBytes + 1U, context),
               Domain::ErrorCodes::LimitExceeded,
               "filesystem read accepted a bound above 2 MiB");
  std::vector<std::byte> oversized(WindowsFileSystem::MaximumTextFileBytes + 1U,
                                   std::byte{'x'});
  requireError(fileSystem.writeFile(authority.path(root / L"oversized.txt",
                                                   Domain::FileAccess::Create),
                                    oversized, context),
               Domain::ErrorCodes::PayloadTooLarge,
               "filesystem write accepted content above 2 MiB");
  const std::vector<std::byte> malformed{std::byte{0xc3}, std::byte{0x28}};
  requireError(fileSystem.writeFile(authority.path(root / L"malformed.txt",
                                                   Domain::FileAccess::Create),
                                    malformed, context),
               Domain::ErrorCodes::InvalidRequest,
               "filesystem write accepted malformed UTF-8");

  const auto second = root / L"second.txt";
  writeFixture(second, "second\n");
  const auto limitedListing = take(fileSystem.list(
      authority.path(root, Domain::FileAccess::Read), 1U, context));
  require(limitedListing.truncated && limitedListing.entries.size() == 1U &&
              limitedListing.entries.front() == pathText(second),
          "directory listing did not return a sorted truncated prefix");

  std::stop_source stopped;
  stopped.request_stop();
  const auto cancelled = activeContext(stopped.get_token());
  requireError(
      fileSystem.readFile(authority.path(file, Domain::FileAccess::Read),
                          1'024U, cancelled),
      Domain::ErrorCodes::Cancelled, "filesystem read ignored cancellation");
  requireError(glob.glob(authority.path(root, Domain::FileAccess::Read), "**",
                         10U, 1'024U, cancelled),
               Domain::ErrorCodes::Cancelled,
               "native glob ignored cancellation");
  requireError(search.search(authority.path(root, Domain::FileAccess::Read),
                             "value", 10U, 1'024U, expiredContext()),
               Domain::ErrorCodes::DeadlineExceeded,
               "native text search ignored an expired deadline");
}

void authorizedOpenPinsAncestorsAgainstRename() {
  ScopedDirectory temporary;
  const auto root = temporary.path() / L"workspace";
  const auto pinnedParent = root / L"pinned";
  const auto renamedParent = root / L"renamed";
  require(std::filesystem::create_directories(pinnedParent),
          "the anchor-lifetime fixture could not be created");
  const auto file = pinnedParent / L"value.txt";
  writeFixture(file, "value\n");
  TestAuthority authority{pathText(root)};

  {
    auto opened = take(NativeDetail::openAuthorizedObject(
        authority.path(file, Domain::FileAccess::Read),
        Domain::FileAccess::Read,
        InfrastructureDetail::MissingPathPolicy::Reject, activeContext()));
    require(static_cast<bool>(opened.handle),
            "the anchored native object did not retain its leaf handle");
    auto writeCapableResult = NativeDetail::openCanonicalDirectory(
        pinnedParent.native(),
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES |
            FILE_ADD_FILE | FILE_DELETE_CHILD,
        FILE_SHARE_READ | FILE_SHARE_WRITE, activeContext());
    require(writeCapableResult.hasValue(),
            "retained authorized anchors blocked a concurrent write-capable open");
    auto writeCapable = take(std::move(writeCapableResult));
    require(static_cast<bool>(writeCapable.handle),
            "the concurrent write-capable directory handle was not retained");

    std::binary_semaphore beginRename{0};
    std::binary_semaphore renameFinished{0};
    bool renamedWhileOpen{};
    DWORD renameError{};
    std::thread renamer{[&]() {
      beginRename.acquire();
      renamedWhileOpen =
          ::MoveFileExW(pinnedParent.c_str(), renamedParent.c_str(),
                        MOVEFILE_WRITE_THROUGH) != FALSE;
      renameError = renamedWhileOpen ? ERROR_SUCCESS : ::GetLastError();
      renameFinished.release();
    }};
    beginRename.release();
    renameFinished.acquire();
    renamer.join();
    require(!renamedWhileOpen &&
                (renameError == ERROR_SHARING_VIOLATION ||
                 renameError == ERROR_ACCESS_DENIED),
            "an active authorized open allowed an ancestor rename race");
  }

  require(::MoveFileExW(pinnedParent.c_str(), renamedParent.c_str(),
                        MOVEFILE_WRITE_THROUGH) != FALSE,
          "the authorized open did not release its ancestor anchors");
}

} // namespace
} // namespace ForgeConductor::Tests

int main() {
  using namespace ForgeConductor::Tests;
  TestRegistry tests;
  addTest(tests, "native-tools.filesystem-roundtrip-mutations",
          filesystemRoundTripAndMutations);
  addTest(tests, "native-tools.glob-search-bounds-order",
          globAndTextSearchAreBoundedAndDeterministic);
  addTest(tests, "native-tools.authority-reparse-root",
          authorityReparseAndRootProtections);
  addTest(tests, "native-tools.bounds-utf8-context",
          boundsUtf8CancellationAndDeadline);
  addTest(tests, "native-tools.authorized-open-pins-ancestors",
          authorizedOpenPinsAncestorsAgainstRename);

  std::size_t passed{};
  for (const auto &[name, run] : tests) {
    try {
      run();
      ++passed;
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception &error) {
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
      return 1;
    } catch (...) {
      std::cerr << "[FAIL] " << name << ": unknown exception\n";
      return 1;
    }
  }
  std::cout << passed << '/' << tests.size()
            << " native filesystem/search tests passed.\n";
  return 0;
}
