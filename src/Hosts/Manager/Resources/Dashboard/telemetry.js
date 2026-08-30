// Shipped JavaScript contract: forge-dashboard-telemetry-v2; keep this source pair synchronized.
import { requireDashboardClient } from "./auth.js";

const MAXIMUM_EVENT_BUFFER_BYTES = 2_097_152;
const MAXIMUM_HISTORY_ROWS = 20;
const MAXIMUM_TOOL_ROWS = 128;
const MAXIMUM_AGENT_ROWS = 128;
const MAXIMUM_PROCESS_ROWS = 12;
const MAXIMUM_FEED_ROWS = 40;
const MAXIMUM_RECONNECT_DELAY_MS = 30_000;
const SILENCE_REFRESH_AFTER_MS = 2_500;
const SILENCE_RECONNECT_AFTER_MS = 5_000;
const SILENCE_WATCHDOG_INTERVAL_MS = 500;

function element(id) {
  const value = document.getElementById(id);
  if (value === null) {
    throw new Error(`Required dashboard element is missing: ${id}`);
  }
  return value;
}

function object(value) {
  return typeof value === "object" && value !== null && !Array.isArray(value) ? value : null;
}

function array(value) {
  return Array.isArray(value) ? value : [];
}

function text(value) {
  return typeof value === "string" && value.length > 0 ? value : null;
}

function number(value) {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

function displayText(value) {
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

function fixed(value, digits = 1) {
  const numeric = number(value);
  return numeric === null ? "Unavailable" : numeric.toFixed(digits);
}

function percent(value) {
  const numeric = number(value);
  return numeric === null ? "Unavailable" : `${numeric.toFixed(1)}%`;
}

function gibibytes(value) {
  const numeric = number(value);
  return numeric === null ? "Unavailable" : `${numeric.toFixed(2)} GiB`;
}

function timestamp(value) {
  const seconds = number(value);
  if (seconds !== null) {
    const date = new Date(seconds * 1000);
    return Number.isNaN(date.valueOf()) ? "Unavailable" : date.toLocaleString();
  }
  const encoded = text(value);
  if (encoded === null) {
    return "Unavailable";
  }
  const date = new Date(encoded);
  return Number.isNaN(date.valueOf()) ? encoded : date.toLocaleString();
}

function setText(id, value) {
  element(id).textContent = value;
}

function tableMessage(bodyId, columnCount, message) {
  const row = document.createElement("tr");
  const cell = document.createElement("td");
  cell.colSpan = columnCount;
  cell.textContent = message;
  row.append(cell);
  element(bodyId).replaceChildren(row);
}

function appendRow(body, values) {
  const row = document.createElement("tr");
  for (const value of values) {
    const cell = document.createElement("td");
    cell.textContent = value;
    row.append(cell);
  }
  body.append(row);
}

function renderSystem(system) {
  const cpu = object(system.cpu);
  const ram = object(system.ram);
  const diskIo = object(system.disk_io);
  const gpus = array(system.gpu).map(object).filter((value) => value !== null);

  setText("system-cpu", cpu === null ? "Unavailable" : percent(cpu.percent));
  setText("system-frequency", cpu === null || number(cpu.freq_mhz) === null
    ? "Unavailable"
    : `${fixed(cpu.freq_mhz, 0)} MHz`);
  setText("system-memory", ram === null ? "Unavailable" : `${percent(ram.percent)} used (${gibibytes(ram.used_gb)} of ${gibibytes(ram.total_gb)})`);
  setText("system-gpu", gpus.length === 0 ? "Unavailable" : percent(gpus[0].util_gpu));
  setText("system-disk-io", diskIo === null || number(diskIo.total_mb_s) === null
    ? "Unavailable"
    : `${fixed(diskIo.total_mb_s)} MiB/s`);
  setText("system-host", [text(system.host), text(system.platform), text(system.arch)].filter((value) => value !== null).join(" · ") || "Unavailable");

  renderCpuCores(cpu);
  renderGpus(gpus);
  renderStorage(array(system.disk));
  renderProcesses(array(system.processes));
}

function renderCpuCores(cpu) {
  const list = element("cpu-core-list");
  list.replaceChildren();
  if (cpu === null) {
    const item = document.createElement("li");
    item.textContent = "Unavailable";
    list.append(item);
    setText("cpu-cores-summary", "Unavailable");
    return;
  }

  const utilization = array(cpu.per_cpu);
  const frequencies = array(cpu.freq_per_core_mhz);
  setText("cpu-cores-summary", `${displayText(cpu.count_logical)} logical processors · ${displayText(cpu.count_physical)} physical cores · ${displayText(cpu.brand)}`);
  if (utilization.length === 0) {
    const item = document.createElement("li");
    item.textContent = "Per-core utilization unavailable";
    list.append(item);
    return;
  }
  utilization.slice(0, 256).forEach((value, index) => {
    const item = document.createElement("li");
    const frequency = number(frequencies[index]);
    item.textContent = `Logical processor ${index + 1}: ${percent(value)}${frequency === null ? "" : ` · ${frequency.toFixed(0)} MHz`}`;
    list.append(item);
  });
}

function renderGpus(gpus) {
  const list = element("gpu-core-list");
  list.replaceChildren();
  if (gpus.length === 0) {
    const item = document.createElement("li");
    item.textContent = "Unavailable";
    list.append(item);
    return;
  }
  gpus.slice(0, 16).forEach((gpu, index) => {
    const item = document.createElement("li");
    const name = text(gpu.name) ?? `GPU ${index + 1}`;
    const memory = number(gpu.mem_used_mib) === null || number(gpu.mem_total_mib) === null
      ? "memory unavailable"
      : `${fixed(gpu.mem_used_mib, 0)} of ${fixed(gpu.mem_total_mib, 0)} MiB`;
    const coreData = gpu.cores === null || gpu.cores === undefined ? "engine/core detail unavailable" : `${displayText(gpu.cores)} cores`;
    item.textContent = `${name}: ${percent(gpu.util_gpu)} · ${memory} · ${coreData}`;
    list.append(item);
  });
}

function renderStorage(values) {
  const disks = values.map(object).filter((value) => value !== null);
  if (disks.length === 0) {
    tableMessage("storage-rows", 4, "Unavailable");
    return;
  }
  const body = element("storage-rows");
  body.replaceChildren();
  disks.slice(0, 32).forEach((disk) => appendRow(body, [
    text(disk.mount) ?? text(disk.device) ?? "Unavailable",
    `${gibibytes(disk.used_gb)} (${percent(disk.percent)})`,
    gibibytes(disk.available_gb),
    gibibytes(disk.total_gb),
  ]));
}

function renderProcesses(values) {
  const processes = values.map(object).filter((value) => value !== null);
  if (processes.length === 0) {
    tableMessage("process-rows", 5, "Unavailable");
    return;
  }
  const body = element("process-rows");
  body.replaceChildren();
  processes.slice(0, MAXIMUM_PROCESS_ROWS).forEach((process) => appendRow(body, [
    displayText(process.name),
    displayText(process.pid),
    percent(process.cpu_percent),
    gibibytes(process.rss_gb),
    displayText(process.source),
  ]));
}

function renderHistory(values) {
  const history = values.map(object).filter((value) => value !== null).slice(-MAXIMUM_HISTORY_ROWS).reverse();
  if (history.length === 0) {
    tableMessage("load-trace-rows", 6, "No samples available.");
    return;
  }
  const body = element("load-trace-rows");
  body.replaceChildren();
  history.forEach((point) => appendRow(body, [
    timestamp(point.ts),
    percent(point.cpu),
    percent(point.ram),
    percent(point.gpu),
    number(point.disk_io) === null ? "Unavailable" : `${fixed(point.disk_io)} MiB/s`,
    displayText(point.mcp),
  ]));
}

function renderOrchestration(value) {
  const orchestration = object(value);
  const container = element("orchestration-values");
  container.replaceChildren();
  const definitions = orchestration === null
    ? [["Health", null]]
    : [
      ["Health", orchestration.health_label ?? orchestration.health],
      ["Mode", orchestration.mode],
      ["Home", orchestration.home],
      ["Manager state", orchestration.manager_state],
      ["Manager PID", orchestration.manager_pid],
      ["Primary alive", orchestration.primary_alive],
      ["Fallback alive", orchestration.fallback_alive],
      ["Watchdog alive", orchestration.watchdog_alive],
      ["Serve processes", orchestration.serve_count],
      ["Supervisors", orchestration.supervise_count],
      ["Heartbeat age", number(orchestration.heartbeat_age_sec) === null ? null : `${fixed(orchestration.heartbeat_age_sec)} seconds`],
      ["Failover events (1 hour)", orchestration.failover_events_1h],
    ];
  for (const [label, raw] of definitions) {
    const wrapper = document.createElement("div");
    const term = document.createElement("dt");
    const detail = document.createElement("dd");
    term.textContent = label;
    detail.textContent = displayText(raw);
    wrapper.append(term, detail);
    container.append(wrapper);
  }
}

function renderMcpServers(value) {
  const raw = Array.isArray(value) ? value : null;
  const servers = raw === null ? [] : raw.map(object).filter((entry) => entry !== null);
  const container = element("mcp-server-list");
  container.replaceChildren();
  if (servers.length === 0) {
    const message = document.createElement("p");
    message.textContent = raw === null || raw.length !== 0 ? "Unavailable" : "No MCP servers reported.";
    container.append(message);
    return;
  }
  servers.slice(0, 32).forEach((server, index) => {
    const article = document.createElement("article");
    const heading = document.createElement("h3");
    const status = document.createElement("p");
    heading.textContent = text(server.name) ?? text(server.id) ?? `Server ${index + 1}`;
    status.textContent = `Status: ${displayText(server.status ?? server.health)}`;
    article.append(heading, status);
    container.append(article);
  });
}

function renderTools(value) {
  const raw = Array.isArray(value) ? value : null;
  const tools = raw === null ? [] : raw.map(object).filter((entry) => entry !== null);
  const emptyMessage = raw === null || raw.length !== 0 ? "Unavailable" : "No MCP tools reported.";
  setText("mcp-tools-summary", tools.length === 0 ? emptyMessage : `${tools.length} tools reported`);
  if (tools.length === 0) {
    tableMessage("mcp-tool-rows", 4, emptyMessage);
    return;
  }
  const body = element("mcp-tool-rows");
  body.replaceChildren();
  tools.slice(0, MAXIMUM_TOOL_ROWS).forEach((tool) => appendRow(body, [
    displayText(tool.name),
    displayText(tool.pack),
    displayText(tool.health_label ?? tool.status),
    displayText(tool.effect),
  ]));
}

function renderAgents(value) {
  const forge = object(value);
  const raw = forge !== null && Array.isArray(forge.agent_sessions) ? forge.agent_sessions : null;
  const sessions = raw === null ? [] : raw.map(object).filter((entry) => entry !== null);
  const summary = forge === null ? null : object(forge.agents_summary);
  const emptyMessage = raw === null || raw.length !== 0 ? "Unavailable" : "No agent sessions reported.";
  setText("agent-summary", summary === null
    ? (sessions.length === 0 ? emptyMessage : `${sessions.length} sessions`)
    : `${displayText(summary.open)} open of ${displayText(summary.total)} sessions`);
  if (sessions.length === 0) {
    tableMessage("agent-session-rows", 4, emptyMessage);
    return;
  }
  const body = element("agent-session-rows");
  body.replaceChildren();
  sessions.slice(0, MAXIMUM_AGENT_ROWS).forEach((session) => appendRow(body, [
    displayText(session.agent_id),
    displayText(session.status),
    displayText(session.summary),
    timestamp(session.updated_at),
  ]));
}

function renderFeed(value) {
  const raw = Array.isArray(value) ? value : null;
  const entries = raw === null ? [] : raw.map(object).filter((entry) => entry !== null);
  const container = element("live-feed-list");
  container.replaceChildren();
  if (entries.length === 0) {
    const message = document.createElement("p");
    message.textContent = raw === null || raw.length !== 0 ? "Feed unavailable" : "No live feed entries reported.";
    container.append(message);
    return;
  }
  entries.slice(0, MAXIMUM_FEED_ROWS).forEach((entry, index) => {
    const article = document.createElement("article");
    const heading = document.createElement("h3");
    const detail = document.createElement("p");
    heading.textContent = text(entry.title) ?? text(entry.kind) ?? `Activity ${index + 1}`;
    detail.textContent = text(entry.summary) ?? text(entry.message) ?? displayText(entry.status);
    article.append(heading, detail);
    container.append(article);
  });
}

function renderFrame(frame) {
  const system = object(frame.system);
  const forge = object(frame.forge);
  if (system !== null) {
    renderSystem(system);
  }
  renderHistory(array(frame.history));
  renderOrchestration(forge?.orchestration);
  renderMcpServers(forge?.mcp_servers);
  renderTools(forge?.mcp_tools);
  renderAgents(forge);
  renderFeed(forge?.live_feed);

  const updated = timestamp(frame.updated);
  setText("dashboard-updated", updated);
  setText("stream-sample-rate", number(frame.sample_hz) === null ? "Unavailable" : `${fixed(frame.sample_hz)} Hz`);
  setText("stream-runtime", displayText(frame.runtime));
  setText("dashboard-version", `Runtime: ${displayText(frame.runtime)}`);
  element("dashboard-loading").hidden = true;
  element("dashboard-error").hidden = true;
}

const authError = element("dashboard-auth-error");
const client = requireDashboardClient(authError);
let active = true;
let streamController = null;
let reconnectTimer = null;
let reconnectDelayMs = 1_000;
let streamConnecting = false;
let pendingFrame = null;
let renderScheduled = false;
let refreshInFlight = null;
let refreshController = null;
let silenceWatchdogTimer = null;
let streamConnectedAtMs = null;
let lastStreamFrameAtMs = null;
let lastFallbackRefreshAtMs = null;

function reportError(message) {
  const target = element("dashboard-error");
  target.textContent = message;
  target.hidden = false;
}

function queueFrame(frame) {
  pendingFrame = frame;
  if (renderScheduled) {
    return;
  }
  renderScheduled = true;
  window.requestAnimationFrame(() => {
    renderScheduled = false;
    const current = pendingFrame;
    pendingFrame = null;
    if (current !== null) {
      renderFrame(current);
    }
  });
}

async function refreshOnce() {
  if (client === null || document.hidden || refreshInFlight !== null) {
    return refreshInFlight ?? Promise.resolve();
  }
  element("dashboard-refresh").disabled = true;
  const controller = new AbortController();
  refreshController = controller;
  const operation = client.json("/api/live", { signal: controller.signal })
    .then((frame) => queueFrame(frame))
    .catch((reason) => {
      if (!controller.signal.aborted) {
        reportError(reason instanceof Error ? reason.message : "Telemetry refresh failed.");
      }
    })
    .finally(() => {
      element("dashboard-refresh").disabled = false;
      if (refreshController === controller) {
        refreshController = null;
      }
      refreshInFlight = null;
    });
  refreshInFlight = operation;
  return operation;
}

function parseEvent(block) {
  let eventName = "message";
  const data = [];
  for (const line of block.split(/\r?\n/)) {
    if (line.startsWith("event:")) {
      eventName = line.slice(6).trim();
    } else if (line.startsWith("data:")) {
      data.push(line.slice(5).trimStart());
    }
  }
  if (eventName !== "telemetry" || data.length === 0) {
    return;
  }
  const frame = object(JSON.parse(data.join("\n")));
  if (frame !== null) {
    lastStreamFrameAtMs = window.performance.now();
    lastFallbackRefreshAtMs = null;
    queueFrame(frame);
  }
}

function clearSilenceWatchdog() {
  if (silenceWatchdogTimer !== null) {
    window.clearInterval(silenceWatchdogTimer);
    silenceWatchdogTimer = null;
  }
  streamConnectedAtMs = null;
  lastStreamFrameAtMs = null;
  lastFallbackRefreshAtMs = null;
}

function startSilenceWatchdog(controller) {
  clearSilenceWatchdog();
  streamConnectedAtMs = window.performance.now();
  silenceWatchdogTimer = window.setInterval(() => {
    if (!active || document.hidden || streamController !== controller || controller.signal.aborted) {
      clearSilenceWatchdog();
      return;
    }
    const now = window.performance.now();
    const lastFrame = lastStreamFrameAtMs ?? streamConnectedAtMs ?? now;
    const silenceMs = now - lastFrame;
    if (silenceMs > SILENCE_RECONNECT_AFTER_MS) {
      clearSilenceWatchdog();
      controller.abort();
      return;
    }
    if (silenceMs >= SILENCE_REFRESH_AFTER_MS &&
        (lastFallbackRefreshAtMs === null || now - lastFallbackRefreshAtMs >= SILENCE_REFRESH_AFTER_MS)) {
      lastFallbackRefreshAtMs = now;
      void refreshOnce();
    }
  }, SILENCE_WATCHDOG_INTERVAL_MS);
}

async function consumeStream(response, signal) {
  if (response.body === null) {
    throw new Error("The telemetry stream returned no body.");
  }
  const reader = response.body.getReader();
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let buffer = "";
  try {
    while (!signal.aborted) {
      const next = await reader.read();
      if (next.done) {
        break;
      }
      buffer += decoder.decode(next.value, { stream: true });
      if (buffer.length > MAXIMUM_EVENT_BUFFER_BYTES) {
        throw new Error("The telemetry stream event exceeded the browser buffer limit.");
      }
      let delimiter = buffer.search(/\r?\n\r?\n/);
      while (delimiter >= 0) {
        const block = buffer.slice(0, delimiter);
        const match = buffer.slice(delimiter).match(/^\r?\n\r?\n/);
        buffer = buffer.slice(delimiter + (match?.[0].length ?? 2));
        if (block.length > 0) {
          parseEvent(block);
        }
        delimiter = buffer.search(/\r?\n\r?\n/);
      }
    }
  } finally {
    reader.releaseLock();
  }
}

function scheduleReconnect() {
  if (!active || document.hidden || reconnectTimer !== null) {
    return;
  }
  reconnectTimer = window.setTimeout(() => {
    reconnectTimer = null;
    if (refreshInFlight !== null) {
      void refreshInFlight.finally(() => scheduleReconnect());
      return;
    }
    void connectStream();
  }, reconnectDelayMs);
  reconnectDelayMs = Math.min(reconnectDelayMs * 2, MAXIMUM_RECONNECT_DELAY_MS);
}

async function connectStream() {
  if (client === null || !active || document.hidden || streamConnecting || streamController !== null) {
    return;
  }
  streamConnecting = true;
  const controller = new AbortController();
  streamController = controller;
  setText("dashboard-connection-status", "Connecting");
  setText("stream-transport", "Authenticated streaming fetch");
  try {
    await client.stream("/api/stream?hz=2", controller.signal, async (response) => {
      reconnectDelayMs = 1_000;
      setText("dashboard-connection-status", "Connected");
      startSilenceWatchdog(controller);
      await consumeStream(response, controller.signal);
    });
    if (!controller.signal.aborted) {
      throw new Error("The telemetry stream closed.");
    }
  } catch (reason) {
    if (!controller.signal.aborted) {
      setText("dashboard-connection-status", "Reconnecting");
      reportError(reason instanceof Error ? reason.message : "The telemetry stream failed.");
      await refreshOnce();
    }
  } finally {
    clearSilenceWatchdog();
    if (streamController === controller) {
      streamController = null;
    }
    streamConnecting = false;
    scheduleReconnect();
  }
}

element("dashboard-refresh").addEventListener("click", () => void refreshOnce());

document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    streamController?.abort();
    refreshController?.abort();
    clearSilenceWatchdog();
    if (reconnectTimer !== null) {
      window.clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
    setText("dashboard-connection-status", "Paused while hidden");
  } else {
    void refreshOnce().finally(() => connectStream());
  }
});

window.addEventListener("pagehide", () => {
  active = false;
  streamController?.abort();
  refreshController?.abort();
  clearSilenceWatchdog();
  if (reconnectTimer !== null) {
    window.clearTimeout(reconnectTimer);
  }
});

if (client !== null) {
  void refreshOnce().finally(() => connectStream());
} else {
  element("dashboard-loading").hidden = true;
  setText("dashboard-connection-status", "Authentication required");
}
