# P06-007: Narrow Native Relative-File Wrapper

Status: Accepted

Date: 2026-08-25

## Context

Forge Conductor supports Windows 11 from build 22000. The Win32 APIs that can
create directories or files while disallowing path redirects—`CreateDirectory2W`
with `DIRECTORY_FLAGS_DISALLOW_PATH_REDIRECTS`, and `CreateFile3` with
`FILE_FLAG_DISALLOW_PATH_REDIRECTS`—require Windows 11 version 24H2. Raising the
minimum operating-system version would violate the accepted product baseline.

Earlier Win32 path-based create/open calls cannot bind a one-component child to
an already verified parent handle. Rechecking a full path after creation is too
late: an attacker may already have redirected a staging write outside the
authority root.

The Windows SDK declares `NtCreateFile` and `RtlNtStatusToDosError` in
`winternl.h`. `NtCreateFile` accepts an `OBJECT_ATTRIBUTES::RootDirectory`
handle and a relative object name, which closes the namespace-resolution race.
P06-006's runtime compatibility matrix also requires the ntdll-exported
`NtSetInformationFile` for the native same-directory
`FileRenameInformationEx` form. These are lower-level Windows native APIs whose
contracts must not leak through product interfaces.

## Decision

- Use `NtCreateFile` only inside the private
  `Infrastructure::Windows::Detail::openRelative` wrapper.
- Link `ntdll` privately. Do not expose `winternl.h`, `NTSTATUS`,
  `OBJECT_ATTRIBUTES`, or native create options through public product headers.
- Resolve `NtSetInformationFile` and `RtlNtStatusToDosError` privately inside
  the atomic implementation. Native class 65 and the matching aligned
  `FILE_RENAME_INFO` layout are fixed implementation details. Missing exports,
  native failure, or an unsupported POSIX-rename volume fails closed; there is
  no path-based fallback.
- The wrapper accepts exactly one validated path component. It rejects empty,
  dot, dot-dot, separator, alternate-stream, control-character, trailing-dot or
  trailing-space, wildcard, invalid Win32-character, overlength, and reserved
  DOS-device names before entering the native API.
- A typed option model maps only file/directory, open/create/open-or-create,
  write-through, sequential access, delete-on-close, desired access, file
  attributes, and the three documented sharing bits. Raw native create options
  are not caller-controlled.
- Every open forces `FILE_OPEN_REPARSE_POINT` and synchronous handle semantics.
  Callers inspect the returned object type/reparse tag and retain the parent
  handle with `FILE_TRAVERSE`. `NtCreateFile` performs the child-creation access
  check against the caller token and directory security; callers never release
  or reopen an anchor to add mutation rights.
- Native status is converted with `RtlNtStatusToDosError` at the two private
  native boundaries and returned as a typed Win32 error. Relative opens return
  typed RAII handle ownership. Exceptions never escape.
- Diagnostics and atomic storage use this wrapper for every security-sensitive
  child open/create beneath a retained authority handle. Full-path creation is
  prohibited while a strong anchor is the security boundary.
- Direct tests cover validation, disposition results, collisions, missing
  children, final reparse handles, native-error mapping, strong-parent relative
  creation, denial of competing full-path mutation, missing native rename
  exports, and same-directory extended rename behavior under the accepted share
  modes.
- A later baseline that requires Windows 11 24H2 or newer may replace this
  wrapper with `CreateDirectory2W`/`CreateFile3`, but only after equivalent
  adversarial tests pass.

## Consequences

The product preserves its Windows 11 build-22000 minimum while binding child
namespace operations to verified parent objects. Native API risk is concentrated
in private Windows-detail translation units and one private link dependency
rather than duplicated across public adapters.

The wrapper requires targeted compatibility testing on the supported Windows 11
baselines. Failure to open relative to a retained handle is a closed operation
failure; callers never fall back to a path-based create.

## Rejected alternatives

- Raise the minimum to Windows 11 24H2: rejected because the initiating mission
  requires a Windows 11 application, not a 24H2-only application.
- Create by full path and validate afterward: rejected because unauthorized
  mutation can occur before validation.
- Duplicate `NtCreateFile` calls in each subsystem: rejected because option,
  validation, error, and ownership policy would drift.
- Accept arbitrary relative paths or raw create options: rejected because the
  wrapper would become a second unbounded filesystem API.

## Evidence

- `.forge-codex/instructions/architecture/SECURITY.md`
- `Directory.Build.props`
- Microsoft `CreateDirectory2W` and `DIRECTORY_FLAGS_DISALLOW_PATH_REDIRECTS`
  contracts
- Microsoft `CreateFile3` and `CREATEFILE3_EXTENDED_PARAMETERS` contracts
- Microsoft `NtCreateFile` contract
- Windows SDK 10.0.26100.0 `winternl.h`
- `.forge-codex/state/probes/relative-create-root-access.cpp`
- `.forge-codex/state/probes/relative-posix-publish-matrix.cpp`
