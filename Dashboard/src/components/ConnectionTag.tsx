import { icons } from "./icons";

export function ConnectionTag({
  connected = true,
  label,
}: {
  connected?: boolean;
  label?: string;
}) {
  return (
    <div className={`connection-tag ${connected ? "" : "offline"}`}>
      <span>{label ?? (connected ? "Connected" : "Disconnected")}</span>
      <img src={connected ? icons.wifi : icons.wifiOff} alt="" />
    </div>
  );
}
