# Product status

Updated September 12, 2026 on `alpha/native-desktop`, base `14660648599378c85c3cdade5bd44ffe1cded079` plus working changes.
Last source audit: September 10, 2026 at `3e67a03b6c64d9a105b677dfe41e413bddd7762c`.

| Area | Current evidence | Next required proof |
|---|---|---|
| Native backend | CLI, Manager and SessionHost build x64 Debug and Release; focused Domain, Manager, MCP, continuity, session and 90/90 infrastructure tests pass | Complete live-provider and installed acceptance |
| Native GUI | WinUI/C++/WinRT builds and stages with the required NavigationView destinations; Rig uses live Manager status and Provider performs typed load/save/probe actions | Complete real actions/data for the remaining destinations and interactive walkthrough |
| Installer | Signed x64 Release engineering MSIX/ZIP exists with public certificate and install helper | Machine publisher trust, installed launch/update/uninstall and installed MCP proof |
| LM Studio managed continuity | Production composition uses loopback LM Studio `/v1/models` and `/v1/responses`; fixture proves fresh root, `context_get`, actual response IDs, chaining, exact acknowledgement and usage; Manager owns the continuity automation lifecycle | Running LM Studio model plus typed Manager-owned ordinary run command and context-forced useful successor |
| Context policy | Count/time rollover fields and behavior removed; context-only regression covers 500 ordinary observations across time | Live provider threshold rollover |
| Provider settings | Endpoint/model/capacity/reserves persist through typed Manager protocol; two successive authenticated isolated-profile save/restart cycles passed | Live `/v1/models` discovery (server currently refuses connection) |
| P2 workflow | Disposable project registration, MCP deployment, native tools and cross-project/foreign-entry preservation passed | Reuse in installed Release acceptance |

The existing current-user central database is schema 9 (C001-C009), while available source supports schema 7
(C001-C007). It was preserved and retained identical hashes during isolated runs. Existing-data compatibility remains open until
the real C008/C009 migration history and contract are available.

The provider transport is no longer synthetic, but `127.0.0.1:1234` currently refuses connections. No live model rollover is
claimed. A Manager-owned ordinary Responses run controller also remains to be composed so real usage observations invoke the
already-tested context-only `ContinuityAutomation` path.

Engineering distribution: `out/dist/engineering-0.9.0.0-20260912-140140/`. Package creation/signing succeeded; MSIX SHA-256 is
`8909b08760f5a04d4dfaee087da622060754cc19c253cda3c393ebcb4e19cad2`.
`Add-AppxPackage` failed with `0x800B0109`; importing the development publisher to machine TrustedPeople failed with
`0x80070005` because the session is not elevated. GitHub plan preview is pending `gh` authentication; no remote changes were made.

## Execution ledger

Use `.forge-alpha/status.json` for phase state and `.forge-alpha/NEXT.md` for the next action. Full transient logs live under
ignored `out/alpha-evidence/`.
