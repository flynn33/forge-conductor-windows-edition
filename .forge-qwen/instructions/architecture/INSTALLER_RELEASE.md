# Installer and release

Produce self-contained x64 and supported ARM64 MSIX artifacts containing GUI, console/MCP, manager, session host, modules/manifests/resources, Windows App SDK files, migrations, playbooks, and static dashboard. Include Start menu app, app execution alias `forge-conductor.exe`, and per-user StartupTask where used.

Native OOP C++ setup bootstrapper supports install, repair, update, uninstall, purge-data, silent, and log. Verify signatures/hashes/publisher, install dependencies/package, register/repair startup, run doctor/MCP smoke, rollback, preserve data by default.

Use supplied production certificate when available and valid. Otherwise generate CurrentUser local test signing, keep private key out of source/deliverable, install public certificate for local tests, and classify honestly.

Clean-profile tests: install, launch, CLI alias/stdin/stdout, MCP, manager startup, GUI smoke, same-version repair, upgrade/data/foreign LM entries, failure rollback, uninstall preserve policy, purge, reinstall, signature/corruption/architecture behavior.
