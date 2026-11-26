import { useEffect, useMemo, useRef } from "react";
import {
  Area,
  AreaChart,
  CartesianGrid,
  ReferenceLine,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";
import type { ChartPoint, MetricKey, MetricRangeDataset, Range } from "../../types/clock";
import { metricConfigs, type MetricChartConfig } from "./metricConfig";

export function MetricAreaChart({
  metric,
  range,
  dataset,
}: {
  metric: MetricKey;
  range: Range;
  dataset: MetricRangeDataset;
}) {
  const config = metricConfigs[metric];
  const { points: data, xTicks, xLabel } = dataset;
  const prevRangeRef = useRef<Range | null>(null);
  const shouldAnimate = prevRangeRef.current !== range;

  useEffect(() => {
    prevRangeRef.current = range;
  }, [range]);

  const stats = useMemo(() => {
    if (!data.length) {
      return { avg: 0, min: config.domain[0], max: config.domain[1] };
    }
    const values = data.map((point) => point.value);
    const sum = values.reduce((acc, current) => acc + current, 0);
    return {
      avg: sum / values.length,
      min: Math.min(...values),
      max: Math.max(...values),
    };
  }, [data, config.domain]);

  const yScale = useMemo(() => {
    if (stats.min >= config.domain[0] && stats.max <= config.domain[1]) {
      return { domain: config.domain, ticks: config.yTicks };
    }
    const step = config.unit === "LX" ? 100 : config.unit === "%" ? 10 : 2;
    const rawLow = Math.min(stats.min, config.domain[0]);
    const rawHigh = Math.max(stats.max * 1.08, config.domain[1]);
    const low = Math.floor(rawLow / step) * step;
    const high = Math.ceil(rawHigh / step) * step;
    const mid = Number(((low + high) / 2).toFixed(config.unit === "°C" ? 1 : 0));
    return {
      domain: [low, high] as [number, number],
      ticks: [low, mid, high],
    };
  }, [stats.min, stats.max, config.domain, config.yTicks, config.unit]);

  const gradientId = `chart-fill-${metric.toLowerCase().replace(/\s+/g, "-")}`;

  return (
    <div className="chart-plot">
      <ResponsiveContainer width="100%" height="100%">
        <AreaChart data={data} margin={{ top: 10, right: 20, bottom: 18, left: 8 }}>
          <defs>
            <linearGradient id={gradientId} x1="0" y1="0" x2="0" y2="1">
              <stop offset="0%" stopColor={config.fillStart} stopOpacity={0.58} />
              <stop offset="100%" stopColor={config.fillEnd} stopOpacity={0.12} />
            </linearGradient>
          </defs>
          <CartesianGrid
            vertical={false}
            stroke="rgba(255, 255, 255, 0.08)"
            strokeDasharray="3 3"
          />
          <XAxis
            dataKey="label"
            ticks={xTicks}
            interval={0}
            height={26}
            tickMargin={3}
            tick={{
              fill: "#d9d9d9",
              fontSize: 11,
              fontFamily: '"Chivo Mono:Regular", monospace',
            }}
            tickLine={{ stroke: "#777" }}
            axisLine={{ stroke: "#777" }}
            label={{
              value: xLabel,
              position: "insideBottom",
              offset: -12,
              style: {
                textAnchor: "middle",
                fill: "#b5aaa4",
                fontSize: 11,
                fontFamily: '"Google Sans Flex:Regular", Arial, sans-serif',
              },
            }}
          />
          <YAxis
            domain={yScale.domain}
            ticks={yScale.ticks}
            width={48}
            tickMargin={3}
            tickFormatter={(tick) => config.formatTick(Number(tick))}
            tick={{
              fill: "#d9d9d9",
              fontSize: 11,
              fontFamily: '"Chivo Mono:Regular", monospace',
            }}
            tickLine={{ stroke: "#777" }}
            axisLine={{ stroke: "#777" }}
            label={{
              value: config.yLabel,
              angle: -90,
              position: "insideLeft",
              offset: -2,
              style: {
                textAnchor: "middle",
                fill: "#b5aaa4",
                fontSize: 11,
                fontFamily: '"Google Sans Flex:Regular", Arial, sans-serif',
              },
            }}
          />
          <ReferenceLine
            y={stats.avg}
            stroke={config.stroke}
            strokeOpacity={0.38}
            strokeDasharray="4 4"
          />
          <Tooltip
            cursor={{
              stroke: "rgba(255, 255, 255, 0.28)",
              strokeWidth: 1,
              strokeDasharray: "3 3",
            }}
            allowEscapeViewBox={{ x: false, y: false }}
            wrapperStyle={{ zIndex: 20, pointerEvents: "none" }}
            content={<ChartTooltip metric={metric} config={config} average={stats.avg} />}
          />
          <Area
            type="monotone"
            dataKey="value"
            stroke={config.stroke}
            strokeWidth={3}
            fill={`url(#${gradientId})`}
            dot={
              range === "Week"
                ? { r: 3, fill: config.stroke, stroke: "#111", strokeWidth: 1.5 }
                : false
            }
            activeDot={{
              r: 5,
              fill: config.stroke,
              stroke: "#ffffff",
              strokeWidth: 2,
            }}
            isAnimationActive={shouldAnimate}
            animationDuration={450}
            animationEasing="ease-out"
          />
        </AreaChart>
      </ResponsiveContainer>
    </div>
  );
}

function ChartTooltip({
  active,
  payload,
  metric,
  config,
  average,
}: {
  active?: boolean;
  payload?: Array<{ value?: number | string; payload?: ChartPoint }>;
  metric: MetricKey;
  config: MetricChartConfig;
  average: number;
}) {
  if (!active || !payload?.length) return null;
  const rawValue = Number(payload[0].value);
  const point = payload[0].payload;
  const formattedValue = config.formatValue(rawValue);
  const formattedAvg = config.formatValue(average);
  const diff = rawValue - average;
  const formattedDiff = `${diff >= 0 ? "+" : ""}${
    config.unit === "°C" ? diff.toFixed(1) : Math.round(diff)
  }${config.unit === "LX" ? " LX" : config.unit}`;
  const unitText = config.unit === "LX" ? " LX" : config.unit;
  const avgUnitText = config.unit === "LX" ? " LX" : config.unit;

  return (
    <div className="chart-tooltip">
      {point?.detailLabel && <div className="chart-tooltip-time">{point.detailLabel}</div>}
      <div className="chart-tooltip-main">
        <span className="chart-tooltip-dot" style={{ backgroundColor: config.stroke }} />
        <span className="chart-tooltip-name">{metric}</span>
        <strong>
          {formattedValue}
          {unitText}
        </strong>
      </div>
      <div className="chart-tooltip-meta">
        {formattedDiff} vs avg ({formattedAvg}
        {avgUnitText})
      </div>
    </div>
  );
}
