import { useCallback, useEffect, useMemo, useState } from "react";
import { fetchDatabaseData, resolveEndpointUrl, updateDatabaseConnection } from "../api/clockApi";
import { ConnectionTag } from "../components/ConnectionTag";
import { DataErrorState } from "../components/DataErrorState";
import { PageHeader } from "../components/PageHeader";
import { RangeSelector } from "../components/RangeSelector";
import { icons } from "../components/icons";
import type { DatabaseData, DataSourceConfig, Range } from "../types/clock";

const cumulativeRanges: Record<Range, Range[]> = {
  Hour: ["Hour"],
  Day: ["Hour", "Day"],
  Week: ["Hour", "Day", "Week"],
  Month: ["Hour", "Day", "Week", "Month"],
};

export function DatabasePage({
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
  const [range, setRange] = useState<Range>("Hour");
  const [page, setPage] = useState(1);
  const [dbData, setDbData] = useState<DatabaseData | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [reveal, setReveal] = useState(false);
  const [reloadToken, setReloadToken] = useState(0);

  const loadDatabase = useCallback(
    (signal?: AbortSignal, notifyOnSuccess = false) => {
      setLoading(true);
      setError(null);
      fetchDatabaseData(dataSource, { range, page }, signal)
        .then((payload) => {
          setDbData(payload);
          setLoading(false);
          if (notifyOnSuccess) {
            onNotice("Database data refreshed");
          }
        })
        .catch((err: unknown) => {
          if (signal?.aborted) return;
          setError(err instanceof Error ? err.message : "Failed to fetch database records.");
          setLoading(false);
        });
    },
    [dataSource, range, page, onNotice],
  );

  useEffect(() => {
    const controller = new AbortController();
    loadDatabase(controller.signal, false);
    return () => controller.abort();
  }, [loadDatabase, reloadToken]);

  const filteredRecords = useMemo(() => {
    if (!dbData) return [];
    if (dataSource.mode === "api") {
      return dbData.records;
    }
    const allowed = cumulativeRanges[range];
    const hasRangeTags = dbData.records.some((record) => Boolean(record.range));
    return hasRangeTags
      ? dbData.records.filter((record) => allowed.includes(record.range))
      : dbData.records;
  }, [dbData, dataSource.mode, range]);

  const pageSize = dbData?.pageSize || 10;
  const totalPages = Math.max(1, Math.ceil(filteredRecords.length / pageSize));
  const safePage = Math.min(page, totalPages);

  const visibleRecords = useMemo(() => {
    if (dataSource.mode === "api" && dbData && dbData.records.length <= pageSize) {
      return dbData.records;
    }
    return filteredRecords.slice((safePage - 1) * pageSize, safePage * pageSize);
  }, [dataSource.mode, dbData, filteredRecords, safePage, pageSize]);

  async function handleEditConnection(field: "address" | "secret") {
    if (!dbData) return;
    const currentValue = dbData.connection[field];
    const next = window.prompt(`Edit ${field}`, currentValue);
    if (!next?.trim()) return;

    try {
      const updated = await updateDatabaseConnection(dataSource, dbData.connection, {
        [field]: next.trim(),
      });
      setDbData((prev) => (prev ? { ...prev, connection: updated } : prev));
      onNotice(`${field} updated`);
    } catch (err: unknown) {
      onNotice(err instanceof Error ? err.message : `Failed to update ${field}`);
    }
  }

  const displayedTotal =
    dataSource.mode !== "dev"
      ? (dbData?.totalResults ?? filteredRecords.length)
      : filteredRecords.length;

  return (
    <section>
      <PageHeader title="Database" dataSource={dataSource} onOpenDataSource={onOpenDataSource} />
      {error ? (
        <DataErrorState
          error={error}
          endpoint={resolveEndpointUrl(dataSource, "database", { range, page })}
          dataSource={dataSource}
          onRetry={() => setReloadToken((n) => n + 1)}
          onSwitchToDev={onSwitchToDev}
        />
      ) : loading || !dbData ? (
        <>
          <div className="glass-card database-card skeleton-card" aria-busy="true" />
          <div className="database-tools">
            <RangeSelector
              value={range}
              onChange={(nextRange) => {
                setRange(nextRange);
                setPage(1);
              }}
              sleep
            />
          </div>
          <div className="table-card glass-card skeleton-card" aria-busy="true" />
        </>
      ) : (
        <>
          <article className="glass-card database-card">
            <h2>{dbData.connection.engine}</h2>
            <div className="database-fields">
              <button onClick={() => handleEditConnection("address")}>
                <span>{dataSource.mode === "netpie" ? "Client ID" : "Address"}</span>
                <strong title={dbData.connection.address}>{dbData.connection.address}</strong>
                <img src={icons.edit} alt="" />
              </button>
              <button
                onClick={() => setReveal((value) => !value)}
                onDoubleClick={() => handleEditConnection("secret")}
                title="Click to reveal/hide, double-click to edit"
              >
                <span>{dataSource.mode === "netpie" ? "Token" : "Secret"}</span>
                <strong title={reveal ? dbData.connection.secret : undefined}>
                  {reveal
                    ? dbData.connection.secret
                    : "*".repeat(Math.min(12, dbData.connection.secret.length))}
                </strong>
                <img src={icons.edit} alt="" />
              </button>
            </div>
            <ConnectionTag
              connected={dbData.connection.connected}
              label={dbData.connection.connectionLabel}
            />
          </article>

          <div className="database-tools">
            <RangeSelector
              value={range}
              onChange={(nextRange) => {
                setRange(nextRange);
                setPage(1);
              }}
              sleep
            />
            <button className="pill-button" onClick={() => loadDatabase(undefined, true)}>
              Refresh
            </button>
          </div>

          <div className="table-card glass-card">
            <div className="table-scroll">
              <table>
                <thead>
                  <tr>
                    {dbData.columns.map((heading) => (
                      <th key={heading}>{heading}</th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {visibleRecords.map((record) => (
                    <tr key={record.id}>
                      <td>{record.timestamp}</td>
                      <td>{record.temp}</td>
                      <td>{record.light}</td>
                      <td>{record.humid}</td>
                      <td>{record.prediction}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </div>
          <div className="pagination">
            <span>Showing {displayedTotal} results</span>
            <div>
              <button onClick={() => setPage((value) => Math.min(totalPages, value + 1))}>
                Next
              </button>
              {Array.from({ length: totalPages }, (_, index) => index + 1).map((number) => (
                <button
                  className={safePage === number ? "selected" : ""}
                  key={number}
                  onClick={() => setPage(number)}
                >
                  {number}
                </button>
              ))}
              <button onClick={() => setPage((value) => Math.max(1, value - 1))}>Prev</button>
            </div>
          </div>
        </>
      )}
    </section>
  );
}
