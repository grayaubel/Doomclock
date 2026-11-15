// record.cpp — builds the JSON that goes to NETPIE (@shadow/data/update).
// Field names kept from the first cloud draft where they still apply
// (action, note, tempCelsius, humidityPercent, lightIntensity, phoneInserted, node_id).
// The draft's ISO "timestamp" string is now "time_local"; NETPIE's own top-level "timestamp" (epoch ms) is added at the end.
#include "shared.h"
#include <math.h>

namespace {
struct Json {
  char* b; size_t cap; size_t n = 0; bool first = true;
  Json(char* buf, size_t c) : b(buf), cap(c) { b[0] = 0; }
  void raw(const char* s) { while (*s && n + 1 < cap) b[n++] = *s++; b[n] = 0; }
  void key(const char* k) { if (!first) raw(","); first = false; raw("\""); raw(k); raw("\":"); }
  void str(const char* k, const char* v) {
    key(k); raw("\"");
    for (; *v && n + 2 < cap; v++) {
      if (*v == '"' || *v == '\\') { b[n++] = '\\'; b[n++] = *v; }
      else if ((uint8_t)*v >= 0x20) b[n++] = *v;
    }
    b[n] = 0; raw("\"");
  }
  void num(const char* k, float v, int dec) {
    if (isnan(v)) return;          // omit unknown values: the NETPIE schema types these fields as number
    key(k);
    char t[24]; snprintf(t, sizeof(t), "%.*f", dec, v); raw(t);
  }
  void i(const char* k, long v) { key(k); char t[16]; snprintf(t, sizeof(t), "%ld", v); raw(t); }
  void bol(const char* k, bool v) { key(k); raw(v ? "true" : "false"); }
};
}  // namespace

size_t buildRecord(char* out, size_t cap, const char* action, const char* note, bool withBed) {
  GatewaySensors s; BedState bed; LinkStats link;
  lockState(); s = gSens; bed = gBed; link = gLink; unlockState();

  Json j(out, cap);
  j.raw("{\"data\":{");
  j.str("action", action);
  if (note && *note) j.str("note", note);

  char ts[32];
  time_t t = (withBed && bed.have && bed.windowEnd) ? bed.windowEnd : nowUtc();
  isoLocal(timeValid() ? t : 0, ts, sizeof(ts));
  j.str("time_local", ts);           // human-readable; NETPIE uses the top-level "timestamp" below

  j.num("tempCelsius", s.tempC, 1);
  j.num("humidityPercent", s.hum, 0);
  j.num("lightIntensity", s.lux, 0);
  j.i("hallRaw", s.hallRaw);
  j.bol("phoneInserted", s.trayDown);

  bool fresh = bed.have && (millis() - bed.rxMs) < BED_FRESH_MS;
  j.bol("bed_fresh", fresh);
  if (withBed && bed.have) {
    const BedPacket& p = bed.last;
    j.i("node_id", p.node_id);
    j.i("bed_seq", (long)p.seq);
    j.i("bed_age_s", p.age_s);
    j.i("bed_imu_n", p.imu_n);
    j.i("bed_move_count", p.move_count);
    j.i("bed_acc_dev_mg", p.acc_dev_mean_mg);
    j.i("bed_acc_dev_max_mg", p.acc_dev_max_mg);
    j.i("bed_jerk_mg", p.jerk_mean_mg);
    j.i("bed_jerk_max_mg", p.jerk_max_mg);
    j.i("bed_gyro_cdps", p.gyro_mean_cdps);
    j.i("bed_gyro_max_cdps", p.gyro_max_cdps);
    j.i("bed_tilt_x", p.tilt_x_deg);
    j.i("bed_tilt_y", p.tilt_y_deg);
    j.i("bed_tof_valid", p.tof_valid);
    j.i("bed_tof_n", p.tof_n);
    j.i("bed_tof_mm", p.tof_mean_mm);
    j.i("bed_tof_min_mm", p.tof_min_mm);
    j.i("bed_tof_max_mm", p.tof_max_mm);
    j.i("bed_tof_std_mm", p.tof_std_mm);
    if (p.flags & BED_F_MIC_ON) { j.i("bed_mic_p2p", p.mic_p2p_mean); j.i("bed_mic_p2p_max", p.mic_p2p_max); }
    j.i("node_attempts", p.attempts);
    j.i("node_backlog", p.backlog);
    j.i("node_dropped", p.dropped_total);
  }
  j.i("link_rx", (long)link.rx);
  j.i("link_lost", (long)link.lost);
  j.i("link_dup", (long)link.dup);
  uint32_t expected = link.rx + link.lost;
  j.num("link_pdr", expected ? 100.0f * link.rx / expected : NAN, 2);
  j.raw("}");
  // NETPIE reads a top-level "timestamp" (epoch ms) as the time of this point. Records replayed
  // from the offline queue therefore land at their real time, and because they are older than
  // the shadow's last point they don't overwrite the current shadow values.
  if (timeValid() && t > 0) {
    char tsms[32];
    snprintf(tsms, sizeof(tsms), ",\"timestamp\":%llu", (unsigned long long)t * 1000ULL);
    j.raw(tsms);
  }
  j.raw("}");
  if (j.n + 1 >= cap) Serial.println("[CLOUD] WARNING: record truncated — enlarge CloudMsg");
  return j.n;
}

void postEvent(const char* action, const char* note) {
  CloudMsg m;
  buildRecord(m.json, sizeof(m.json), action, note, false);
  if (xQueueSend(gCloudQ, &m, 0) != pdTRUE) Serial.println("[CLOUD] queue full, event dropped");
  Serial.printf("[EVENT] %s %s\n", action, note ? note : "");
}

void postBedRecord() {
  CloudMsg m;
  buildRecord(m.json, sizeof(m.json), "bed", "", true);
  if (xQueueSend(gCloudQ, &m, 0) != pdTRUE) Serial.println("[CLOUD] queue full, bed record dropped");
  gLastBedPostMs = millis();
}

void postNotify(const char* text) {
  NotifyMsg n;
  size_t len = strlen(text);
  if (len > sizeof(n.text) - 1) {
    len = sizeof(n.text) - 1;
    while (len > 0 && (((uint8_t)text[len]) & 0xC0) == 0x80) len--;   // don't cut a Thai (UTF-8) character in half
  }
  memcpy(n.text, text, len); n.text[len] = 0;
  if (xQueueSend(gNotifyQ, &n, 0) != pdTRUE) Serial.println("[NOTIFY] queue full");
}
