import type { Range, SensorMetricData } from "../../types/clock";
import { RangeSelector } from "../RangeSelector";
import { MetricAreaChart } from "./MetricAreaChart";
import { formatHeadline } from "./metricConfig";

export function SensorCard({
  sensor,
  range,
  setRange,
}: {
  sensor: SensorMetricData;
  range: Range;
  setRange: (range: Range) => void;
}) {
  const rangeData = sensor.ranges[range];
  return (
    <article className="glass-card sensor-card">
      <div className="card-heading">
        <h2>{sensor.name}</h2>
        <div className="metric">
          {formatHeadline(rangeData.headline)}
          <sup>{sensor.unit}</sup>
        </div>
      </div>
      <RangeSelector value={range} onChange={setRange} />
      <div className="sensor-chart">
        <MetricAreaChart metric={sensor.name} range={range} dataset={rangeData} />
      </div>
    </article>
  );
}
