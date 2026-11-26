import { useEffect, useState } from "react";
import { resolveEndpointUrl, subscribeDashboardFeed } from "../api/clockApi";
import { DataErrorState } from "../components/DataErrorState";
import { PageHeader } from "../components/PageHeader";
import { AlarmCard } from "../components/dashboard/AlarmCard";
import { SensorCard } from "../components/dashboard/SensorCard";
import { SleepCard } from "../components/dashboard/SleepCard";
import type { DashboardData, DataSourceConfig, Range } from "../types/clock";

export function DashboardPage({
  dataSource,
  onOpenDataSource,
  onSwitchToDev,
}: {
  dataSource: DataSourceConfig;
  onOpenDataSource: () => void;
  onSwitchToDev: () => void;
}) {
  const [sleepRange, setSleepRange] = useState<Range>("Hour");
  const [sensorRanges, setSensorRanges] = useState<Record<string, Range>>({
    Temperature: "Hour",
    Humidity: "Hour",
    Luminosity: "Hour",
  });
  const [data, setData] = useState<DashboardData | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);

  useEffect(() => {
    setLoading(true);
    setError(null);

    const unsubscribe = subscribeDashboardFeed(dataSource, {
      onData: (payload) => {
        setData(payload);
        setLoading(false);
      },
      onError: (message) => {
        setError(message);
        setLoading(false);
      },
    });

    return unsubscribe;
  }, [dataSource, reloadToken]);

  return (
    <section>
      <PageHeader title="Dashboard" dataSource={dataSource} onOpenDataSource={onOpenDataSource} />
      {error ? (
        <DataErrorState
          error={error}
          endpoint={resolveEndpointUrl(dataSource, "dashboard")}
          dataSource={dataSource}
          onRetry={() => setReloadToken((n) => n + 1)}
          onSwitchToDev={onSwitchToDev}
        />
      ) : loading || !data ? (
        <div className="dashboard-grid" aria-busy="true">
          <div className="glass-card alarm-card skeleton-card" />
          <div className="glass-card sleep-card skeleton-card" />
          <div className="glass-card sensor-card skeleton-card" />
          <div className="glass-card sensor-card skeleton-card" />
          <div className="glass-card sensor-card skeleton-card" />
        </div>
      ) : (
        <div className="dashboard-grid">
          <AlarmCard alarm={data.alarm} />
          <SleepCard
            sleepActivity={data.sleepActivity}
            range={sleepRange}
            setRange={setSleepRange}
          />
          {Object.values(data.sensors).map((sensor) => {
            const activeRange = sensorRanges[sensor.name] ?? "Hour";
            return (
              <SensorCard
                key={sensor.name}
                sensor={sensor}
                range={activeRange}
                setRange={(nextRange) =>
                  setSensorRanges((old) => ({ ...old, [sensor.name]: nextRange }))
                }
              />
            );
          })}
        </div>
      )}
    </section>
  );
}
