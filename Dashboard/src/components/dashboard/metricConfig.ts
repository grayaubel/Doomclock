import type { MetricKey } from "../../types/clock";

export interface MetricChartConfig {
  stroke: string;
  fillStart: string;
  fillEnd: string;
  unit: string;
  yLabel: string;
  domain: [number, number];
  yTicks: number[];
  formatTick: (tick: number) => string;
  formatValue: (value: number) => string;
}

export const metricConfigs: Record<MetricKey, MetricChartConfig> = {
  "Sleep Activity": {
    stroke: "#4c47d9",
    fillStart: "#4844c9",
    fillEnd: "#242173",
    unit: "%",
    yLabel: "Activity",
    domain: [0, 100],
    yTicks: [0, 50, 100],
    formatTick: (tick) => `${tick}%`,
    formatValue: (value) => `${Math.round(value)}`,
  },
  Temperature: {
    stroke: "#3eceee",
    fillStart: "#3eceee",
    fillEnd: "#174b58",
    unit: "°C",
    yLabel: "Temp (°C)",
    domain: [21, 25],
    yTicks: [21, 23, 25],
    formatTick: (tick) => `${tick}°`,
    formatValue: (value) => value.toFixed(1),
  },
  Humidity: {
    stroke: "#6b73cd",
    fillStart: "#6b73cd",
    fillEnd: "#2b2f5c",
    unit: "%",
    yLabel: "Humidity",
    domain: [50, 90],
    yTicks: [50, 70, 90],
    formatTick: (tick) => `${tick}%`,
    formatValue: (value) => `${Math.round(value)}`,
  },
  Luminosity: {
    stroke: "#cdb66b",
    fillStart: "#cdb66b",
    fillEnd: "#4d4323",
    unit: "LX",
    yLabel: "Light (LX)",
    domain: [0, 400],
    yTicks: [0, 200, 400],
    formatTick: (tick) => `${tick}`,
    formatValue: (value) => `${Math.round(value)}`,
  },
};

export function formatHeadline(value: number): string {
  return `${Math.round(Number(value))}`;
}
