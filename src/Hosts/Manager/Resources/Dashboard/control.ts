// Shipped JavaScript contract: forge-dashboard-control-v2; keep this source pair synchronized.
import { DashboardClient, requireDashboardClient } from "./auth.js";
import type { JsonObject } from "./auth.js";

const MAXIMUM_SESSION_ROWS = 128;
const MAXIMUM_AGENT_ROWS = 64;
const MAXIMUM_AUDIT_ROWS = 128;
const MAXIMUM_DIAGNOSTIC_LINES = 256;
const RESTART_RECONNECT_ATTEMPTS = 12;
const RESTART_RECONNECT_DELAY_MS = 500;
const RESTART_PROBE_TIMEOUT_MS = 1_000;
const PAGE_RESTORE_ATTEMPTS = 20;
const PAGE_RESTORE_DELAY_MS = 1_000;
const PAGE_RESTORE_PROBE_TIMEOUT_MS = 1_000;
const MANAGER_ACTION_PATHS = {
  start: "/api/manager/start",
  stop: "/api/manager/stop",
  restart: "/api/manager/restart",
} as const;
let latestServiceActive: boolean | null = null;
let latestRestartCount: number | null = null;
let expectedRestartCount: number | null = null;
let restartReconnectAttempt = 0;
let restartReconnectGeneration = 0;
let restartReconnectTimer: number | null = null;
let restartReconnectActive = false;
let restartProbeController: AbortController | null = null;
let pageLifecycleGeneration = 0;
let pageLifecycleActive = true;
let pageRequestController = new AbortController();
let pageRestoreAttempt = 0;
let pageRestoreGeneration = 0;
let pageRestoreTimer: number | null = null;
let pageRestoreController: AbortController | null = null;

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
  latestRestartCount = number(status.restart_count);
}

function suspendRestartReconnect(): void {
  restartReconnectGeneration += 1;
  if (restartProbeController !== null && restartReconnectAttempt > 0) {
    restartReconnectAttempt -= 1;
  }
  restartProbeController?.abort();
  restartProbeController = null;
  if (restartReconnectTimer !== null) {
    window.clearTimeout(restartReconnectTimer);
    restartReconnectTimer = null;
  }
}

function stopRestartReconnect(): void {
  suspendRestartReconnect();
  restartReconnectActive = false;
  restartReconnectAttempt = 0;
  expectedRestartCount = null;
}

function scheduleRestartReconnect(delayMs = RESTART_RECONNECT_DELAY_MS): void {
  if (client === null || !pageLifecycleActive || !restartReconnectActive ||
      restartReconnectTimer !== null) {
    return;
  }
  restartReconnectTimer = window.setTimeout(() => {
    restartReconnectTimer = null;
    void probeRestartCompletion();
  }, delayMs);
}

async function probeRestartCompletion(): Promise<void> {
  if (client === null || !pageLifecycleActive || !restartReconnectActive ||
      expectedRestartCount === null) {
    return;
  }
  const generation = restartReconnectGeneration;
  const requiredRestartCount = expectedRestartCount;
  const controller = new AbortController();
  restartProbeController = controller;
  restartReconnectAttempt += 1;
  let status: JsonObject | null = null;
  try {
    status = await client.json(
      "/api/manager/status",
      { signal: controller.signal },
      RESTART_PROBE_TIMEOUT_MS,
    );
  } catch {
    // The listener is expected to be unavailable during the bounded cutover.
  } finally {
    if (restartProbeController === controller) {
      restartProbeController = null;
    }
  }

  if (!pageLifecycleActive || !restartReconnectActive ||
      generation !== restartReconnectGeneration ||
      requiredRestartCount !== expectedRestartCount) {
    return;
  }
  const state = status === null ? null : text(status.state);
  const restartCount = status === null ? null : number(status.restart_count);
  if (state === "running" && restartCount !== null && restartCount >= requiredRestartCount) {
    stopRestartReconnect();
    client.navigateToReboundDashboard(window.location.origin);
    return;
  }

  if (restartReconnectAttempt < RESTART_RECONNECT_ATTEMPTS) {
    scheduleRestartReconnect();
    return;
  }
  stopRestartReconnect();
  mutationInFlight = false;
  setBusy(false);
  setNotice("Manager restart is taking longer than expected. Refresh to reconnect.", true);
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

interface MutationOwner {
  readonly controller: AbortController;
  readonly generation: number;
}

let refreshInFlight: Promise<void> | null = null;
let settingsInFlight: Promise<void> | null = null;
let mutationInFlight = false;
let mutationController: AbortController | null = null;
let autoRefreshTimer: number | null = null;

function pageRequestIsCurrent(
  generation: number,
  controller: AbortController,
): boolean {
  return pageLifecycleActive && generation === pageLifecycleGeneration &&
    controller === pageRequestController && !controller.signal.aborted;
}

function beginMutation(): MutationOwner {
  const owner = {
    controller: new AbortController(),
    generation: pageLifecycleGeneration,
  };
  mutationController = owner.controller;
  mutationInFlight = true;
  setBusy(true);
  return owner;
}

function mutationIsCurrent(owner: MutationOwner): boolean {
  return pageLifecycleActive && owner.generation === pageLifecycleGeneration &&
    mutationController === owner.controller && !owner.controller.signal.aborted;
}

function finishMutation(owner: MutationOwner, retainForRestart = false): void {
  if (mutationController !== owner.controller) {
    return;
  }
  mutationController = null;
  mutationInFlight = retainForRestart;
  if (!retainForRestart && pageLifecycleActive) {
    setBusy(false);
  }
}

function stopPageRestore(): void {
  pageRestoreGeneration += 1;
  pageRestoreAttempt = 0;
  pageRestoreController?.abort();
  pageRestoreController = null;
  if (pageRestoreTimer !== null) {
    window.clearTimeout(pageRestoreTimer);
    pageRestoreTimer = null;
  }
}

function schedulePageRestore(delayMs = PAGE_RESTORE_DELAY_MS): void {
  if (client === null || !pageLifecycleActive || restartReconnectActive ||
      pageRestoreTimer !== null || pageRestoreController !== null) {
    return;
  }
  pageRestoreTimer = window.setTimeout(() => {
    pageRestoreTimer = null;
    void restorePersistedPage();
  }, delayMs);
}

async function restorePersistedPage(): Promise<void> {
  if (client === null || !pageLifecycleActive || restartReconnectActive) {
    return;
  }
  const generation = pageRestoreGeneration;
  pageRestoreAttempt += 1;
  if (refreshInFlight !== null || settingsInFlight !== null || mutationInFlight) {
    if (pageRestoreAttempt < PAGE_RESTORE_ATTEMPTS) {
      schedulePageRestore();
    } else {
      stopPageRestore();
      setNotice("Dashboard restoration is taking longer than expected. Refresh to reconnect.", true);
    }
    return;
  }

  const controller = new AbortController();
  pageRestoreController = controller;
  let restored = false;
  try {
    await client.json(
      "/api/manager/status",
      { signal: controller.signal },
      PAGE_RESTORE_PROBE_TIMEOUT_MS,
    );
    if (!pageLifecycleActive || generation !== pageRestoreGeneration ||
        controller.signal.aborted) {
      return;
    }
    await loadSettings();
    if (!pageLifecycleActive || generation !== pageRestoreGeneration ||
        controller.signal.aborted || mutationInFlight) {
      return;
    }
    await refreshAll();
    restored = pageLifecycleActive && generation === pageRestoreGeneration &&
      !controller.signal.aborted && autoRefreshTimer !== null;
  } catch {
    // A BFCache restore can race the listener cutover; retry within the bound.
  } finally {
    if (pageRestoreController === controller) {
      pageRestoreController = null;
    }
  }

  if (!pageLifecycleActive || generation !== pageRestoreGeneration) {
    return;
  }
  if (restored) {
    stopPageRestore();
    return;
  }
  if (pageRestoreAttempt < PAGE_RESTORE_ATTEMPTS) {
    schedulePageRestore();
    return;
  }
  stopPageRestore();
  setBusy(false);
  setNotice("Dashboard restoration is taking longer than expected. Refresh to reconnect.", true);
}

function startPageRestore(): void {
  stopPageRestore();
  schedulePageRestore(0);
}

function configureAutoRefresh(seconds: number): void {
  if (autoRefreshTimer !== null) {
    window.clearInterval(autoRefreshTimer);
  }
  if (!pageLifecycleActive) {
    autoRefreshTimer = null;
    return;
  }
  const boundedSeconds = Math.min(300, Math.max(2, seconds));
  autoRefreshTimer = window.setInterval(() => {
    if (!document.hidden) {
      void refreshAll();
    }
  }, boundedSeconds * 1000);
}

async function loadSettings(): Promise<void> {
  if (client === null || !pageLifecycleActive || refreshInFlight !== null || mutationInFlight) {
    return settingsInFlight ?? Promise.resolve();
  }
  if (settingsInFlight !== null) {
    return settingsInFlight;
  }
  const generation = pageLifecycleGeneration;
  const controller = pageRequestController;
  setBusy(true);
  const operation = (async () => {
    try {
      const settings = await client.json(
        "/api/manager/settings",
        { signal: controller.signal },
      );
      if (pageRequestIsCurrent(generation, controller)) {
        renderSettings(settings);
      }
    } finally {
      settingsInFlight = null;
      if (pageRequestIsCurrent(generation, controller)) {
        setBusy(false);
      }
    }
  })();
  settingsInFlight = operation;
  return operation;
}

async function refreshAll(): Promise<void> {
  if (client === null || !pageLifecycleActive || refreshInFlight !== null || settingsInFlight !== null || mutationInFlight) {
    return refreshInFlight ?? Promise.resolve();
  }
  const generation = pageLifecycleGeneration;
  const controller = pageRequestController;
  const request = (path: string): Promise<JsonObject> => client.json(
    path,
    { signal: controller.signal },
  );
  setBusy(true);
  const operation = (async () => {
    try {
      const [managerStatus, applicationStatus] = await Promise.all([
        request("/api/manager/status"),
        request("/api/status"),
      ]);
      if (!pageRequestIsCurrent(generation, controller)) {
        return;
      }
      renderManagerStatus(managerStatus);
      renderApplicationStatus(applicationStatus);
      if (managerStatus.service_active !== true) {
        renderServiceStopped();
      } else {
        const [sessions, agents, audit, diagnostics] = await Promise.all([
          request("/api/sessions"),
          request("/api/agents"),
          request("/api/audit"),
          request("/api/diagnostics"),
        ]);
        if (!pageRequestIsCurrent(generation, controller)) {
          return;
        }
        renderSessions(sessions);
        renderAgents(agents);
        renderAudit(audit);
        renderDiagnostics(diagnostics);
      }
      element("control-loading").hidden = true;
      element("control-error").hidden = true;
    } catch (reason: unknown) {
      if (pageRequestIsCurrent(generation, controller)) {
        setNotice(reason instanceof Error ? reason.message : "Manager refresh failed.", true);
      }
    } finally {
      refreshInFlight = null;
      if (pageRequestIsCurrent(generation, controller)) {
        setBusy(false);
      }
    }
  })();
  refreshInFlight = operation;
  return operation;
}

async function managerAction(action: "start" | "stop" | "restart"): Promise<void> {
  if (client === null || !pageLifecycleActive || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  const owner = beginMutation();
  let refreshAfter = false;
  try {
    if (action === "restart" && latestRestartCount === null) {
      throw new Error("The current manager restart generation is unavailable.");
    }
    const result = await client.post(
      MANAGER_ACTION_PATHS[action],
      {},
      owner.controller.signal,
    );
    if (!mutationIsCurrent(owner)) {
      return;
    }
    if (action === "restart") {
      if (result.state !== "restarting" || result.ok !== true) {
        throw new Error("The manager returned an invalid restart acknowledgement.");
      }
      stopRestartReconnect();
      expectedRestartCount = (latestRestartCount as number) + 1;
      restartReconnectAttempt = 0;
      restartReconnectActive = true;
      setText("manager-header-status", "Manager restarting");
      setNotice("Manager restart accepted. Waiting for the listener to return.");
      scheduleRestartReconnect();
      return;
    }
    renderManagerStatus(result);
    setNotice(`Manager ${action} request completed.`);
  } catch (reason: unknown) {
    if (mutationIsCurrent(owner)) {
      setNotice(reason instanceof Error ? reason.message : `Manager ${action} failed.`, true);
    }
  } finally {
    const current = mutationIsCurrent(owner);
    const retainForRestart = current && action === "restart" && restartReconnectActive;
    refreshAfter = current && action !== "restart";
    finishMutation(owner, retainForRestart);
  }
  if (refreshAfter) {
    await refreshAll();
  }
}

async function closeSession(sessionId: string): Promise<void> {
  if (client === null || !pageLifecycleActive || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  const owner = beginMutation();
  let refreshAfter = false;
  try {
    await client.post("/api/sessions/close", {
      session_id: sessionId,
      summary: "Closed from dashboard",
    }, owner.controller.signal);
    if (mutationIsCurrent(owner)) {
      setNotice("Session closed.");
    }
  } catch (reason: unknown) {
    if (mutationIsCurrent(owner)) {
      setNotice(reason instanceof Error ? reason.message : "Session close failed.", true);
    }
  } finally {
    refreshAfter = mutationIsCurrent(owner);
    finishMutation(owner);
  }
  if (refreshAfter) {
    await refreshAll();
  }
}

async function pruneSessions(): Promise<void> {
  if (client === null || !pageLifecycleActive || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  const owner = beginMutation();
  let refreshAfter = false;
  try {
    await client.post("/api/sessions/prune", {}, owner.controller.signal);
    if (mutationIsCurrent(owner)) {
      setNotice("Stale sessions pruned.");
    }
  } catch (reason: unknown) {
    if (mutationIsCurrent(owner)) {
      setNotice(reason instanceof Error ? reason.message : "Session pruning failed.", true);
    }
  } finally {
    refreshAfter = mutationIsCurrent(owner);
    finishMutation(owner);
  }
  if (refreshAfter) {
    await refreshAll();
  }
}

async function saveSettings(event: SubmitEvent): Promise<void> {
  event.preventDefault();
  if (client === null || !pageLifecycleActive || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  const form = element<HTMLFormElement>("manager-settings-form");
  if (!form.reportValidity()) {
    return;
  }
  const owner = beginMutation();
  let refreshAfter = false;
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
    }, owner.controller.signal);
    if (!mutationIsCurrent(owner)) {
      return;
    }
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
    if (mutationIsCurrent(owner)) {
      setNotice(reason instanceof Error ? reason.message : "Settings update failed.", true);
    }
  } finally {
    refreshAfter = mutationIsCurrent(owner);
    finishMutation(owner);
  }
  if (refreshAfter) {
    await refreshAll();
  }
}

async function runDoctor(): Promise<void> {
  if (client === null || !pageLifecycleActive || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  const owner = beginMutation();
  try {
    const report = await client.json(
      "/api/doctor",
      { signal: owner.controller.signal },
    );
    if (mutationIsCurrent(owner)) {
      renderDoctor(report);
      setNotice("Doctor completed.");
    }
  } catch (reason: unknown) {
    if (mutationIsCurrent(owner)) {
      setNotice(reason instanceof Error ? reason.message : "Doctor failed.", true);
    }
  } finally {
    finishMutation(owner);
  }
}

async function shutdownManager(): Promise<void> {
  if (client === null || !pageLifecycleActive || mutationInFlight || refreshInFlight !== null || settingsInFlight !== null) {
    return;
  }
  const owner = beginMutation();
  try {
    await client.post("/api/manager/shutdown", {}, owner.controller.signal);
    if (mutationIsCurrent(owner)) {
      setNotice("Manager is shutting down.");
      setText("manager-header-status", "Manager stopping");
    }
  } catch (reason: unknown) {
    if (mutationIsCurrent(owner)) {
      setNotice(reason instanceof Error ? reason.message : "Manager shutdown failed.", true);
    }
  } finally {
    finishMutation(owner);
  }
}

element("manager-start").addEventListener("click", () => void managerAction("start"));
element("manager-stop").addEventListener("click", () => void managerAction("stop"));
element("manager-restart").addEventListener("click", () => void managerAction("restart"));
element("manager-refresh").addEventListener("click", () => void refreshAll());
element("sessions-prune").addEventListener("click", () => void pruneSessions());
element("settings-reload").addEventListener("click", () => {
  const generation = pageLifecycleGeneration;
  const controller = pageRequestController;
  void loadSettings().catch((reason: unknown) => {
    if (pageRequestIsCurrent(generation, controller)) {
      setNotice(reason instanceof Error ? reason.message : "Settings reload failed.", true);
    }
  });
});
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
  if (pageLifecycleActive && !document.hidden) {
    void refreshAll();
  }
});

window.addEventListener("pagehide", () => {
  pageLifecycleActive = false;
  pageLifecycleGeneration += 1;
  pageRequestController.abort();
  mutationController?.abort();
  suspendRestartReconnect();
  stopPageRestore();
  if (autoRefreshTimer !== null) {
    window.clearInterval(autoRefreshTimer);
    autoRefreshTimer = null;
  }
});

window.addEventListener("pageshow", (event: PageTransitionEvent) => {
  if (!event.persisted || client === null) {
    return;
  }
  pageLifecycleActive = true;
  pageRequestController = new AbortController();
  setBusy(mutationInFlight || restartReconnectActive);
  if (restartReconnectActive && expectedRestartCount !== null) {
    scheduleRestartReconnect(0);
    return;
  }
  startPageRestore();
});

if (client !== null) {
  const generation = pageLifecycleGeneration;
  const controller = pageRequestController;
  void loadSettings()
    .then(() => {
      if (pageRequestIsCurrent(generation, controller)) {
        return refreshAll();
      }
      return undefined;
    })
    .catch((reason: unknown) => {
      if (pageRequestIsCurrent(generation, controller)) {
        setNotice(reason instanceof Error ? reason.message : "Dashboard initialization failed.", true);
        void refreshAll();
      }
    });
} else {
  element("control-loading").hidden = true;
  setText("manager-header-status", "Authentication required");
  setBusy(true);
}
