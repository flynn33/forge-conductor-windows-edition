# Installer, update, repair, and release

## Artifacts

Produce:

- `ForgeConductor_<version>_x64.msixbundle`;
- ARM64 bundle when supported;
- `.appinstaller` descriptor with configurable update URI;
- `ForgeConductor.Setup.exe` native C++ bootstrapper;
- symbol package;
- checksums;
- SBOM/dependency manifest;
- release notes and install guide.

## Setup bootstrapper

Implement in object-oriented native C++ using Win32/WinRT deployment APIs. It supports:

```text
/install
/repair
/update
/uninstall
/purge-data
/silent
/log <path>
```

Responsibilities:

- verify hashes/signatures;
- install required Windows App SDK dependencies when not self-contained;
- install the MSIX bundle;
- register execution alias/start menu entries;
- register or repair manager startup;
- run doctor and MCP smoke;
- rollback on failure;
- preserve data by default;
- remove data only on explicit purge.

## Signing

Noninteractive precedence:

1. release certificate from environment/secure build context;
2. repository-configured enterprise certificate;
3. automatically generated CurrentUser development certificate.

Never prompt. A development certificate is sufficient for automated install testing, but release evidence must state the signing class.

## Lifecycle tests

On clean Windows 11 user profiles, automatically test:

- fresh install;
- first launch;
- manager startup;
- CLI alias;
- MCP serve;
- repair;
- same-version reinstall;
- upgrade from previous test package;
- data preservation;
- uninstall;
- reinstall;
- purge;
- broken/interrupted install recovery.

Collect Windows deployment logs and setup logs.
