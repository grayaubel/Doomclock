import { useState } from "react";
import type { Range } from "../types/clock";
import { TabIcon } from "./TabIcon";

export function RangeSelector({
  value,
  onChange,
  sleep = false,
}: {
  value: Range;
  onChange: (value: Range) => void;
  sleep?: boolean;
}) {
  const [customSelected, setCustomSelected] = useState(false);
  const options: Range[] = sleep ? ["Hour", "Day", "Week", "Month"] : ["Hour", "Day", "Week"];
  return (
    <div className="range-selector">
      {options.map((option, index) => (
        <button
          key={option}
          className={!customSelected && value === option ? "selected" : ""}
          onClick={() => {
            setCustomSelected(false);
            onChange(option);
          }}
        >
          {sleep && index === 0 ? "12 Hour" : option}
        </button>
      ))}
      <button
        className={customSelected ? "selected" : ""}
        aria-label="Select custom date range"
        title="Select custom date range"
        onClick={() => {
          const start = window.prompt("Start date (YYYY-MM-DD)");
          if (!start) return;
          const end = window.prompt("End date (YYYY-MM-DD)", start);
          if (!end) return;

          const startDate = new Date(`${start}T00:00:00`);
          const endDate = new Date(`${end}T00:00:00`);
          const duration = Math.ceil((endDate.getTime() - startDate.getTime()) / 86_400_000);

          if (
            Number.isNaN(duration) ||
            duration < 0 ||
            !/^\d{4}-\d{2}-\d{2}$/.test(start) ||
            !/^\d{4}-\d{2}-\d{2}$/.test(end)
          ) {
            window.alert("Please enter a valid date range.");
            return;
          }

          setCustomSelected(true);
          onChange(
            duration <= 1 ? "Hour" : duration <= 7 ? "Day" : duration <= 31 ? "Week" : "Month",
          );
        }}
      >
        <TabIcon name="calendar" />
      </button>
    </div>
  );
}
