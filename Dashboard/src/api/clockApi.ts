import type {
  ChartPoint,
  DashboardData,
  DashboardFeedTick,
  DatabaseConnectionInfo,
  DatabaseData,
  DataMode,
  DataSourceConfig,
  DeviceData,
  MetricRangeDataset,
  Range,
  SensorKey,
  SleepRangeDataset,
} from "../types/clock";
import { applyExternalFeedTick, generateNextMockTick } from "./mockFeed";
import {
  DEFAULT_NETPIE_CONFIG,
  fetchNetpieDatabaseData,
  fetchNetpieDeviceData,
  fetchNetpieSnapshot,
} from "./netpieClient";

const STORAGE_KEY = "clock.dataSourceConfig.v3";
const RANGES: Range[] = ["Hour", "Day", "Week", "Month"];
const SENSOR_KEYS: SensorKey[] = ["Temperature", "Humidity", "Luminosity"];

function defaultModeFromEnv(): DataMode {
  const explicitMode = import.meta.env.VITE_DATA_MODE as string | undefined;
  if (explicitMode === "netpie" || explicitMode === "api" || explicitMode === "dev") {
    return explicitMode;
  }
  const envApiUrl = import.meta.env.VITE_API_BASE_URL as string | undefined;
  if (envApiUrl && envApiUrl.trim().length > 0) {
    return "api";
  }
  return "netpie";
}

export function getDefaultDataSourceConfig(): DataSourceConfig {
  const envInterval = Number(import.meta.env.VITE_FEED_INTERVAL_MS);
  return {
    mode: defaultModeFromEnv(),
    netpie: { ...DEFAULT_NETPIE_CONFIG },
    mockBasePath: (import.meta.env.VITE_MOCK_DATA_PATH as string | undefined)?.trim() || "/mock",
    apiBaseUrl: (import.meta.env.VITE_API_BASE_URL as string | undefined)?.trim() || "/api",
    streamUrl: (import.meta.env.VITE_STREAM_URL as string | undefined)?.trim() || "",
    feedEnabled: (import.meta.env.VITE_FEED_ENABLED as string | undefined) !== "false",
    feedIntervalMs:
      Number.isFinite(envInterval) && envInterval >= 1000 ? Math.round(envInterval) : 2500,
  };
}

export function loadDataSourceConfig(): DataSourceConfig {
  const defaults = getDefaultDataSourceConfig();
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY);
    if (!raw) return defaults;
    const parsed = JSON.parse(raw) as Partial<DataSourceConfig>;
    const interval = Number(parsed.feedIntervalMs);
    return {
      mode:
        parsed.mode === "netpie" || parsed.mode === "api" || parsed.mode === "dev"
          ? parsed.mode
          : defaults.mode,
      netpie: {
        clientId: parsed.netpie?.clientId?.trim() || defaults.netpie.clientId,
        token: parsed.netpie?.token?.trim() || defaults.netpie.token,
        secret: parsed.netpie?.secret?.trim() || defaults.netpie.secret,
      },
      mockBasePath: parsed.mockBasePath?.trim() || defaults.mockBasePath,
      apiBaseUrl: parsed.apiBaseUrl?.trim() || defaults.apiBaseUrl,
      streamUrl: parsed.streamUrl?.trim() ?? defaults.streamUrl,
      feedEnabled:
        typeof parsed.feedEnabled === "boolean" ? parsed.feedEnabled : defaults.feedEnabled,
      feedIntervalMs:
        Number.isFinite(interval) && interval >= 400
          ? Math.round(interval)
          : defaults.feedIntervalMs,
    };
  } catch {
    return defaults;
  }
}

export function saveDataSourceConfig(config: DataSourceConfig): void {
  try {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify(config));
  } catch {
    // Ignore storage quota / private browsing errors
  }
}

export function resolveEndpointUrl(
  config: DataSourceConfig,
  resource: "dashboard" | "device" | "database",
  query?: Record<string, string | number | undefined>,
): string {
  if (config.mode === "netpie") {
    return resource === "device"
      ? "https://api.netpie.io/v2/device/status + /shadow/data"
      : "https://api.netpie.io/v2/device/shadow/data";
  }

  if (config.mode === "dev") {
    const base = config.mockBasePath.replace(/\/+$/, "");
    return `${base}/${resource}.json`;
  }

  const base = config.apiBaseUrl.replace(/\/+$/, "");
  const search = new URLSearchParams();
  if (query) {
    for (const [key, value] of Object.entries(query)) {
      if (value !== undefined && value !== "") {
        search.set(key, String(value));
      }
    }
  }
  const queryString = search.toString();
  return queryString ? `${base}/${resource}?${queryString}` : `${base}/${resource}`;
}

async function requestJson<T>(url: string, init?: RequestInit): Promise<T> {
  const response = await fetch(url, {
    headers: {
      Accept: "application/json",
      ...(init?.body ? { "Content-Type": "application/json" } : {}),
      ...init?.headers,
    },
    ...init,
  });

  if (!response.ok) {
    throw new Error(
      `HTTP ${response.status} (${response.statusText || "Request failed"}) at ${url}`,
    );
  }

  return (await response.json()) as T;
}

function deriveXTicks(points: ChartPoint[], maxTicks: number): string[] {
  if (points.length <= maxTicks) {
    return points.map((p) => p.label);
  }
  const ticks: string[] = [];
  const steps = maxTicks - 1;
  for (let i = 0; i <= steps; i += 1) {
    const idx = Math.round((i / steps) * (points.length - 1));
    const label = points[idx]?.label;
    if (label && !ticks.includes(label)) {
      ticks.push(label);
    }
  }
  return ticks;
}

function defaultXLabel(range: Range): string {
  if (range === "Week") return "Day";
  if (range === "Month") return "Date";
  return "Time";
}

function normalizeRangeDataset(
  raw: Partial<MetricRangeDataset> | undefined,
  range: Range,
  wide: boolean,
): MetricRangeDataset {
  const points = (raw?.points ?? []).map((pt) => ({
    label: String(pt.label),
    detailLabel: pt.detailLabel ? String(pt.detailLabel) : String(pt.label),
    value: Number(pt.value),
  }));
  const latest = points[points.length - 1]?.value ?? 0;
  const maxTicks = range === "Week" ? 7 : wide ? 6 : 4;

  return {
    headline: raw?.headline !== undefined ? Number(raw.headline) : latest,
    xLabel: raw?.xLabel || defaultXLabel(range),
    xTicks: raw?.xTicks?.length ? raw.xTicks.map(String) : deriveXTicks(points, maxTicks),
    points,
  };
}

function normalizeSleepRangeDataset(
  raw: Partial<SleepRangeDataset> | undefined,
  range: Range,
): SleepRangeDataset {
  const base = normalizeRangeDataset(raw, range, true);
  return {
    ...base,
    summary: {
      sleepOnsetLatencyMinutes: Number(raw?.summary?.sleepOnsetLatencyMinutes ?? 0),
      totalSleepTime: String(raw?.summary?.totalSleepTime ?? "0:00"),
      wakeAfterSleepOnsetMinutes: Number(raw?.summary?.wakeAfterSleepOnsetMinutes ?? 0),
    },
  };
}

export function normalizeDashboardPayload(raw: DashboardData): DashboardData {
  const sleepRanges = {} as Record<Range, SleepRangeDataset>;
  for (const range of RANGES) {
    sleepRanges[range] = normalizeSleepRangeDataset(raw.sleepActivity?.ranges?.[range], range);
  }

  const sensors = {} as Record<SensorKey, DashboardData["sensors"][SensorKey]>;
  for (const key of SENSOR_KEYS) {
    const rawSensor = raw.sensors?.[key];
    const sensorRanges = {} as Record<Range, MetricRangeDataset>;
    for (const range of RANGES) {
      sensorRanges[range] = normalizeRangeDataset(rawSensor?.ranges?.[range], range, false);
    }
    sensors[key] = {
      name: rawSensor?.name || key,
      unit: rawSensor?.unit || "",
      ranges: sensorRanges,
    };
  }

  return {
    alarm: {
      currentTime: String(raw.alarm?.currentTime ?? "--:--"),
      alarmTime: String(raw.alarm?.alarmTime ?? "--:--"),
      lockDurationMinutes: Number(raw.alarm?.lockDurationMinutes ?? 0),
      lockStatus: String(raw.alarm?.lockStatus ?? "Unknown"),
      connected: Boolean(raw.alarm?.connected),
      connectionLabel: raw.alarm?.connectionLabel,
    },
    sleepActivity: {
      title: raw.sleepActivity?.title || "Sleep Activity",
      unit: raw.sleepActivity?.unit || "%",
      ranges: sleepRanges,
    },
    sensors,
  };
}

export async function fetchDashboardData(
  config: DataSourceConfig,
  signal?: AbortSignal,
): Promise<DashboardData> {
  if (config.mode === "netpie") {
    return fetchNetpieSnapshot(config.netpie, false, signal);
  }
  const url = resolveEndpointUrl(config, "dashboard");
  const raw = await requestJson<DashboardData>(url, { signal });
  return normalizeDashboardPayload(raw);
}

function isFullDashboardPayload(payload: unknown): payload is DashboardData {
  if (!payload || typeof payload !== "object") return false;
  const candidate = payload as Record<string, unknown>;
  return (
    typeof candidate.sleepActivity === "object" &&
    candidate.sleepActivity !== null &&
    "ranges" in (candidate.sleepActivity as Record<string, unknown>)
  );
}

function extractFeedTick(payload: unknown): DashboardFeedTick | null {
  if (!payload || typeof payload !== "object") return null;
  const record = payload as Record<string, unknown>;
  if (record.tick && typeof record.tick === "object") {
    return record.tick as DashboardFeedTick;
  }
  if (
    typeof record.sleepActivity === "number" ||
    typeof record.currentTime === "string" ||
    (record.sensors && typeof record.sensors === "object")
  ) {
    return record as DashboardFeedTick;
  }
  return null;
}

export function subscribeDashboardFeed(
  config: DataSourceConfig,
  callbacks: {
    onData: (data: DashboardData) => void;
    onError: (message: string) => void;
  },
): () => void {
  let disposed = false;
  let currentData: DashboardData | null = null;
  let timerId: number | undefined;
  let ws: WebSocket | undefined;
  let eventSource: EventSource | undefined;
  const controller = new AbortController();

  function handleIncomingPayload(payload: unknown) {
    if (disposed) return;
    if (isFullDashboardPayload(payload)) {
      currentData = normalizeDashboardPayload(payload);
      callbacks.onData(currentData);
      return;
    }
    const tick = extractFeedTick(payload);
    if (tick && currentData) {
      currentData = applyExternalFeedTick(currentData, tick);
      callbacks.onData(currentData);
    }
  }

  fetchDashboardData(config, controller.signal)
    .then((initial) => {
      if (disposed) return;
      currentData = initial;
      callbacks.onData(initial);

      if (!config.feedEnabled) {
        return;
      }

      if (config.mode === "netpie") {
        let netpiePollInFlight = false;
        const safeInterval = Math.max(1500, config.feedIntervalMs);
        timerId = window.setInterval(() => {
          if (disposed || netpiePollInFlight) return;
          netpiePollInFlight = true;
          fetchNetpieSnapshot(config.netpie, true, controller.signal)
            .then((nextData) => {
              if (disposed) return;
              currentData = nextData;
              callbacks.onData(nextData);
            })
            .catch(() => {
              // Preserve last valid NETPIE snapshot during transient poll errors
            })
            .finally(() => {
              netpiePollInFlight = false;
            });
        }, safeInterval);
        return;
      }

      if (config.mode === "dev") {
        let stepIndex = 1;
        timerId = window.setInterval(() => {
          if (disposed || !currentData) return;
          currentData = generateNextMockTick(currentData, stepIndex);
          stepIndex += 1;
          callbacks.onData(currentData);
        }, config.feedIntervalMs);
        return;
      }

      const streamUrl = config.streamUrl?.trim();
      if (streamUrl && /^wss?:\/\//i.test(streamUrl)) {
        ws = new WebSocket(streamUrl);
        ws.onmessage = (event) => {
          try {
            handleIncomingPayload(JSON.parse(String(event.data)));
          } catch {
            // Ignore malformed stream frames
          }
        };
        return;
      }

      if (streamUrl) {
        eventSource = new EventSource(streamUrl);
        eventSource.onmessage = (event) => {
          try {
            handleIncomingPayload(JSON.parse(event.data));
          } catch {
            // Ignore malformed SSE frames
          }
        };
        return;
      }

      let pollInFlight = false;
      timerId = window.setInterval(() => {
        if (disposed || pollInFlight) return;
        pollInFlight = true;
        const url = resolveEndpointUrl(config, "dashboard");
        requestJson<unknown>(url, { signal: controller.signal })
          .then((payload) => {
            handleIncomingPayload(payload);
          })
          .catch(() => {
            // Keep last valid telemetry frame during transient network hiccups
          })
          .finally(() => {
            pollInFlight = false;
          });
      }, config.feedIntervalMs);
    })
    .catch((err: unknown) => {
      if (disposed || controller.signal.aborted) return;
      callbacks.onError(
        err instanceof Error ? err.message : "Failed to connect to dashboard feed.",
      );
    });

  return () => {
    disposed = true;
    controller.abort();
    if (timerId !== undefined) {
      window.clearInterval(timerId);
    }
    ws?.close();
    eventSource?.close();
  };
}

export async function fetchDeviceData(
  config: DataSourceConfig,
  signal?: AbortSignal,
): Promise<DeviceData> {
  if (config.mode === "netpie") {
    return fetchNetpieDeviceData(config.netpie, signal);
  }
  const url = resolveEndpointUrl(config, "device");
  return requestJson<DeviceData>(url, { signal });
}

export async function updateDeviceConnection(
  config: DataSourceConfig,
  current: DeviceData,
  connected: boolean,
): Promise<DeviceData> {
  if (config.mode === "netpie") {
    return fetchNetpieDeviceData(config.netpie);
  }
  if (config.mode === "dev") {
    return {
      ...current,
      connected,
      connectionLabel: connected ? "Connected" : "Disconnected",
      lastUpdate: new Date().toLocaleTimeString("en-GB", {
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit",
      }),
    };
  }

  const url = resolveEndpointUrl(config, "device");
  return requestJson<DeviceData>(url, {
    method: "PATCH",
    body: JSON.stringify({ connected }),
  });
}

export async function fetchDatabaseData(
  config: DataSourceConfig,
  params?: { range?: Range; page?: number },
  signal?: AbortSignal,
): Promise<DatabaseData> {
  if (config.mode === "netpie") {
    return fetchNetpieDatabaseData(config.netpie, signal);
  }
  const url = resolveEndpointUrl(config, "database", params);
  return requestJson<DatabaseData>(url, { signal });
}

export async function updateDatabaseConnection(
  config: DataSourceConfig,
  current: DatabaseConnectionInfo,
  patch: Partial<Pick<DatabaseConnectionInfo, "address" | "secret">>,
): Promise<DatabaseConnectionInfo> {
  if (config.mode === "dev" || config.mode === "netpie") {
    return { ...current, ...patch };
  }

  const base = config.apiBaseUrl.replace(/\/+$/, "");
  return requestJson<DatabaseConnectionInfo>(`${base}/database/connection`, {
    method: "PATCH",
    body: JSON.stringify(patch),
  });
}
