import type { DataSourceConfig } from "../types/clock";

export function PageHeader({
  title,
  dataSource,
  onOpenDataSource,
}: {
  title: string;
  dataSource: DataSourceConfig;
  onOpenDataSource: () => void;
}) {
  const activePath =
    dataSource.mode === "netpie"
      ? `${dataSource.netpie.clientId.slice(0, 8)}…`
      : dataSource.mode === "dev"
        ? dataSource.mockBasePath
        : dataSource.streamUrl?.trim() || dataSource.apiBaseUrl;
  const statusLabel =
    dataSource.mode === "netpie"
      ? dataSource.feedEnabled
        ? "NETPIE FEED"
        : "NETPIE 2020"
      : dataSource.mode === "dev"
        ? dataSource.feedEnabled
          ? "DEV FEED"
          : "DEV MOCK"
        : dataSource.feedEnabled
          ? "LIVE FEED"
          : "LIVE API";
  return (
    <div className="page-header">
      <h1 className="page-title">{title}</h1>
      <button
        type="button"
        className={`data-source-pill ${dataSource.mode}`}
        onClick={onOpenDataSource}
        title="Configure NETPIE / API / Dev Mode feed settings"
      >
        <span className={`data-source-dot ${dataSource.feedEnabled ? "streaming" : ""}`} />
        <span>{statusLabel}</span>
        <code>{activePath}</code>
      </button>
    </div>
  );
}
