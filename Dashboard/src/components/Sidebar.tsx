import type { Page } from "../types/clock";
import { icons } from "./icons";
import { TabIcon, type TabIconName } from "./TabIcon";

const nav: { id: Page | "provider"; label: string; icon: TabIconName }[] = [
  { id: "dashboard", label: "Dashboard", icon: "home" },
  { id: "device", label: "Device", icon: "device" },
  { id: "database", label: "Database", icon: "database" },
  { id: "provider", label: "Provider", icon: "provider" },
];

export function Sidebar({
  page,
  configOpen,
  onNavigate,
  onOpenDataSource,
  onNotice,
}: {
  page: Page;
  configOpen: boolean;
  onNavigate: (page: Page) => void;
  onOpenDataSource: () => void;
  onNotice: (message: string) => void;
}) {
  return (
    <aside className="sidebar">
      <img className="sidebar-bg" src={icons.sidebarBg} alt="" />
      <div className="brand">
        <img src={icons.logo} alt="Doom Clock" />
      </div>
      <div className="sidebar-panel">
        <nav className="nav-list" aria-label="Primary navigation">
          {nav.map((item) => (
            <button
              className={`nav-button ${
                item.id === "provider"
                  ? configOpen
                    ? "active"
                    : ""
                  : !configOpen && item.id === page
                    ? "active"
                    : ""
              }`}
              key={item.id}
              onClick={() => (item.id === "provider" ? onOpenDataSource() : onNavigate(item.id))}
            >
              <TabIcon name={item.icon} />
              {item.label}
            </button>
          ))}
        </nav>
        <div className="utility-list">
          {(
            [
              ["settings", "Maintainance"],
              ["help", "Help"],
              ["info", "About"],
            ] as const
          ).map(([icon, label]) => (
            <button
              className="nav-button"
              key={label}
              onClick={() =>
                label === "Maintainance" ? onOpenDataSource() : onNotice(`${label} panel opened`)
              }
            >
              <TabIcon name={icon} />
              {label}
            </button>
          ))}
        </div>
      </div>
    </aside>
  );
}
