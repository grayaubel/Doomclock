import type { Range, SleepActivityData } from "../../types/clock";
import { RangeSelector } from "../RangeSelector";
import { MetricAreaChart } from "./MetricAreaChart";
import { formatHeadline } from "./metricConfig";

export function SleepCard({
  sleepActivity,
  range,
  setRange,
}: {
  sleepActivity: SleepActivityData;
  range: Range;
  setRange: (range: Range) => void;
}) {
  const rangeData = sleepActivity.ranges[range];
  return (
    <article className="glass-card sleep-card">
      <div className="sleep-chart-card">
        <div className="card-heading">
          <h2>{sleepActivity.title}</h2>
          <div className="metric">
            {formatHeadline(rangeData.headline)}
            <sup>{sleepActivity.unit}</sup>
          </div>
        </div>
        <RangeSelector value={range} onChange={setRange} sleep />
        <div className="wide-chart">
          <MetricAreaChart metric="Sleep Activity" range={range} dataset={rangeData} />
        </div>
      </div>
      <div className="sleep-summary">
        <span className="summary-label">Summary</span>
        <dl>
          <div>
            <dt>Sleep Onset Latency</dt>
            <dd>
              <strong>{rangeData.summary.sleepOnsetLatencyMinutes}</strong> Minutes Average
            </dd>
          </div>
          <div>
            <dt>Total Sleep Time</dt>
            <dd>
              <strong>{rangeData.summary.totalSleepTime}</strong> Hours Average
            </dd>
          </div>
          <div>
            <dt>Wake After Sleep Onset</dt>
            <dd>
              <strong>{rangeData.summary.wakeAfterSleepOnsetMinutes}</strong> Minutes Average
            </dd>
          </div>
        </dl>
      </div>
    </article>
  );
}
