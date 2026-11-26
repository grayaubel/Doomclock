import type {
  ChartPoint,
  DashboardData,
  DashboardFeedTick,
  MetricRangeDataset,
  SensorKey,
  SleepRangeDataset,
} from "../types/clock";

const SENSOR_KEYS: SensorKey[] = ["Temperature", "Humidity", "Luminosity"];

const SENSOR_BOUNDS: Record<
  SensorKey,
  { min: number; max: number; step: number; decimals: number }
> = {
  Temperature: { min: 21.4, max: 24.6, step: 0.22, decimals: 1 },
  Humidity: { min: 53, max: 87, step: 2.8, decimals: 0 },
  Luminosity: { min: 45, max: 375, step: 18, decimals: 0 },
};

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

function roundTo(value: number, decimals: number): number {
  const factor = 10 ** decimals;
  return Math.round(value * factor) / factor;
}

export function addMinutesToTime(hhmm: string, deltaMinutes: number): string {
  const [rawH, rawM] = hhmm.split(":").map(Number);
  const baseMinutes = Number.isFinite(rawH) && Number.isFinite(rawM) ? rawH * 60 + rawM : 0;
  const nextTotal = (((baseMinutes + deltaMinutes) % 1440) + 1440) % 1440;
  const hh = String(Math.floor(nextTotal / 60)).padStart(2, "0");
  const mm = String(nextTotal % 60).padStart(2, "0");
  return `${hh}:${mm}`;
}

function computeEvenXTicks(points: ChartPoint[], tickCount: number): string[] {
  if (points.length <= tickCount) {
    return points.map((pt) => pt.label);
  }
  const ticks: string[] = [];
  const steps = tickCount - 1;
  for (let i = 0; i <= steps; i += 1) {
    const index = Math.round((i / steps) * (points.length - 1));
    const label = points[index]?.label;
    if (label && !ticks.includes(label)) {
      ticks.push(label);
    }
  }
  return ticks;
}

function rollTimeWindow(
  dataset: MetricRangeDataset,
  nextValue: number,
  stepMinutes: number,
  detailPrefix: string,
  tickCount: number,
): MetricRangeDataset {
  const points = dataset.points;
  if (!points.length) return dataset;

  const lastLabel = points[points.length - 1].label;
  const nextLabel = addMinutesToTime(lastLabel, stepMinutes);
  const nextPoint: ChartPoint = {
    label: nextLabel,
    detailLabel: `${detailPrefix} · ${nextLabel}`,
    value: nextValue,
  };
  const nextPoints = [...points.slice(1), nextPoint];

  return {
    ...dataset,
    headline: nextValue,
    xTicks: computeEvenXTicks(nextPoints, tickCount),
    points: nextPoints,
  };
}

function blendLatestBucket(
  dataset: MetricRangeDataset,
  incomingValue: number,
  decimals: number,
): MetricRangeDataset {
  if (!dataset.points.length) return dataset;
  const lastIndex = dataset.points.length - 1;
  const lastPoint = dataset.points[lastIndex];
  const blended = roundTo(lastPoint.value * 0.72 + incomingValue * 0.28, decimals);
  const nextPoints = dataset.points.map((pt, idx) =>
    idx === lastIndex ? { ...pt, value: blended } : pt,
  );
  return {
    ...dataset,
    headline: blended,
    points: nextPoints,
  };
}

function meanOf(points: ChartPoint[], fallback: number): number {
  if (!points.length) return fallback;
  const total = points.reduce((sum, pt) => sum + pt.value, 0);
  return total / points.length;
}

export function applyExternalFeedTick(
  current: DashboardData,
  tick: DashboardFeedTick,
): DashboardData {
  const nextSleepRanges = { ...current.sleepActivity.ranges };
  if (tick.sleepActivity !== undefined) {
    const sleepVal = clamp(Math.round(tick.sleepActivity), 0, 100);
    const updatedHour = rollTimeWindow(
      current.sleepActivity.ranges.Hour,
      sleepVal,
      15,
      "12h Window",
      5,
    );
    nextSleepRanges.Hour = {
      ...updatedHour,
      summary: current.sleepActivity.ranges.Hour.summary,
    };
    nextSleepRanges.Day = {
      ...blendLatestBucket(current.sleepActivity.ranges.Day, sleepVal, 0),
      summary: current.sleepActivity.ranges.Day.summary,
    };
    nextSleepRanges.Week = {
      ...blendLatestBucket(current.sleepActivity.ranges.Week, sleepVal, 0),
      summary: current.sleepActivity.ranges.Week.summary,
    };
    nextSleepRanges.Month = {
      ...blendLatestBucket(current.sleepActivity.ranges.Month, sleepVal, 0),
      summary: current.sleepActivity.ranges.Month.summary,
    };
  }

  const nextSensors = { ...current.sensors };
  let latestFeedTimestamp: string | undefined;

  for (const key of SENSOR_KEYS) {
    const rawIncoming = tick.sensors?.[key];
    if (rawIncoming === undefined) continue;
    const { decimals } = SENSOR_BOUNDS[key];
    const incoming = roundTo(Number(rawIncoming), decimals);
    const sensor = current.sensors[key];
    const nextHour = rollTimeWindow(sensor.ranges.Hour, incoming, 5, "Live Feed", 4);
    latestFeedTimestamp = nextHour.points[nextHour.points.length - 1]?.label;

    nextSensors[key] = {
      ...sensor,
      ranges: {
        Hour: nextHour,
        Day: blendLatestBucket(sensor.ranges.Day, incoming, decimals),
        Week: blendLatestBucket(sensor.ranges.Week, incoming, decimals),
        Month: blendLatestBucket(sensor.ranges.Month, incoming, decimals),
      },
    };
  }

  return {
    alarm: {
      ...current.alarm,
      currentTime: tick.currentTime ?? latestFeedTimestamp ?? current.alarm.currentTime,
      connected: tick.alarmConnected ?? current.alarm.connected,
    },
    sleepActivity: {
      ...current.sleepActivity,
      ranges: nextSleepRanges,
    },
    sensors: nextSensors,
  };
}

export function generateNextMockTick(current: DashboardData, stepIndex: number): DashboardData {
  const sleepPoints = current.sleepActivity.ranges.Hour.points;
  const prevSleep = sleepPoints[sleepPoints.length - 1]?.value ?? 15;
  const sleepTarget = 42 + Math.sin(stepIndex * 0.34) * 34 + Math.cos(stepIndex * 0.17) * 12;
  const sleepNoise = (Math.random() - 0.5) * 9;
  const nextSleep = clamp(Math.round(prevSleep * 0.62 + sleepTarget * 0.38 + sleepNoise), 4, 94);

  const sensorTick: Partial<Record<SensorKey, number>> = {};
  for (const key of SENSOR_KEYS) {
    const { min, max, step, decimals } = SENSOR_BOUNDS[key];
    const hourPoints = current.sensors[key].ranges.Hour.points;
    const prevVal = hourPoints[hourPoints.length - 1]?.value ?? (min + max) / 2;
    const anchor = meanOf(hourPoints, (min + max) / 2);
    const wave = Math.sin(stepIndex * 0.45 + SENSOR_KEYS.indexOf(key) * 1.3) * step * 0.65;
    const pull = (anchor - prevVal) * 0.22;
    const jitter = (Math.random() - 0.5) * step * 1.4;
    sensorTick[key] = roundTo(clamp(prevVal + pull + wave + jitter, min, max), decimals);
  }

  const updated = applyExternalFeedTick(current, {
    sleepActivity: nextSleep,
    sensors: sensorTick,
  });

  // Keep Sleep Summary subtly responsive to the live sleep window
  const prevSummary = updated.sleepActivity.ranges.Hour.summary;
  const nextHourSleep: SleepRangeDataset = {
    ...updated.sleepActivity.ranges.Hour,
    summary: {
      ...prevSummary,
      wakeAfterSleepOnsetMinutes: clamp(
        Math.round(prevSummary.wakeAfterSleepOnsetMinutes + (nextSleep < 20 ? 0.4 : -0.2)),
        8,
        28,
      ),
    },
  };

  return {
    ...updated,
    sleepActivity: {
      ...updated.sleepActivity,
      ranges: {
        ...updated.sleepActivity.ranges,
        Hour: nextHourSleep,
      },
    },
  };
}
