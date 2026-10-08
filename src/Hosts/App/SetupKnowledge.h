#pragma once

#include <array>
#include <string_view>

namespace ForgeConductor::Hosts::App {

struct HelpArticle final {
    std::wstring_view title;
    std::wstring_view body;
};

inline constexpr std::array SetupKnowledge{
    HelpArticle{L"Prepare a workspace", L"Open Workspace, register or select the project folder, choose its provider profile, add any instruction-package folders, choose the automatic-continuity preference, and read the readiness summary. Optional governance and disabled continuity do not prevent ordinary work."},
    HelpArticle{L"Create a new project folder", L"Use the Windows folder picker and select a new or empty folder. Registration assigns a durable project identity but does not initialize Git, install dependencies, or generate starter files."},
    HelpArticle{L"Continue an existing project", L"Select the existing project in Workspace. Its authorized root, provider binding, package queue, governance binding, and automatic-continuity preference are restored from durable state."},
    HelpArticle{L"Manager ownership", L"The Manager owns project access, tools, memory, settings, policy, deployment, and telemetry. Sessions are LM Studio chats. The primary MCP worker owns native Auto Continuity; closing this window does not cancel chat work."},
    HelpArticle{L"Provider connection failed", L"Check that LM Studio is serving the configured loopback host and port, save the provider profile, and test again. A successful connection does not by itself prove task completion or tool execution."},
    HelpArticle{L"Choose a model", L"Use a downloaded tool-capable model with enough loaded context for the configured reserves. A pinned model is respected; select another profile or model if it is unavailable."},
    HelpArticle{L"Add instruction packages", L"Choose one or more folders in Workspace. Each becomes a project-scoped queue row. Move rows up or down, drag to reorder, or remove any row, including an active package. Changes are saved immediately for future package reads."},
    HelpArticle{L"Universal package intake", L"Package admission has no extension, encoding, NUL, file-count, file-size, aggregate-size, depth, or filename filter. Forge Conductor streams hashes, records every entry, and derives bounded text only when safe."},
    HelpArticle{L"Opaque and unavailable entries", L"Binary, non-UTF-8, or large content stays represented by metadata and SHA-256 instead of being omitted. Reparse points and read failures are explicit inventory records and are never silently followed or discarded."},
    HelpArticle{L"Package paging and retry", L"Open a queued package to page through its deterministic inventory or bounded content. Cursors are bound to the package revision. Correct a failed source and choose Retry; remove any row when it is no longer needed. Removal deletes the queue binding and keeps the source folder available to add again."},
    HelpArticle{L"Package order during work", L"get_forge_status reports the bound project and package paths in execution order. Call instruction_package.read with each selected queue_row_id to read its paged entries and content before project work."},
    HelpArticle{L"Bind CLU governance", L"Choose policy folder or Choose policy file in Workspace to select a development-policy source. With a project selected, selection binds that source immediately. Browsing also works before project selection; select a project and choose its policy again to activate governance. Reload policies from source deliberately when its content changes; inspect status/findings or read a selected policy document."},
    HelpArticle{L"What CLU does", L"CLU evaluates development evidence, records findings, requests corrections, and accepts correction evidence. Tool-result notifications deliver findings to the loaded model; Inspect findings and Activity show them to the user. CLU does not create chats."},
    HelpArticle{L"Governance is nonblocking", L"No bound policy produces an inactive governance state. Findings request correction and remain visible, but CLU does not take over execution or block unrelated work."},
    HelpArticle{L"Read policy documents", L"Use the bounded document reader in Workspace or project_policy.read through MCP. Opaque entries and access gaps remain visible through coverage state rather than being represented as parsed text."},
    HelpArticle{L"Auto Continuity", L"The dashboard/Workspace toggle is saved for the selected project/provider. At native chat reserve pressure, the primary MCP worker pauses at a completed tool boundary, asks the loaded model for a detailed packet, opens the next LM Studio chat, hands it over, and verifies packet retrieval plus another Forge call."},
    HelpArticle{L"Continuity release bounds", L"Version 1.3.5 verified reserve-triggered pause, not physical context exhaustion. Native rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed. No replacement run manager or fourth plugin is added."},
    HelpArticle{L"Tools and permissions", L"Native tools remain scoped to the authorized project and Manager policy. Read-only tools inspect; write, Git, and shell tools may change project state. A denied request does not broaden authority when repeated."},
    HelpArticle{L"Read Rig telemetry", L"Rig shows resource, provider, storage, process, context, continuity, and workflow observations. Missing, stale, or disconnected data is not a zero measurement."},
    HelpArticle{L"Use Activity evidence", L"Activity correlates operational outcomes with project/run identity and governance findings. A model response, process completion, native check, and policy evaluation are distinct evidence types."},
    HelpArticle{L"Settings and saved records", L"Settings retains Load effective settings, Save and read back, Revert pending edits, Test LM Studio, and Restart Manager. Select saved project records and use Delete record or Delete selected. Continuity separately lists packets with delete-selection and clear buttons."},
    HelpArticle{L"Report a problem", L"Record version 1.3.20, project identity, provider profile, package revision or policy revision when relevant, action, expected result, actual result, error text, and time. Remove credentials and private content before sharing diagnostics."},
};

} // namespace ForgeConductor::Hosts::App
