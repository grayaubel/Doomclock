import type { AlarmStatusData } from "../../types/clock";
import { ConnectionTag } from "../ConnectionTag";
import { icons } from "../icons";

export function AlarmCard({ alarm }: { alarm: AlarmStatusData }) {
  const [hours = "--", minutes = "--"] = alarm.currentTime.split(":");
  return (
    <article className="glass-card alarm-card">
      <div className="clock-display">
        <span>{hours}</span>
        <b>:</b>
        <span>{minutes}</span>
      </div>
      <dl className="alarm-details">
        <div>
          <dt>Alarm</dt>
          <dd>{alarm.alarmTime}</dd>
        </div>
        <div>
          <dt>Lock duration</dt>
          <dd>
            <strong>{alarm.lockDurationMinutes}</strong> Minutes
          </dd>
        </div>
        <div>
          <dt>Lock status</dt>
          <dd>
            <img src={icons.lock} alt="" /> {alarm.lockStatus}
          </dd>
        </div>
      </dl>
      <ConnectionTag connected={alarm.connected} label={alarm.connectionLabel} />
    </article>
  );
}
