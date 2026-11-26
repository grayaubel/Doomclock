import type { DataSourceConfig } from "../types/clock";

export function DataErrorState({
  error,
  endpoint,
  dataSource,
  onRetry,
  onSwitchToDev,
}: {
  error: string;
  endpoint: string;
  dataSource: DataSourceConfig;
  onRetry: () => void;
  onSwitchToDev: () => void;
}) {
  return (
    <div className="glass-card data-status-card">
      <h2>Unable to load external data</h2>
      <p>{error}</p>
      <code>GET {endpoint}</code>
      <div className="data-status-actions">
        <button type="button" className="pill-button" onClick={onRetry}>
          Retry Fetch
        </button>
        {dataSource.mode !== "dev" && (
          <button type="button" className="pill-button selected-action" onClick={onSwitchToDev}>
            Switch to Dev Mode ({dataSource.mockBasePath})
          </button>
        )}
      </div>
    </div>
  );
}
