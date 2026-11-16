// settings.cpp — the 7-day wake schedule and sleep goal (set on the touch screen, kept in NVS).
// Every day always has a wake time; "silent" days don't ring but still jail the phone and
// let the AI decide when you are really up (agreed 14 Nov 2026).
#include "shared.h"
#include <Preferences.h>

Settings gSettings;
static Preferences prefs;
static time_t wakeOverride = 0;    // one-shot: the knob moved the next wake

void settingsBegin() {
  prefs.begin("dcset", false);
  memset(&gSettings, 0, sizeof(gSettings));
  if (prefs.getBytesLength("s") == sizeof(Settings)) prefs.getBytes("s", &gSettings, sizeof(Settings));
  if (gSettings.magic != SETTINGS_MAGIC) {         // defaults shown in the first-boot setup screen
    for (int d = 0; d < 7; d++) gSettings.day[d] = {7, 0, 1};
    gSettings.sleepGoalMin = 8 * 60;
    gSettings.dataMode = 0;
  }
  wakeOverride = (time_t)prefs.getULong("ovr", 0);
}

bool settingsValid() { return gSettings.magic == SETTINGS_MAGIC; }

void settingsSave() {
  gSettings.magic = SETTINGS_MAGIC;
  prefs.putBytes("s", &gSettings, sizeof(Settings));
}

time_t scheduledWakeAfter(time_t from, bool* ring) {
  struct tm lt;
  localtime_r(&from, &lt);
  for (int d = 0; d <= 7; d++) {
    struct tm t = lt;
    t.tm_mday += d;
    t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0; t.tm_isdst = -1;
    time_t midnight = mktime(&t);                  // normalises the date
    struct tm m;
    localtime_r(&midnight, &m);
    const DaySlot& s = gSettings.day[m.tm_wday];
    time_t w = midnight + s.hour * 3600 + s.minute * 60;
    if (w > from) { if (ring) *ring = s.ring; return w; }
  }
  return 0;
}

time_t nextWake(time_t now, bool* ring) {
  bool r = true;
  time_t sched = scheduledWakeAfter(now, &r);
  if (wakeOverride > now && wakeOverride - now < 24 * 3600) {
    // the override replaces the scheduled wake it was made from (within 12 h of it)
    if (llabs((long long)wakeOverride - (long long)sched) <= 12 * 3600) { if (ring) *ring = r; return wakeOverride; }
  }
  if (ring) *ring = r;
  return sched;
}

void nudgeNextWake(time_t now, int minutes) {
  bool r;
  time_t base = nextWake(now, &r);
  if (!base) return;
  time_t sched = scheduledWakeAfter(now, &r);
  time_t w = base + minutes * 60;
  if (w <= now + 60) return;                                         // never into the past
  if (llabs((long long)w - (long long)sched) > 12 * 3600) return;   // keep it near the schedule
  wakeOverride = (w == sched) ? 0 : w;
  prefs.putULong("ovr", (uint32_t)wakeOverride);
}

void clearWakeOverride() {
  wakeOverride = 0;
  prefs.putULong("ovr", 0);
}
