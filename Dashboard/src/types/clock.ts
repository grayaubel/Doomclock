export type Page = "dashboard" | "device" | "database";
export type Range = "Hour" | "Day" | "Week" | "Month";
export type SensorKey = "Temperature" | "Humidity" | "Luminosity";
export type MetricKey = "Sleep Activity" | SensorKey;
export type DataMode = "netpie" | "dev" | "api";

export interface NetpieConfig {
  clientId: string;
  token: string;
  secret: string;
}

export interface DataSourceConfig {
  mode: DataMode;
  netpie: NetpieConfig;
  mockBasePath: string;
  apiBaseUrl: string;
  streamUrl?: string;
  feedEnabled: boolean;
  feedIntervalMs: number;
}

export interface ChartPoint {
  label: string;
  detailLabel?: string;
  value: number;
}

export interface SleepSummaryMetrics {
  sleepOnsetLatencyMinutes: number;
  totalSleepTime: string;
  wakeAfterSleepOnsetMinutes: number;
}

export interface MetricRangeDataset {
  headline: number;
  xLabel: string;
  xTicks: string[];
  points: ChartPoint[];
}

export interface SleepRangeDataset extends MetricRangeDataset {
  summary: SleepSummaryMetrics;
}

export interface AlarmStatusData {
  currentTime: string;
  alarmTime: string;
  lockDurationMinutes: number;
  lockStatus: string;
  connected: boolean;
  connectionLabel?: string;
}

export interface SleepActivityData {
  title: string;
  unit: string;
  ranges: Record<Range, SleepRangeDataset>;
}

export interface SensorMetricData {
  name: SensorKey;
  unit: string;
  ranges: Record<Range, MetricRangeDataset>;
}

export interface DashboardData {
  alarm: AlarmStatusData;
  sleepActivity: SleepActivityData;
  sensors: Record<SensorKey, SensorMetricData>;
}

export interface DashboardFeedTick {
  currentTime?: string;
  alarmConnected?: boolean;
  sleepActivity?: number;
  sensors?: Partial<Record<SensorKey, number>>;
}

export interface DeviceData {
  name: string;
  lastUpdate: string;
  connected: boolean;
  connectionLabel?: string;
}

export interface DatabaseRecord {
  id: string;
  timestamp: string;
  range: Range;
  temp: number;
  light: number;
  humid: number;
  prediction: number;
}

export interface DatabaseConnectionInfo {
  engine: string;
  address: string;
  secret: string;
  connected: boolean;
  connectionLabel?: string;
}

export interface DatabaseData {
  connection: DatabaseConnectionInfo;
  columns: string[];
  totalResults: number;
  pageSize: number;
  records: DatabaseRecord[];
}
