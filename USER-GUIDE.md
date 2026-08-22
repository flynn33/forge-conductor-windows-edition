# Forge Conductor for Windows — user guide

Version **0.8.0**.

Forge Conductor is a **local MCP tool server** plus a **native dashboard**. LM Studio is the host: it loads the model, owns the chat window, and calls Forge tools over stdio.

You never need a command prompt.

## Install

1. Run `ForgeConductor-0.8.0-win-x64.msi`.
2. Open **Forge Conductor** from the Start Menu.
3. First launch creates `%USERPROFILE%\.forge-conductor`.
4. Open the **Diagnostics** page and confirm Doctor is green.
5. Open **LM Studio MCP** and click **Deploy to LM Studio**.
6. In LM Studio, enable **forge-conductor**, **forge-conductor-fallback**, and **comfy-control** on a new chat.
7. Ask the model to prepare a video. It will set up ComfyUI and tell you the next steps. It will **not** render on this machine while the chat model holds the GPU.

## Surfaces

| Page | What it is |
|---|---|
| FORGE RIG | Live CPU / RAM / GPU / disk instrument panel |
| LM Studio MCP | Deploy, plugin status, ComfyUI prepare-only next steps |
| Agents | Specialist playbooks |
| Tools | The 34 MCP tools |
| Live Feed | Recent tool audit |
| Diagnostics | Doctor checks |
| Manager | Start / stop / Start with Windows |

## Uninstall

Windows Settings → Apps → Forge Conductor → Uninstall.  
Your notes and sessions in `%USERPROFILE%\.forge-conductor` are left in place.

## Continuity

Forge checkpoints work automatically. After a handoff, start a **new** LM Studio chat and have the model call `context_get`.
