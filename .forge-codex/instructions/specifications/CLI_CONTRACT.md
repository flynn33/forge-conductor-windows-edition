# CLI contract

Required commands:

```text
forge-conductor install
forge-conductor install-lmstudio-plugin [--binary PATH]
forge-conductor doctor
forge-conductor status
forge-conductor agents
forge-conductor serve
forge-conductor manager run [--home PATH] [--open]
forge-conductor manager start|stop|restart|status
forge-conductor version
```

Windows additions:

```text
forge-conductor projects list|show|reset-memory|reset-continuity|reset-all
forge-conductor continuity status|recover|request-rollover
forge-conductor import-macos --source PATH [--dry-run]
forge-conductor installer doctor
```

Use stable exit codes and JSON output through `--json`. Commands are noninteractive when required values are supplied. Destructive commands require explicit confirmation flags/tokens, not prompts in automation.
