# Native desktop engineering checkpoint — September 12, 2026

All commands ran in `D:\GitHub\Forge-Conductor-Windows-Edition` on branch `alpha/native-desktop`,
base `14660648599378c85c3cdade5bd44ffe1cded079` plus working changes.

Native C++/WinRT WinUI app: `src/Hosts/App/ForgeConductor.App.vcxproj`.
VS 2022 Community MSBuild 17.14.40, v143, Windows SDK 26100, Windows App SDK 2.4.0,
C++/WinRT 3.0.260818.1. Existing Build Tools installation lacked UWP/XAML targets; the build helper discovers
the complete VS 2022 installation. Native NuGet imports are used without the legacy managed UAP resolver.
C++/WinRT ABI-generated derived classes require non-final UI implementation classes.

## Results
- Debug native app MSBuild: exit 0, `out/alpha-evidence/p1-app-build.log`.
- Actual native window launched through Windows app automation. Rig, Start manager and Refresh were visible
  and exercised. No browser-based UI or fabricated manager state is used.
- Start dispatched the sibling manager. Refresh displayed the actual named-pipe deadline failure.
- Direct manager launch exited 1: `unsupported_version: The central database schema is newer than this application supports.`
  User database was not changed to force acceptance. Successful default-profile manager attach remains unverified.
- Non-mutating copied-store inspection recorded schema 9 with C001-C009. This checkout and every fetched public
  ref only contain C001-C007, so C008/C009 compatibility cannot be reconstructed from available migration source.
- Added explicit `--alpha-root` composition. Its canonical root deterministically scopes the Manager lease/pipe
  and DPAPI registry key. The WinUI app propagates that root to its sibling Manager and attaches to the same scope.
- Real isolated-profile WinUI run passed: Start/Refresh displayed Manager PID 26324, version 0.9.0, active service,
  listening dashboard, and the exact disposable root. Closing the GUI left Manager PID 26324 alive.
  `out/alpha-evidence/p1-isolated-profile.txt` records the run. Production DB/WAL/SHM hashes remained unchanged.
- Integrated `scripts/build.ps1 -Configuration Release -Architecture x64 -Product All -Parallel 4`: exit 0.
  `out/alpha-evidence/p1-release-build.log`, outputs `out/app/x64/Release/`.
- Built and ran `ForgeConductor.Manager.NamedPipeRoundTripTests` with x64 Debug preset: exit 0, 1/1 passed
  in 0.25 seconds. `out/alpha-evidence/p1-pipe-test.log`. This is transport regression evidence, not GUI/provider acceptance.
- `scripts/package.ps1 -Configuration Release -Architecture x64 -DevelopmentSigning`: MakeAppx and SignTool
  succeeded. Distribution `out/dist/engineering-0.9.0.0-20260912-112241/` contains signed MSIX, public certificate,
  install helper, metadata/README and engineering ZIP. No private key is exported. Full log: `out/alpha-evidence/p1-package.log`.
- Installation attempt on the earlier same-identity engineering package failed `0x800B0109` (publisher trust).
  Current-user TrustedPeople alone was insufficient; LocalMachine TrustedPeople import returned `0x80070005`.
  No installed-app acceptance is claimed. The install helper requires Administrator only for publisher trust.

## Release executable SHA-256
- GUI: `dbe4b07dc200c0816a1f194865fc964ec0deae2e4d0c1c36c74db6fb8310b450`
- CLI: `ccdb4752f8f2002b31d0da0619dfb5c0e42954877657c46807fb4f48eff4ab20`
- Manager: `7c4818bb4f9b62ccf1cbf8a9002377b34428e2e7d97c9d1237614c696a3ba2a5`
- Session host: `ef787c384ade438c6f6a4e8954cf9f87303d9e5f53969e65ae1481ef19ff8d10`

P1 runtime attach and detach are complete; installed launch remains blocked on publisher trust. P2–P6, quota removal,
real provider continuity and all required native pages remain outstanding. This package is explicitly an engineering checkpoint.
