import { useEffect, useState } from "react";
import { loadDataSourceConfig, saveDataSourceConfig } from "./api/clockApi";
import { DataSourceModal } from "./components/DataSourceModal";
import { Sidebar } from "./components/Sidebar";
import { DashboardPage } from "./pages/DashboardPage";
import { DatabasePage } from "./pages/DatabasePage";
import { DevicePage } from "./pages/DevicePage";
import type { DataSourceConfig, Page } from "./types/clock";

function currentPage(): Page {
  const value = window.location.hash.replace("#/", "");
  return value === "device" || value === "database" ? value : "dashboard";
}

export default function App() {
  const [page, setPage] = useState<Page>(currentPage);
  const [notice, setNotice] = useState("");
  const [dataSource, setDataSource] = useState<DataSourceConfig>(loadDataSourceConfig);
  const [configModalOpen, setConfigModalOpen] = useState(false);

  useEffect(() => {
    const onHash = () => setPage(currentPage());
    window.addEventListener("hashchange", onHash);
    return () => window.removeEventListener("hashchange", onHash);
  }, []);

  function go(next: Page) {
    window.location.hash = `/${next}`;
    setPage(next);
  }

  function showNotice(message: string) {
    setNotice(message);
    window.setTimeout(() => setNotice(""), 2200);
  }

  function handleSaveDataSource(nextConfig: DataSourceConfig) {
    saveDataSourceConfig(nextConfig);
    setDataSource(nextConfig);
    setConfigModalOpen(false);
    showNotice(
      nextConfig.mode === "netpie"
        ? `NETPIE 2020 linked (${nextConfig.netpie.clientId.slice(0, 8)}…)`
        : nextConfig.mode === "dev"
          ? `Dev mode active (${nextConfig.mockBasePath})`
          : `Live API active (${nextConfig.apiBaseUrl})`,
    );
  }

  function handleFallbackToDevMode() {
    const nextConfig: DataSourceConfig = { ...dataSource, mode: "dev" };
    saveDataSourceConfig(nextConfig);
    setDataSource(nextConfig);
    showNotice(`Switched to Dev mode (${nextConfig.mockBasePath})`);
  }

  return (
    <div className="app-shell">
      <Sidebar
        page={page}
        configOpen={configModalOpen}
        onNavigate={go}
        onOpenDataSource={() => setConfigModalOpen(true)}
        onNotice={showNotice}
      />
      <main className="main-content">
        {page === "dashboard" && (
          <DashboardPage
            dataSource={dataSource}
            onOpenDataSource={() => setConfigModalOpen(true)}
            onSwitchToDev={handleFallbackToDevMode}
          />
        )}
        {page === "device" && (
          <DevicePage
            dataSource={dataSource}
            onOpenDataSource={() => setConfigModalOpen(true)}
            onSwitchToDev={handleFallbackToDevMode}
            onNotice={showNotice}
          />
        )}
        {page === "database" && (
          <DatabasePage
            dataSource={dataSource}
            onOpenDataSource={() => setConfigModalOpen(true)}
            onSwitchToDev={handleFallbackToDevMode}
            onNotice={showNotice}
          />
        )}
      </main>
      {configModalOpen && (
        <DataSourceModal
          config={dataSource}
          onSave={handleSaveDataSource}
          onClose={() => setConfigModalOpen(false)}
        />
      )}
      {notice && <div className="toast">{notice}</div>}
    </div>
  );
}
