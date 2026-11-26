import { useState } from "react";
import { getDefaultDataSourceConfig, resolveEndpointUrl } from "../api/clockApi";
import type { DataMode, DataSourceConfig } from "../types/clock";

export function DataSourceModal({
  config,
  onSave,
  onClose,
}: {
  config: DataSourceConfig;
  onSave: (config: DataSourceConfig) => void;
  onClose: () => void;
}) {
  const [mode, setMode] = useState<DataMode>(config.mode);
  const [netpieClientId, setNetpieClientId] = useState(config.netpie.clientId);
  const [netpieToken, setNetpieToken] = useState(config.netpie.token);
  const [netpieSecret, setNetpieSecret] = useState(config.netpie.secret);
  const [mockBasePath, setMockBasePath] = useState(config.mockBasePath);
  const [apiBaseUrl, setApiBaseUrl] = useState(config.apiBaseUrl);
  const [streamUrl, setStreamUrl] = useState(config.streamUrl ?? "");
  const [feedEnabled, setFeedEnabled] = useState(config.feedEnabled);
  const [feedIntervalMs, setFeedIntervalMs] = useState(String(config.feedIntervalMs));

  const parsedInterval = Number(feedIntervalMs);
  const defaults = getDefaultDataSourceConfig();
  const previewConfig: DataSourceConfig = {
    mode,
    netpie: {
      clientId: netpieClientId.trim() || defaults.netpie.clientId,
      token: netpieToken.trim() || defaults.netpie.token,
      secret: netpieSecret.trim() || defaults.netpie.secret,
    },
    mockBasePath: mockBasePath.trim() || "/mock",
    apiBaseUrl: apiBaseUrl.trim() || "/api",
    streamUrl: streamUrl.trim(),
    feedEnabled,
    feedIntervalMs:
      Number.isFinite(parsedInterval) && parsedInterval >= 400 ? Math.round(parsedInterval) : 2500,
  };

  function handleReset() {
    setMode(defaults.mode);
    setNetpieClientId(defaults.netpie.clientId);
    setNetpieToken(defaults.netpie.token);
    setNetpieSecret(defaults.netpie.secret);
    setMockBasePath(defaults.mockBasePath);
    setApiBaseUrl(defaults.apiBaseUrl);
    setStreamUrl(defaults.streamUrl ?? "");
    setFeedEnabled(defaults.feedEnabled);
    setFeedIntervalMs(String(defaults.feedIntervalMs));
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div
        className="glass-card modal-card"
        role="dialog"
        aria-modal="true"
        aria-labelledby="data-source-title"
        onClick={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <h2 id="data-source-title">Data Provider &amp; Dev Mode</h2>
          <button type="button" className="pill-button" onClick={onClose}>
            Close
          </button>
        </div>
        <p className="modal-subtitle">
          Connect directly to NETPIE 2020 (Doomclock gateway), use the Dev Mode mock feed, or point
          to a custom API.
        </p>
        <div className="mode-toggle-group three-cols">
          <button
            type="button"
            className={mode === "netpie" ? "selected" : ""}
            onClick={() => setMode("netpie")}
          >
            NETPIE 2020
          </button>
          <button
            type="button"
            className={mode === "dev" ? "selected" : ""}
            onClick={() => setMode("dev")}
          >
            Dev Mode
          </button>
          <button
            type="button"
            className={mode === "api" ? "selected" : ""}
            onClick={() => setMode("api")}
          >
            Custom API
          </button>
        </div>

        <div className="modal-fields">
          {mode === "netpie" && (
            <>
              <label className="modal-field">
                <span>NETPIE Client ID (Device &quot;gateway&quot;)</span>
                <input
                  type="text"
                  value={netpieClientId}
                  onChange={(event) => setNetpieClientId(event.target.value)}
                  placeholder="39c1b827-259e-4b75-8751-8100ec25771c"
                />
              </label>
              <label className="modal-field">
                <span>NETPIE Token</span>
                <input
                  type="text"
                  value={netpieToken}
                  onChange={(event) => setNetpieToken(event.target.value)}
                  placeholder="wqVCPnhR4ekZnQUCNDhA7ch9XET4arj8"
                />
              </label>
              <label className="modal-field">
                <span>NETPIE Secret</span>
                <input
                  type="password"
                  value={netpieSecret}
                  onChange={(event) => setNetpieSecret(event.target.value)}
                  placeholder="sRqFBCyivkr5VsLgiWpHc3nNRrUmmsbG"
                />
              </label>
            </>
          )}
          {mode === "dev" && (
            <label className="modal-field">
              <span>Dev Mode Mock Seed Path</span>
              <input
                type="text"
                value={mockBasePath}
                onChange={(event) => setMockBasePath(event.target.value)}
                placeholder="/mock"
              />
            </label>
          )}
          {mode === "api" && (
            <>
              <label className="modal-field">
                <span>External API Base URL</span>
                <input
                  type="text"
                  value={apiBaseUrl}
                  onChange={(event) => setApiBaseUrl(event.target.value)}
                  placeholder="http://localhost:8000/api"
                />
              </label>
              <label className="modal-field">
                <span>
                  Optional Live Stream URL (SSE or WebSocket; blank = poll GET /dashboard)
                </span>
                <input
                  type="text"
                  value={streamUrl}
                  onChange={(event) => setStreamUrl(event.target.value)}
                  placeholder="ws://localhost:8000/ws/dashboard"
                />
              </label>
            </>
          )}
          <div className="modal-field-row">
            <label className="modal-field">
              <span>Feed Interval (ms)</span>
              <input
                type="number"
                min={400}
                step={200}
                value={feedIntervalMs}
                onChange={(event) => setFeedIntervalMs(event.target.value)}
              />
            </label>
            <div className="modal-field">
              <span>Continuous Feed</span>
              <button
                type="button"
                className={`pill-button feed-toggle-button ${feedEnabled ? "selected-action" : ""}`}
                onClick={() => setFeedEnabled((prev) => !prev)}
              >
                {feedEnabled ? "Streaming ON" : "Paused"}
              </button>
            </div>
          </div>
        </div>

        <div className="endpoint-preview">
          <span>
            Active Feed (
            {mode === "netpie" ? "NETPIE 2020" : mode === "dev" ? "Dev Mocker" : "Live API"} · every{" "}
            {previewConfig.feedIntervalMs}ms)
          </span>
          <code>
            {mode === "api" && previewConfig.streamUrl
              ? `STREAM ${previewConfig.streamUrl}`
              : `FEED ${resolveEndpointUrl(previewConfig, "dashboard")}`}
          </code>
          <code>GET {resolveEndpointUrl(previewConfig, "device")}</code>
          <code>
            GET {resolveEndpointUrl(previewConfig, "database", { range: "Hour", page: 1 })}
          </code>
        </div>

        <div className="modal-actions">
          <button type="button" className="pill-button" onClick={handleReset}>
            Reset Defaults
          </button>
          <button
            type="button"
            className="pill-button selected-action"
            onClick={() => onSave(previewConfig)}
          >
            Apply &amp; Fetch Data
          </button>
        </div>
      </div>
    </div>
  );
}
