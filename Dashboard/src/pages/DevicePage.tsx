import { useEffect, useState } from "react";
import { fetchDeviceData, resolveEndpointUrl, updateDeviceConnection } from "../api/clockApi";
import { ConnectionTag } from "../components/ConnectionTag";
import { DataErrorState } from "../components/DataErrorState";
import { PageHeader } from "../components/PageHeader";
import type { DataSourceConfig, DeviceData } from "../types/clock";

export function DevicePage({
  dataSource,
  onOpenDataSource,
  onSwitchToDev,
  onNotice,
}: {
  dataSource: DataSourceConfig;
  onOpenDataSource: () => void;
  onSwitchToDev: () => void;
  onNotice: (message: string) => void;
}) {
  const [device, setDevice] = useState<DeviceData | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);

  useEffect(() => {
    const controller = new AbortController();
    setLoading(true);
    setError(null);

    fetchDeviceData(dataSource, controller.signal)
      .then((payload) => {
        setDevice(payload);
        setLoading(false);
      })
      .catch((err: unknown) => {
        if (controller.signal.aborted) return;
        setError(err instanceof Error ? err.message : "Failed to fetch device status.");
        setLoading(false);
      });

    return () => controller.abort();
  }, [dataSource, reloadToken]);

  async function handleToggleConnection() {
    if (!device) return;
    try {
      const updated = await updateDeviceConnection(dataSource, device, !device.connected);
      setDevice(updated);
      onNotice(updated.connected ? "Gateway connected" : "Gateway disconnected");
    } catch (err: unknown) {
      onNotice(err instanceof Error ? err.message : "Failed to update gateway connection");
    }
  }

  return (
    <section>
      <PageHeader title="Device" dataSource={dataSource} onOpenDataSource={onOpenDataSource} />
      {error ? (
        <DataErrorState
          error={error}
          endpoint={resolveEndpointUrl(dataSource, "device")}
          dataSource={dataSource}
          onRetry={() => setReloadToken((n) => n + 1)}
          onSwitchToDev={onSwitchToDev}
        />
      ) : loading || !device ? (
        <div className="glass-card gateway-card skeleton-card" aria-busy="true" />
      ) : (
        <article className="glass-card gateway-card">
          <h2>{device.name}</h2>
          <div className="gateway-update">
            <span>Last update</span>
            <strong>{device.lastUpdate}</strong>
          </div>
          <button
            className="connection-control"
            onClick={handleToggleConnection}
            aria-label="Toggle gateway connection"
          >
            <ConnectionTag connected={device.connected} label={device.connectionLabel} />
          </button>
        </article>
      )}
    </section>
  );
}
