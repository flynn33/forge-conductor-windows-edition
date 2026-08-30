// Shipped JavaScript contract: forge-dashboard-control-v2; keep this source pair synchronized.
import { DashboardClient, requireDashboardClient } from "./auth.js";
import type { JsonObject } from "./auth.js";

const MAXIMUM_SESSION_ROWS = 128;
const MAXIMUM_AGENT_ROWS = 64;
const MAXIMUM_AUDIT_ROWS = 128;
const MAXIMUM_DIAGNOSTIC_LINES = 256;
const MANAGER_ACTION_PATHS = {
  start: "/api/manager/start",
  stop: "/api/manager/stop",
  restart: "/api/manager/restart",
} as const;
let latestServiceActive: boolean | null = null;

function element<T extends HTMLElement>(id: string): T {
  const value = document.getElementById(id);
  if (value === null) {
    throw new Error(`Required dashboard element is missing: ${id}`);
  }
  return value as T;
}

function object(value: unknown): JsonObject | null {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? value as JsonObject
    : null;
}

function array(value: unknown): unknown[] {
  return Array.isArray(value) ? value : [];
}

function text(value: unknown): string | null {
  return typeof value === "string" && value.length > 0 ? value : null;
}

function number(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

function boolean(value: unknown): boolean | null {
  return typeof value === "boolean" ? value : null;
}

function displayText(value: unknown): string {
  if (typeof value === "string" && value.length > 0) {
    return value;
  }
  if (typeof value === "number" && Number.isFinite(value)) {
    return String(value);
  }
  if (typeof value === "boolean") {
    return value ? "Yes" : "No";
  }
  return "Unavailable";
}

function timestamp(value: unknown): string {
  const encoded = text(value);
  if (encoded === null) {
    return "Unavailable";
  }
  const date = new Date(encoded);
  return Number.isNaN(date.valueOf()) ? encoded : date.toLocaleString();
}

function setText(id: string, value: string): void {
  element(id).textContent = value;
}

function tableMessage(bodyId: string, columnCount: number, message: string): void {
  const row = document.createElement("tr");
  const cell = document.createElement("td");
  cell.colSpan = columnCount;
  cell.textContent = message;
  row.append(cell);
  element<HTMLTableSectionElement>(bodyId).replaceChildren(row);
}

function appendTextCell(row: HTMLTableRowElement, value: string): void {
  const cell = document.createElement("td");
  cell.textContent = value;
  row.append(cell);
}

function setNotice(message: string, error = false): void {
  const status = element("control-status-message");
  const errorTarget = element("control-error");
  if (error) {
    errorTarget.textContent = message;
    errorTarget.hidden = false;
    return;
  }
  errorTarget.hidden = true;
  status.textContent = message;
  status.hidden = false;
}

function inputNumber(id: string): number {
  return element<HTMLInputElement>(id).valueAsNumber;
}

function inputText(id: string): string {
  return element<HTMLInputElement>(id).value.trim();
}

function setBusy(busy: boolean): void {
  for (const id of ["manager-refresh", "manager-shutdown", "settings-save", "settings-reload"]) {
    element<HTMLButtonElement>(id).disabled = busy;
  }
  element<HTMLButtonElement>("manager-start").disabled = busy || latestServiceActive !== false;
  element<HTMLButtonElement>("manager-stop").disabled = busy || latestServiceActive !== true;
  element<HTMLButtonElement>("manager-restart").disabled = busy || latestServiceActive !== true;
  element<HTMLButtonElement>("sessions-prune").disabled = busy || latestServiceActive !== true;
  element<HTMLButtonElement>("doctor-run").disabled = busy || latestServiceActive !== true;
}

function renderManagerStatus(status: JsonObject): void {
  const dashboard = object(status.dashboard);
  const state = text(status.state) ?? "Unavailable";
  setText("manager-header-status", `Manager ${state}`);
  setText("manager-state", state);
  setText("manager-service-state", boolean(status.service_active) === null ? "Unavailable" : (status.service_active ? "Running" : "Stopped"));
  setText("manager-http-state", boolean(status.http_listening) === null ? "Unavailable" : (status.http_listening ? "Listening" : "Stopped"));
  setText("manager-pid", displayText(status.pid));
  setText("manager-uptime", number(status.uptime_sec) === null ? "Unavailable" : `${displayText(status.uptime_sec)} seconds`);
  setText("manager-restart-count", displayText(status.restart_count));
  setText("manager-version", displayText(status.version));
  setText("manager-home", displayText(status.home));
  setText("manager-dashboard-url", dashboard === null ? "Unavailable" : displayText(dashboard.url));
  setText("manager-last-error", text(status.last_error) ?? "None reported");

  latestServiceActive = boolean(status.service_active);
}

function renderApplicationStatus(status: JsonObject): void {
  const diagnostics = object(status.runtime_diagnostics);
  setText("application-service-state", boolean(status.service_active) === null
    ? "Unavailable"
    : (status.service_active ? "Running" : "Stopped"));
  setText("application-open-sessions", displayText(status.open_session_count));
  setText("application-agent-count", displayText(status.agent_count));
  setText("application-presence-count", displayText(status.presence_count));
  setText("application-runtime", displayText(status.runtime));
  setText("application-runtime-pressure", diagnostics === null ? "Unavailable" : displayText(diagnostics.pressure));
}

function renderSettings(settings: JsonObject): void {
  const dashboard = object(settings.dashboard);
  const manager = object(settings.manager);
  const sessions = object(settings.sessions);
  const shell = object(settings.shell);
  if (dashboard === null || manager === null || sessions === null || shell === null) {
    throw new Error("The manager returned an incomplete settings document.");
  }
  element<HTMLInputElement>("settings-dashboard-host").value = text(dashboard.host) ?? "127.0.0.1";
  element<HTMLInputElement>("settings-dashboard-port").value = displayText(dashboard.port);
  element<HTMLInputElement>("settings-refresh-interval").value = displayText(dashboard.refresh_interval_sec);
  element<HTMLInputElement>("settings-watchdog-interval").value = displayText(manager.watchdog_interval_sec);
  element<HTMLInputElement>("settings-session-idle-ttl").value = displayText(sessions.idle_ttl_sec);
  element<HTMLInputElement>("settings-shell-timeout").value = displayText(shell.default_timeout_sec);
  element<HTMLInputElement>("settings-auto-restart").checked = manager.auto_restart === true;
  element<HTMLInputElement>("settings-open-browser").checked = manager.open_browser_on_start === true;
  element<HTMLSelectElement>("settings-log-level").value = text(settings.log_level) ?? "info";

  const refreshSeconds = number(dashboard.refresh_interval_sec);
  if (refreshSeconds !== null) {
    configureAutoRefresh(refreshSeconds);
  }
}

function renderSessions(payload: JsonObject): void {
  const open = array(payload.open).map(object).filter((entry): entry is JsonObject => entry !== null);
  const recent = array(payload.recent).map(object).filter((entry): entry is JsonObject => entry !== null);
  setText("sessions-summary", `${open.length} open · ${recent.length} recent`);

  const openBody = element<HTMLTableSectionElement>("open-session-rows");
  openBody.replaceChildren();
  if (open.length === 0) {
    tableMessage("open-session-rows", 5, "No open sessions.");
  } else {
    open.slice(0, MAXIMUM_SESSION_ROWS).forEach((session) => {
      const row = document.createElement("tr");
      appendTextCell(row, displayText(session.agent_id));
      appendTextCell(row, displayText(session.status));
      appendTextCell(row, timestamp(session.updated_at));
      appendTextCell(row, displayText(session.summary));
      const action = document.createElement("td");
      const button = document.createElement("button");
      const sessionId = text(session.id);
      button.type = "button";
      button.textContent = "Close";
      button.disabled = sessionId === null;
      button.setAttribute("aria-label", `Close session ${sessionId ?? "with unavailable identifier"}`);
      if (sessionId !== null) {
        button.addEventListener("click", () => void closeSession(sessionId));
      }
      action.append(button);
      row.append(action);
      openBody.append(row);
    });
  }

  const recentBody = element<HTMLTableSectionElement>("recent-session-rows");
  recentBody.replaceChildren();
  if (recent.length === 0) {
    tableMessage("recent-session-rows", 4, "No recent sessions.");
  } else {
    recent.slice(0, MAXIMUM_SESSION_ROWS).forEach((session) => {
      const row = document.createElement("tr");
      appendTextCell(row, displayText(session.agent_id));
      appendTextCell(row, displayText(session.status));
      appendTextCell(row, timestamp(session.updated_at));
      appendTextCell(row, displayText(session.summary));
      recentBody.append(row);
    });
  }
}

function renderAgents(payload: JsonObject): void {
  const agents = array(payload.agents).map(object).filter((entry): entry is JsonObject => entry !== null);
  if (agents.length === 0) {
    tableMessage("agent-rows", 4, "No agents reported.");
    return;
  }
  const body = element<HTMLTableSectionElement>("agent-rows");
  body.replaceChildren();
  agents.slice(0, MAXIMUM_AGENT_ROWS).forEach((agent) => {
    const row = document.createElement("tr");
    appendTextCell(row, displayText(agent.id));
    appendTextCell(row, displayText(agent.display_name));
    appendTextCell(row, displayText(agent.description));
    appendTextCell(row, displayText(agent.source));
    body.append(row);
  });
}

function renderAudit(payload: JsonObject): void {
  const events = array(payload.events).map(object).filter((entry): entry is JsonObject => entry !== null);
  if (events.length === 0) {
    tableMessage("audit-rows", 5, "No audit events reported.");
    return;
  }
  const body = element<HTMLTableSectionElement>("audit-rows");
  body.replaceChildren();
  events.slice(0, MAXIMUM_AUDIT_ROWS).forEach((event) => {
    const row = document.createElement("tr");
    appendTextCell(row, timestamp(event.timestamp));
    appendTextCell(row, displayText(event.tool));
    appendTextCell(row, displayText(event.status));
    appendTextCell(row, number(event.duration_ms) === null ? "Unavailable" : `${displayText(event.duration_ms)} ms`);
    appendTextCell(row, text(event.error) ?? "None reported");
    body.append(row);
  });
}

function renderDiagnostics(payload: JsonObject): void {
  const lines = array(payload.lines).filter((line): line is string => typeof line === "string");
  element("diagnostic-lines").textContent = lines.length === 0
    ? "No diagnostics reported."
    : lines.slice(0, MAXIMUM_DIAGNOSTIC_LINES).join("\n");
}

function renderDoctor(payload: JsonObject): void {
  const checks = array(payload.checks).map(object).filter((entry): entry is JsonObject => entry !== null);
  setText("doctor-summary", `${payload.ok === true ? "All checks passed" : "Issues detected"} · ${displayText(payload.home)} · version ${displayText(payload.version)}`);
  if (checks.length === 0) {
    tableMessage("doctor-rows", 3, "No checks reported.");
    return;
  }
  const body = element<HTMLTableSectionElement>("doctor-rows");
  body.replaceChildren();
  checks.slice(0, 128).forEach((check) => {
    const row = document.createElement("tr");
    appendTextCell(row, displayText(check.name));
    appendTextCell(row, check.ok === true ? "Passed" : "Failed");
    appendTextCell(row, displayText(check.detail));
    body.append(row);
  });
}

function renderServiceStopped(): void {
  setText("sessions-summary", "Service stopped");
  tableMessage("open-session-rows", 5, "Service stopped.");
  tableMessage("recent-session-rows", 4, "Service stopped.");
  tableMessage("agent-rows", 4, "Service stopped.");
  tableMessage("audit-rows", 5, "Service stopped.");
  setText("diagnostic-lines", "Service stopped.");
}

const authError = element("control-auth-error");
const client: DashboardClient | null = requireDashboardClient(authError);
let refreshInFlight: Promise<void> | null = null;
let settingsInFlight: Promise<void> | null = null;
let mutationInFlight = false;
let autoRefreshTimer: number | null = null;

function configureAutoRefresh(seconds: number): void {
  if (autoRefreshTimer !== null) {
    window.clearInterval(autoRefreshTimer);
  }
  const boundedSeconds = Math.min(300, Math.max(2, seconds));
  autoRefreshTimer = window.setInterval(() => {
    if (!document.hidden) {
      void refreshAll();
    }
  }, boundedSeconds * 1000);
}

async function loadSettings(): Promise<void> {
  if (client === null || refreshInFlight !== null || mutationInFlight) {
    return settingsInFlight ?? Promise.resolve();
  }
  if (settingsInFlight !== null) {
    return settingsInFlight;
  }
  setBusy(true);
  const operation = (async () => {
    try {
      renderSettings(await client.json("/api/manager/settings"));
    } finally {
      settingsInFlight = null;
      setBusy(false);
    }
  })();
  settingsInFlight = operation;
  return operation;
}

async function refreshAll(): Promise<void> {
  if (client === null || refreshInFlight !== null || settingsInFlight !== null || mutationInFlight) {
    return refreshInFlight ?? Promise.resolve();
  }
  setBusy(true);
  const operation = (async () => {
    try {
      const [managerStatus, applicationStatus] = await Promise.all([
        client.json("/api/manager/status"),
        client.json("/api/status"),
      ]);
      renderManagerStatus(managerStatus);
      renderApplicationStatus(applicationStatus);
      if (managerStatus.service_active !== true) {
        renderServiceStopped();
      } else {
        const [sessions, agents, audit, diagnostics] = await Promise.all([
          client.json("/api/sessions"),
          client.json("/api/agents"),
          client.json("/api/audit"),
          client.json("/api/diagnostics"),
        ]);
        renderSessions(sessions);
        renderAgents(agents);
        renderAudit(audit);
        renderDiagnostics(diagnostics);
      }
      element("control-loading").hidden = true;
      element("control-error").hidden = true;
    } catch (reason: unknown) {
      setNotice(reason instanceof Error ? reason.message : "Manager refresh failed.", true);
    } finally {
      setBusy(false);
      refreshInFlight = null;
    }
  })();
  refreshInFlight = operation;
  return operation;
}

async function managerAction(action: "start" | "stop" | "restart"): Promise<void> {
  if (client === null || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  mutationInFlight = true;
  setBusy(true);
  try {
    const status = await client.post(MANAGER_ACTION_PATHS[action], {});
    renderManagerStatus(status);
    setNotice(`Manager ${action} request completed.`);
  } catch (reason: unknown) {
    setNotice(reason instanceof Error ? reason.message : `Manager ${action} failed.`, true);
  } finally {
    mutationInFlight = false;
    setBusy(false);
  }
  await refreshAll();
}

async function closeSession(sessionId: string): Promise<void> {
  if (client === null || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  mutationInFlight = true;
  setBusy(true);
  try {
    await client.post("/api/sessions/close", {
      session_id: sessionId,
      summary: "Closed from dashboard",
    });
    setNotice("Session closed.");
  } catch (reason: unknown) {
    setNotice(reason instanceof Error ? reason.message : "Session close failed.", true);
  } finally {
    mutationInFlight = false;
    setBusy(false);
  }
  await refreshAll();
}

async function pruneSessions(): Promise<void> {
  if (client === null || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  mutationInFlight = true;
  setBusy(true);
  try {
    await client.post("/api/sessions/prune", {});
    setNotice("Stale sessions pruned.");
  } catch (reason: unknown) {
    setNotice(reason instanceof Error ? reason.message : "Session pruning failed.", true);
  } finally {
    mutationInFlight = false;
    setBusy(false);
  }
  await refreshAll();
}

async function saveSettings(event: SubmitEvent): Promise<void> {
  event.preventDefault();
  if (client === null || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  const form = element<HTMLFormElement>("manager-settings-form");
  if (!form.reportValidity()) {
    return;
  }
  mutationInFlight = true;
  setBusy(true);
  try {
    const result = await client.post("/api/manager/settings", {
      apply: true,
      settings: {
        dashboard: {
          host: inputText("settings-dashboard-host"),
          port: inputNumber("settings-dashboard-port"),
          refresh_interval_sec: inputNumber("settings-refresh-interval"),
        },
        manager: {
          auto_restart: element<HTMLInputElement>("settings-auto-restart").checked,
          watchdog_interval_sec: inputNumber("settings-watchdog-interval"),
          open_browser_on_start: element<HTMLInputElement>("settings-open-browser").checked,
        },
        sessions: { idle_ttl_sec: inputNumber("settings-session-idle-ttl") },
        shell: { default_timeout_sec: inputNumber("settings-shell-timeout") },
        log_level: element<HTMLSelectElement>("settings-log-level").value,
      },
    });
    renderSettings(result);
    const reboundStatus = object(result.status);
    if (result.applied === true && result.bind_changed === true && reboundStatus !== null) {
      const dashboard = object(reboundStatus.dashboard);
      const canonicalUrl = dashboard === null ? null : text(dashboard.url);
      if (canonicalUrl === null) {
        throw new Error("The manager did not return the rebound dashboard URL.");
      }
      setNotice("Settings saved. Reconnecting to the new dashboard listener.");
      client.navigateToReboundDashboard(canonicalUrl);
      return;
    }
    setNotice(result.applied === true ? "Settings saved and applied." : "Settings validated without applying changes.");
  } catch (reason: unknown) {
    setNotice(reason instanceof Error ? reason.message : "Settings update failed.", true);
  } finally {
    mutationInFlight = false;
    setBusy(false);
  }
  await refreshAll();
}

async function runDoctor(): Promise<void> {
  if (client === null || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  mutationInFlight = true;
  setBusy(true);
  try {
    renderDoctor(await client.json("/api/doctor"));
    setNotice("Doctor completed.");
  } catch (reason: unknown) {
    setNotice(reason instanceof Error ? reason.message : "Doctor failed.", true);
  } finally {
    mutationInFlight = false;
    setBusy(false);
  }
}

async function shutdownManager(): Promise<void> {
  if (client === null || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  mutationInFlight = true;
  setBusy(true);
  try {
    await client.post("/api/manager/shutdown", {});
    setNotice("Manager is shutting down.");
    setText("manager-header-status", "Manager stopping");
  } catch (reason: unknown) {
    setNotice(reason instanceof Error ? reason.message : "Manager shutdown failed.", true);
  } finally {
    mutationInFlight = false;
    setBusy(false);
  }
}

element("manager-start").addEventListener("click", () => void managerAction("start"));
element("manager-stop").addEventListener("click", () => void managerAction("stop"));
element("manager-restart").addEventListener("click", () => void managerAction("restart"));
element("manager-refresh").addEventListener("click", () => void refreshAll());
element("sessions-prune").addEventListener("click", () => void pruneSessions());
element("settings-reload").addEventListener("click", () => void loadSettings().catch((reason: unknown) => setNotice(reason instanceof Error ? reason.message : "Settings reload failed.", true)));
element("doctor-run").addEventListener("click", () => void runDoctor());
element<HTMLFormElement>("manager-settings-form").addEventListener("submit", (event) => void saveSettings(event));

const shutdownDialog = element<HTMLDialogElement>("manager-shutdown-dialog");
element("manager-shutdown").addEventListener("click", () => shutdownDialog.showModal());
shutdownDialog.addEventListener("close", () => {
  if (shutdownDialog.returnValue === "confirm") {
    void shutdownManager();
  }
  element<HTMLButtonElement>("manager-shutdown").focus();
});

document.addEventListener("visibilitychange", () => {
  if (!document.hidden) {
    void refreshAll();
  }
});

window.addEventListener("pagehide", () => {
  if (autoRefreshTimer !== null) {
    window.clearInterval(autoRefreshTimer);
  }
});

if (client !== null) {
  void loadSettings()
    .then(() => refreshAll())
    .catch((reason: unknown) => {
      setNotice(reason instanceof Error ? reason.message : "Dashboard initialization failed.", true);
      void refreshAll();
    });
} else {
  element("control-loading").hidden = true;
  setText("manager-header-status", "Authentication required");
  setBusy(true);
}
