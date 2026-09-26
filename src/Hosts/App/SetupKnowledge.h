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
    HelpArticle{L"Manager ownership", L"The Manager owns project access, runs, tools, memory, telemetry, and runtime continuity. Closing this window detaches the client without cancelling Manager-owned work."},
    HelpArticle{L"Provider connection failed", L"Check that LM Studio is serving the configured loopback host and port, save the provider profile, and test again. A successful connection does not by itself prove task completion or tool execution."},
    HelpArticle{L"Choose a model", L"Use a downloaded tool-capable model with enough loaded context for the configured reserves. A pinned model is respected; select another profile or model if it is unavailable."},
    HelpArticle{L"Add instruction packages", L"Choose one or more folders in Workspace. Each becomes a project-scoped queue row. Move rows up or down to define the order supplied to future work."},
    HelpArticle{L"Universal package intake", L"Package admission has no extension, encoding, NUL, file-count, file-size, aggregate-size, depth, or filename filter. Forge Conductor streams hashes, records every entry, and derives bounded text only when safe."},
    HelpArticle{L"Opaque and unavailable entries", L"Binary, non-UTF-8, or large content stays represented by metadata and SHA-256 instead of being omitted. Reparse points and read failures are explicit inventory records and are never silently followed or discarded."},
    HelpArticle{L"Package paging and retry", L"Open a queued package to page through its deterministic inventory or bounded content. Cursors are bound to the package revision. Correct a failed source and choose Retry; remove an inactive row when it is no longer needed."},
    HelpArticle{L"Package order during work", L"New managed work receives package identities in queue order plus available bounded text. A run retains the exact revisions it started with even if the queue changes later."},
    HelpArticle{L"Bind CLU governance", L"Enter a local folder or supported remote development-policy source in Workspace, bind it, and inspect its immutable revision, entries, interpretations, and coverage gaps. Refresh deliberately when the source changes."},
    HelpArticle{L"What CLU does", L"CLU evaluates development evidence, records deduplicated findings, requests corrections, delivers notifications, accepts correction evidence, and exports a redacted history. It is a governance role, not a continuity controller."},
    HelpArticle{L"Governance is nonblocking", L"No bound policy produces an inactive governance state. Findings request correction and remain visible, but CLU does not take over execution or block unrelated work."},
    HelpArticle{L"Read policy documents", L"Use the bounded document reader in Workspace or project_policy.read through MCP. Opaque entries and access gaps remain visible through coverage state rather than being represented as parsed text."},
    HelpArticle{L"Automatic continuity", L"The Workspace toggle is saved for the selected project and provider profile. Enabled Manager-owned runs may roll over at the context boundary. Disabled runs skip automatic continuity observations and otherwise execute normally."},
    HelpArticle{L"Desktop chats and managed runs", L"LM Studio desktop chats and Forge-managed runs are separate modes. MCP connection does not retroactively enroll a desktop chat in Manager-owned automatic continuity."},
    HelpArticle{L"Tools and permissions", L"Native tools remain scoped to the authorized project and Manager policy. Read-only tools inspect; write, Git, and shell tools may change project state. A denied request does not broaden authority when repeated."},
    HelpArticle{L"Read Rig telemetry", L"Rig shows resource, provider, storage, process, context, continuity, and workflow observations. Missing, stale, or disconnected data is not a zero measurement."},
    HelpArticle{L"Use Activity evidence", L"Activity correlates operational outcomes with project/run identity and governance findings. A model response, process completion, native check, and policy evaluation are distinct evidence types."},
    HelpArticle{L"Settings and recovery", L"Settings persists runtime preferences and exposes explicitly scoped maintenance. Check the selected project and confirmation text before reset operations; a reset is not a routine provider-recovery step."},
    HelpArticle{L"Report a problem", L"Record version 1.3.0, project identity, provider profile, package revision or policy revision when relevant, action, expected result, actual result, error text, and time. Remove credentials and private content before sharing diagnostics."},
};

} // namespace ForgeConductor::Hosts::App
