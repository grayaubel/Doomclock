import type {
  ChartPoint,
  DashboardData,
  DatabaseData,
  DatabaseRecord,
  DeviceData,
  MetricRangeDataset,
  NetpieConfig,
  Range,
  SensorKey,
  SleepRangeDataset,
} from "../types/clock";
import { addMinutesToTime } from "./mockFeed";

const NETPIE_API_BASE = "https://api.netpie.io/v2/device";
const HISTORY_STORAGE_PREFIX = "clock.netpieTelemetry.v1.";
const MAX_LIVE_POINTS = 36;
const MAX_LOG_RECORDS = 120;
const WEEK_DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

export const DEFAULT_NETPIE_CONFIG: NetpieConfig = {
  clientId:
    (import.meta.env.VITE_NETPIE_CLIENT_ID as string | undefined)?.trim() ||
    "39c1b827-259e-4b75-8751-8100ec25771c",
  token:
    (import.meta.env.VITE_NETPIE_TOKEN as string | undefined)?.trim() ||
    "wqVCPnhR4ekZnQUCNDhA7ch9XET4arj8",
  secret:
    (import.meta.env.VITE_NETPIE_SECRET as string | undefined)?.trim() ||
    "sRqFBCyivkr5VsLgiWpHc3nNRrUmmsbG",
};

interface NetpieShadowData {
  action?: string;
  time_local?: string;
  phoneInserted?: boolean | number;
  bed_fresh?: boolean | number;
  link_rx?: number;
  link_lost?: number;
  link_dup?: number;
  link_pdr?: number;
  tempCelsius?: number;
  humidityPercent?: number;
  lightIntensity?: number;
  bed_imu_n?: number;
  bed_move_count?: number;
  bed_age_s?: number;
  bed_jerk_mg?: number;
  bed_tof_mm?: number;
  bed_tof_std_mm?: number;
  up_min?: number;
  wake_rel_min?: number;
  alarm_time?: string;
  wake_time?: string;
  lock_duration_min?: number;
  state?: string;
  ai?: string;
  ai_src?: string;
  ai_conf?: number;
  label?: string;
  note?: string;
}

interface NetpieShadowEnvelope {
  deviceid: string;
  data?: NetpieShadowData;
  rev?: number;
  timestamp?: number;
  modified?: number;
}

interface NetpieDeviceStatusEnvelope {
  deviceid: string;
  alias?: string;
  projectid?: string;
  status?: number;
  enabled?: boolean;
}

interface StoredTelemetrySample {
  id: string;
  rev: number;
  epochMs: number;
  timeLabel: string;
  hourLabel: string;
  dayOfWeek: string;
  dayOfMonth: string;
  fullTimestamp: string;
  clockHHMM: string;
  sleepActivity: number;
  tempCelsius: number;
  humidityPercent: number;
  lightIntensity: number;
  prediction: number;
  aiState: string;
  upMin: number;
  wakeRelMin: number;
}

interface StoredNetpieHistory {
  liveSamples: StoredTelemetrySample[];
  revLogs: StoredTelemetrySample[];
}

function clamp(val: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, val));
}

function authHeader(netpie: NetpieConfig): string {
  return `Device ${netpie.clientId.trim()}:${netpie.token.trim()}`;
}

async function netpieGet<T>(path: string, netpie: NetpieConfig, signal?: AbortSignal): Promise<T> {
  const response = await fetch(`${NETPIE_API_BASE}${path}`, {
    headers: {
      Accept: "application/json",
      Authorization: authHeader(netpie),
    },
    cache: "no-store",
    signal,
  });
  if (!response.ok) {
    throw new Error(`NETPIE HTTP ${response.status} on ${path}`);
  }
  return (await response.json()) as T;
}

function parseLocalTimeParts(timeLocal?: string, fallbackMs?: number) {
  if (timeLocal) {
    const isoMatch = timeLocal.match(/^(\d{4})-(\d{2})-(\d{2})[T\s](\d{2}):(\d{2}):(\d{2})/);
    if (isoMatch) {
      const [, yyyy, mm, dd, hh, min, ss] = isoMatch;
      const parsedDate = new Date(timeLocal);
      const dayIdx = Number.isNaN(parsedDate.getTime()) ? new Date().getDay() : parsedDate.getDay();
      return {
        clockHHMM: `${hh}:${min}`,
        hms: `${hh}:${min}:${ss}`,
        hourLabel: `${hh}:00`,
        dayOfWeek: WEEK_DAYS[dayIdx] ?? "Thu",
        dayOfMonth: String(Number(dd)),
        fullTimestamp: `${dd}/${mm}/${yyyy} ${hh}:${min}:${ss}`,
      };
    }

    const timeOnly = timeLocal.match(/(\d{2}):(\d{2})(?::(\d{2}))?/);
    if (timeOnly) {
      const [, hh, min, ss = "00"] = timeOnly;
      const now = new Date();
      const dd = String(now.getDate()).padStart(2, "0");
      const mm = String(now.getMonth() + 1).padStart(2, "0");
      const yyyy = now.getFullYear();
      return {
        clockHHMM: `${hh}:${min}`,
        hms: `${hh}:${min}:${ss}`,
        hourLabel: `${hh}:00`,
        dayOfWeek: WEEK_DAYS[now.getDay()],
        dayOfMonth: String(now.getDate()),
        fullTimestamp: `${dd}/${mm}/${yyyy} ${hh}:${min}:${ss}`,
      };
    }
  }

  const d = new Date(fallbackMs ?? Date.now());
  const hh = String(d.getHours()).padStart(2, "0");
  const min = String(d.getMinutes()).padStart(2, "0");
  const ss = String(d.getSeconds()).padStart(2, "0");
  const dd = String(d.getDate()).padStart(2, "0");
  const mm = String(d.getMonth() + 1).padStart(2, "0");
  const yyyy = d.getFullYear();
  return {
    clockHHMM: `${hh}:${min}`,
    hms: `${hh}:${min}:${ss}`,
    hourLabel: `${hh}:00`,
    dayOfWeek: WEEK_DAYS[d.getDay()],
    dayOfMonth: String(d.getDate()),
    fullTimestamp: `${dd}/${mm}/${yyyy} ${hh}:${min}:${ss}`,
  };
}

function computeSleepActivityPercent(data: NetpieShadowData): number {
  if (
    typeof data.bed_move_count === "number" &&
    typeof data.bed_imu_n === "number" &&
    data.bed_imu_n > 0
  ) {
    return clamp(Math.round((data.bed_move_count / data.bed_imu_n) * 100), 0, 100);
  }
  if (typeof data.bed_move_count === "number") {
    return clamp(Math.round((data.bed_move_count / 250) * 100), 0, 100);
  }
  if (typeof data.bed_jerk_mg === "number") {
    return clamp(Math.round(data.bed_jerk_mg / 10), 0, 100);
  }
  if (typeof data.ai_conf === "number") {
    return clamp(Math.round(data.ai_conf * 100), 0, 100);
  }
  return 0;
}

function loadHistory(clientId: string): StoredNetpieHistory {
  try {
    const raw = window.localStorage.getItem(`${HISTORY_STORAGE_PREFIX}${clientId}`);
    if (!raw) return { liveSamples: [], revLogs: [] };
    const parsed = JSON.parse(raw) as Partial<StoredNetpieHistory>;
    return {
      liveSamples: Array.isArray(parsed.liveSamples) ? parsed.liveSamples : [],
      revLogs: Array.isArray(parsed.revLogs) ? parsed.revLogs : [],
    };
  } catch {
    return { liveSamples: [], revLogs: [] };
  }
}

function saveHistory(clientId: string, history: StoredNetpieHistory): void {
  try {
    window.localStorage.setItem(`${HISTORY_STORAGE_PREFIX}${clientId}`, JSON.stringify(history));
  } catch {
    // Ignore storage quota errors
  }
}

function pickTicks(points: ChartPoint[], maxTicks: number): string[] {
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

function ensureMinimumTwoPoints(
  points: ChartPoint[],
  fallbackPrevLabel: string,
  detailPrefix: string,
): ChartPoint[] {
  if (points.length >= 2) return points;
  if (points.length === 1) {
    const only = points[0];
    const prevLabel = only.label === fallbackPrevLabel ? `prev` : fallbackPrevLabel;
    return [
      {
        label: prevLabel,
        detailLabel: `${detailPrefix} · ${prevLabel}`,
        value: only.value,
      },
      only,
    ];
  }
  return [
    { label: fallbackPrevLabel, detailLabel: fallbackPrevLabel, value: 0 },
    { label: "now", detailLabel: "Now", value: 0 },
  ];
}

function buildAggregatedSeries(
  samples: StoredTelemetrySample[],
  selector: (s: StoredTelemetrySample) => number,
  keyFn: (s: StoredTelemetrySample) => string,
  detailPrefix: string,
  fallbackPrevLabel: string,
  xLabel: string,
  maxTicks: number,
  decimals: number,
): MetricRangeDataset {
  const buckets = new Map<string, { sum: number; count: number }>();
  for (const s of samples) {
    const k = keyFn(s);
    const prev = buckets.get(k) ?? { sum: 0, count: 0 };
    buckets.set(k, { sum: prev.sum + selector(s), count: prev.count + 1 });
  }

  const rawPoints: ChartPoint[] = Array.from(buckets.entries()).map(([label, agg]) => {
    const factor = 10 ** decimals;
    const avg = Math.round((agg.sum / agg.count) * factor) / factor;
    return {
      label,
      detailLabel: `${detailPrefix} · ${label}`,
      value: avg,
    };
  });

  const latestSampleValue = samples.length ? selector(samples[samples.length - 1]) : 0;
  const points = ensureMinimumTwoPoints(rawPoints, fallbackPrevLabel, detailPrefix);

  return {
    headline: latestSampleValue,
    xLabel,
    xTicks: pickTicks(points, maxTicks),
    points,
  };
}

function recordShadowSample(
  netpie: NetpieConfig,
  envelope: NetpieShadowEnvelope,
  appendStreamTick: boolean,
): StoredNetpieHistory {
  const data = envelope.data ?? {};
  const rev = Number(envelope.rev ?? 0);
  const shadowParts = parseLocalTimeParts(data.time_local, envelope.modified ?? envelope.timestamp);
  const nowParts = parseLocalTimeParts(undefined, Date.now());

  const sleepActivity = computeSleepActivityPercent(data);
  const tempCelsius = Number((data.tempCelsius ?? 0).toFixed(1));
  const humidityPercent = Math.round(Number(data.humidityPercent ?? 0));
  const lightIntensity = Math.round(Number(data.lightIntensity ?? 0));
  const prediction =
    typeof data.ai_conf === "number"
      ? clamp(Math.round(data.ai_conf * 100), 0, 100)
      : sleepActivity;
  const aiState = data.ai || data.state || "IDLE";

  const history = loadHistory(netpie.clientId);
  const lastLive = history.liveSamples[history.liveSamples.length - 1];
  const isNewRev = !history.revLogs.some((item) => item.rev === rev);

  // Use shadow timestamp when a brand-new hardware revision arrives; otherwise stamp stream poll time
  const chosenTimeLabel =
    isNewRev && lastLive?.timeLabel !== shadowParts.hms ? shadowParts.hms : nowParts.hms;

  const sample: StoredTelemetrySample = {
    id: `np-${rev}-${Date.now()}`,
    rev,
    epochMs: Date.now(),
    timeLabel: chosenTimeLabel,
    hourLabel: shadowParts.hourLabel,
    dayOfWeek: shadowParts.dayOfWeek,
    dayOfMonth: shadowParts.dayOfMonth,
    fullTimestamp: isNewRev ? shadowParts.fullTimestamp : nowParts.fullTimestamp,
    clockHHMM: shadowParts.clockHHMM,
    sleepActivity,
    tempCelsius,
    humidityPercent,
    lightIntensity,
    prediction,
    aiState,
    upMin: Number(data.up_min ?? 0),
    wakeRelMin: Number(data.wake_rel_min ?? 0),
  };

  let nextLive = history.liveSamples;
  if (!lastLive) {
    // Seed a two-point baseline at the real hardware reading so initial chart renders immediately
    const seedPrevLabel = addMinutesToTime(shadowParts.clockHHMM, -1) + ":00";
    nextLive = [{ ...sample, id: `${sample.id}-seed`, timeLabel: seedPrevLabel }, sample];
  } else if ((appendStreamTick || isNewRev) && lastLive.timeLabel !== sample.timeLabel) {
    nextLive = [...history.liveSamples, sample].slice(-MAX_LIVE_POINTS);
  }

  let nextRevLogs = history.revLogs;
  if (isNewRev || nextRevLogs.length === 0) {
    nextRevLogs = [sample, ...history.revLogs].slice(0, MAX_LOG_RECORDS);
  }

  const updated: StoredNetpieHistory = {
    liveSamples: nextLive,
    revLogs: nextRevLogs,
  };
  saveHistory(netpie.clientId, updated);
  return updated;
}

function buildDashboardFromNetpie(
  envelope: NetpieShadowEnvelope,
  statusEnvelope: NetpieDeviceStatusEnvelope | null,
  history: StoredNetpieHistory,
): DashboardData {
  const d = envelope.data ?? {};
  const samples = history.liveSamples;
  const latest = samples[samples.length - 1];
  const timeParts = parseLocalTimeParts(d.time_local, envelope.modified ?? envelope.timestamp);

  let alarmDisplay = d.alarm_time || d.wake_time || "";
  if (!alarmDisplay && typeof d.wake_rel_min === "number" && Number.isFinite(d.wake_rel_min)) {
    alarmDisplay = addMinutesToTime(timeParts.clockHHMM, -Math.round(d.wake_rel_min));
  }
  if (!alarmDisplay) {
    alarmDisplay = d.state || "--:--";
  }

  const isMqttOnline = statusEnvelope?.status === 1;
  const bedFresh = Boolean(d.bed_fresh);
  const connected = isMqttOnline || bedFresh;
  const connectionLabel = isMqttOnline
    ? "NETPIE Online"
    : bedFresh
      ? `Shadow #${envelope.rev ?? 0}`
      : "Offline";

  const aiTag = d.ai
    ? `${d.ai}${typeof d.ai_conf === "number" ? ` (${Math.round(d.ai_conf * 100)}%)` : ""}`
    : `Rev #${envelope.rev ?? 0}`;

  function makeLiveHourDataset(
    selector: (s: StoredTelemetrySample) => number,
    maxTicks: number,
  ): MetricRangeDataset {
    const rawPoints: ChartPoint[] = samples.map((s) => ({
      label: s.timeLabel,
      detailLabel: `NETPIE · ${s.timeLabel} · ${aiTag}`,
      value: selector(s),
    }));
    const points = ensureMinimumTwoPoints(
      rawPoints,
      addMinutesToTime(timeParts.clockHHMM, -1) + ":00",
      "NETPIE",
    );
    return {
      headline: latest ? selector(latest) : 0,
      xLabel: "Time",
      xTicks: pickTicks(points, maxTicks),
      points,
    };
  }

  const sleepSamplesCount = history.revLogs.filter(
    (s) => /sleep/i.test(s.aiState) && !/awake/i.test(s.aiState),
  ).length;
  const sleepHours = Math.floor((sleepSamplesCount * 5) / 60);
  const sleepMins = String((sleepSamplesCount * 5) % 60).padStart(2, "0");
  const summary = {
    sleepOnsetLatencyMinutes: Math.round(Number(d.up_min ?? 0)),
    totalSleepTime: `${sleepHours}:${sleepMins}`,
    wakeAfterSleepOnsetMinutes: Math.abs(Math.round(Number(d.wake_rel_min ?? d.bed_age_s ?? 0))),
  };

  const prevHourFallback = addMinutesToTime(timeParts.clockHHMM, -60).slice(0, 2) + ":00";
  const prevWeekFallback = WEEK_DAYS[(WEEK_DAYS.indexOf(timeParts.dayOfWeek) + 6) % 7] ?? "Wed";
  const prevMonthFallback = String(Math.max(1, Number(timeParts.dayOfMonth) - 1));

  const sleepRanges: Record<Range, SleepRangeDataset> = {
    Hour: {
      ...makeLiveHourDataset((s) => s.sleepActivity, 5),
      summary,
    },
    Day: {
      ...buildAggregatedSeries(
        samples,
        (s) => s.sleepActivity,
        (s) => s.hourLabel,
        "Hourly Avg",
        prevHourFallback,
        "Time",
        6,
        0,
      ),
      summary,
    },
    Week: {
      ...buildAggregatedSeries(
        samples,
        (s) => s.sleepActivity,
        (s) => s.dayOfWeek,
        "Daily Avg",
        prevWeekFallback,
        "Day",
        7,
        0,
      ),
      summary,
    },
    Month: {
      ...buildAggregatedSeries(
        samples,
        (s) => s.sleepActivity,
        (s) => s.dayOfMonth,
        "Day of Month",
        prevMonthFallback,
        "Date",
        6,
        0,
      ),
      summary,
    },
  };

  function buildSensorRanges(
    selector: (s: StoredTelemetrySample) => number,
    decimals: number,
  ): Record<Range, MetricRangeDataset> {
    return {
      Hour: makeLiveHourDataset(selector, 3),
      Day: buildAggregatedSeries(
        samples,
        selector,
        (s) => s.hourLabel,
        "Hourly Avg",
        prevHourFallback,
        "Time",
        4,
        decimals,
      ),
      Week: buildAggregatedSeries(
        samples,
        selector,
        (s) => s.dayOfWeek,
        "Daily Avg",
        prevWeekFallback,
        "Day",
        7,
        decimals,
      ),
      Month: buildAggregatedSeries(
        samples,
        selector,
        (s) => s.dayOfMonth,
        "Day of Month",
        prevMonthFallback,
        "Date",
        5,
        decimals,
      ),
    };
  }

  const sensors: Record<SensorKey, DashboardData["sensors"][SensorKey]> = {
    Temperature: {
      name: "Temperature",
      unit: "°C",
      ranges: buildSensorRanges((s) => s.tempCelsius, 1),
    },
    Humidity: {
      name: "Humidity",
      unit: "%",
      ranges: buildSensorRanges((s) => s.humidityPercent, 0),
    },
    Luminosity: {
      name: "Luminosity",
      unit: "LX",
      ranges: buildSensorRanges((s) => s.lightIntensity, 0),
    },
  };

  return {
    alarm: {
      currentTime: timeParts.clockHHMM,
      alarmTime: alarmDisplay,
      lockDurationMinutes: Number(d.lock_duration_min ?? d.up_min ?? 30),
      lockStatus: d.phoneInserted ? "Locked" : "Unlocked",
      connected,
      connectionLabel,
    },
    sleepActivity: {
      title: "Sleep Activity",
      unit: "%",
      ranges: sleepRanges,
    },
    sensors,
  };
}

export async function fetchNetpieSnapshot(
  netpie: NetpieConfig,
  appendStreamTick: boolean,
  signal?: AbortSignal,
): Promise<DashboardData> {
  const [shadowEnvelope, statusEnvelope] = await Promise.all([
    netpieGet<NetpieShadowEnvelope>("/shadow/data", netpie, signal),
    netpieGet<NetpieDeviceStatusEnvelope>("/status", netpie, signal).catch(() => null),
  ]);

  const history = recordShadowSample(netpie, shadowEnvelope, appendStreamTick);
  return buildDashboardFromNetpie(shadowEnvelope, statusEnvelope, history);
}

export async function fetchNetpieDeviceData(
  netpie: NetpieConfig,
  signal?: AbortSignal,
): Promise<DeviceData> {
  const [shadowEnvelope, statusEnvelope] = await Promise.all([
    netpieGet<NetpieShadowEnvelope>("/shadow/data", netpie, signal),
    netpieGet<NetpieDeviceStatusEnvelope>("/status", netpie, signal).catch(() => null),
  ]);
  const d = shadowEnvelope.data ?? {};
  const timeParts = parseLocalTimeParts(
    d.time_local,
    shadowEnvelope.modified ?? shadowEnvelope.timestamp,
  );
  const isOnline = statusEnvelope?.status === 1;
  const pdr = d.link_pdr !== undefined ? ` · PDR ${d.link_pdr}%` : "";

  return {
    name: statusEnvelope?.alias ? `Gateway (${statusEnvelope.alias})` : "Gateway",
    lastUpdate: timeParts.hms,
    connected: isOnline || Boolean(d.bed_fresh),
    connectionLabel: isOnline ? `NETPIE Online${pdr}` : `Shadow #${shadowEnvelope.rev ?? 0}${pdr}`,
  };
}

export async function fetchNetpieDatabaseData(
  netpie: NetpieConfig,
  signal?: AbortSignal,
): Promise<DatabaseData> {
  const [shadowEnvelope, statusEnvelope] = await Promise.all([
    netpieGet<NetpieShadowEnvelope>("/shadow/data", netpie, signal),
    netpieGet<NetpieDeviceStatusEnvelope>("/status", netpie, signal).catch(() => null),
  ]);
  const history = recordShadowSample(netpie, shadowEnvelope, true);
  const sourceSamples =
    history.revLogs.length >= history.liveSamples.length
      ? history.revLogs
      : [...history.liveSamples].reverse();

  const records: DatabaseRecord[] = sourceSamples.map((s) => ({
    id: s.id,
    timestamp: s.fullTimestamp,
    range: "Hour",
    temp: s.tempCelsius,
    light: s.lightIntensity,
    humid: s.humidityPercent,
    prediction: s.prediction,
  }));

  const isOnline = statusEnvelope?.status === 1;
  return {
    connection: {
      engine: "NETPIE 2020",
      address: netpie.clientId,
      secret: netpie.token,
      connected: isOnline || Boolean(shadowEnvelope.data?.bed_fresh),
      connectionLabel: isOnline ? "NETPIE Connected" : `Shadow #${shadowEnvelope.rev ?? 0}`,
    },
    columns: ["Time Stamp", "Temp", "Light", "Humid", "Prediction"],
    totalResults: Number(shadowEnvelope.rev ?? records.length),
    pageSize: 10,
    records,
  };
}
