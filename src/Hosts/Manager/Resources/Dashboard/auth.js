// Shipped JavaScript contract: forge-dashboard-auth-v2; keep this source pair synchronized.
const SESSION_KEY = "forge.dashboard.bearer.v1";
const TOKEN_PATTERN = /^[0-9a-f]{64}$/i;
const DEFAULT_REQUEST_TIMEOUT_MS = 15_000;
const MAXIMUM_JSON_RESPONSE_BYTES = 2_080_768;

export class DashboardHttpError extends Error {
  constructor(status, code, message) {
    super(message);
    this.name = "DashboardHttpError";
    this.status = status;
    this.code = code;
  }
}

function currentDocumentPath() {
  return `${window.location.pathname}${window.location.search}`;
}

function fragmentToken() {
  if (window.location.hash.length <= 1) {
    return null;
  }
  const parameters = new URLSearchParams(window.location.hash.slice(1));
  return parameters.get("token");
}

function errorMessage(payload, fallback) {
  if (typeof payload !== "object" || payload === null) {
    return { code: "http_error", message: fallback };
  }
  const value = payload;
  const nested = typeof value.error === "object" && value.error !== null
    ? value.error
    : value;
  return {
    code: typeof nested.code === "string" ? nested.code : "http_error",
    message: typeof nested.message === "string" ? nested.message : fallback,
  };
}

async function readBoundedJson(response) {
  if (response.body === null) {
    throw new Error("The dashboard returned no response body.");
  }

  const reader = response.body.getReader();
  const bytes = new Uint8Array(MAXIMUM_JSON_RESPONSE_BYTES);
  let length = 0;
  try {
    while (true) {
      const next = await reader.read();
      if (next.done) {
        break;
      }
      if (length + next.value.byteLength > bytes.byteLength) {
        throw new Error("The dashboard response exceeded the browser JSON limit.");
      }
      bytes.set(next.value, length);
      length += next.value.byteLength;
    }
  } finally {
    reader.releaseLock();
  }

  const text = new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(0, length));
  return JSON.parse(text);
}

export class DashboardClient {
  constructor(token) {
    this.token = token;
  }

  static bootstrap() {
    let token = null;
    try {
      const supplied = fragmentToken();
      if (supplied !== null) {
        if (!TOKEN_PATTERN.test(supplied)) {
          window.sessionStorage.removeItem(SESSION_KEY);
          window.history.replaceState(null, document.title, currentDocumentPath());
          return null;
        }
        window.sessionStorage.setItem(SESSION_KEY, supplied.toLowerCase());
      }
      if (window.location.hash.length > 0) {
        window.history.replaceState(null, document.title, currentDocumentPath());
      }
      token = window.sessionStorage.getItem(SESSION_KEY);
    } catch {
      if (window.location.hash.length > 0) {
        window.history.replaceState(null, document.title, currentDocumentPath());
      }
      return null;
    }

    return token !== null && TOKEN_PATTERN.test(token)
      ? new DashboardClient(token.toLowerCase())
      : null;
  }

  async json(path, init = {}) {
    return this.withResponse(path, init, DEFAULT_REQUEST_TIMEOUT_MS, async (response) => {
      const payload = await readBoundedJson(response);
      if (typeof payload !== "object" || payload === null || Array.isArray(payload)) {
        throw new DashboardHttpError(response.status, "invalid_response", "The dashboard returned an invalid JSON document.");
      }
      return payload;
    });
  }

  async post(path, body) {
    return this.json(path, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
  }

  async stream(path, signal, consume) {
    return this.withResponse(path, { method: "GET", signal }, null, consume);
  }

  navigateToReboundDashboard(canonicalUrl) {
    const destination = new URL(canonicalUrl);
    if (destination.protocol !== "http:" || destination.username.length !== 0 || destination.password.length !== 0) {
      throw new Error("The manager returned an invalid dashboard URL.");
    }
    destination.hash = `token=${encodeURIComponent(this.token)}`;
    window.location.assign(destination.toString());
  }

  async withResponse(path, init, timeoutMs, consume) {
    if (!(path.startsWith("/api/") || path === "/ping")) {
      throw new Error("Dashboard requests must use a documented local API path.");
    }

    const controller = new AbortController();
    const externalSignal = init.signal;
    const cancelFromExternal = () => controller.abort();
    if (externalSignal !== null && externalSignal !== undefined) {
      if (externalSignal.aborted) {
        controller.abort();
      } else {
        externalSignal.addEventListener("abort", cancelFromExternal, { once: true });
      }
    }

    const timeout = timeoutMs === null
      ? null
      : window.setTimeout(() => controller.abort(), timeoutMs);
    try {
      const headers = new Headers(init.headers);
      headers.set("Authorization", `Bearer ${this.token}`);
      headers.set("Accept", path === "/api/stream" || path.startsWith("/api/stream?")
        ? "text/event-stream"
        : "application/json");
      const response = await window.fetch(path, {
        ...init,
        cache: "no-store",
        credentials: "omit",
        redirect: "error",
        headers,
        signal: controller.signal,
      });
      if (!response.ok) {
        let payload = null;
        try {
          payload = await readBoundedJson(response);
        } catch {
          payload = null;
        }
        const detail = errorMessage(payload, `Dashboard request failed with HTTP ${response.status}.`);
        throw new DashboardHttpError(response.status, detail.code, detail.message);
      }
      return await consume(response);
    } finally {
      controller.abort();
      if (timeout !== null) {
        window.clearTimeout(timeout);
      }
      externalSignal?.removeEventListener("abort", cancelFromExternal);
    }
  }
}

export function requireDashboardClient(errorElement) {
  const client = DashboardClient.bootstrap();
  if (client !== null) {
    return client;
  }
  errorElement.textContent = "Dashboard authentication is unavailable. Open the dashboard from Forge Conductor Manager to establish this browser session.";
  errorElement.hidden = false;
  return null;
}
