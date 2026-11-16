// ui.cpp — screens on the ILI9488 (480x320 landscape) with XPT2046 touch, via TFT_eSPI.
// Text is English because TFT_eSPI's built-in fonts have no Thai glyphs.
// Only the parts that changed are redrawn (an 18-bit SPI panel is slow to repaint fully).
#include "ui.h"
#include <TFT_eSPI.h>
#include <Preferences.h>

static TFT_eSPI tft;
static Preferences uiPrefs;
static uint16_t calData[5];
static bool haveCal = false;

static UiScreen screen = SCR_HOME;
static bool fullRedraw = true;
static Settings work;              // working copy while editing
static int sel = -1;               // selected field: 0..6 = day time, 7 = sleep goal

// colours
static uint16_t C_BG, C_CARD, C_DIGIT, C_ORANGE, C_MUTED, C_TEXT, C_GREEN, C_RED, C_BLUE, C_PANEL;

// touch targets rebuilt on every full draw
struct Btn { int16_t x, y, w, h; uint8_t action; int8_t arg; };
static Btn btns[28];
static int nBtns = 0;
static bool touchWasDown = false;

static const char* DAYS[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char* MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

enum : uint8_t { BA_ACTION = 0, BA_SEL_DAY = 100, BA_TOGGLE_RING = 101, BA_SEL_GOAL = 102, BA_TOGGLE_DATA = 103 };

static void addBtn(int x, int y, int w, int h, uint8_t action, int8_t arg = 0) {
  if (nBtns < 28) btns[nBtns++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, action, arg};
}

static void button(int x, int y, int w, int h, const char* label, uint16_t fill, uint16_t text, uint8_t action, int8_t arg = 0) {
  tft.fillRoundRect(x, y, w, h, 8, fill);
  tft.setTextColor(text, fill);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2 + 1, 4);
  addBtn(x, y, w, h, action, arg);
}

void uiBegin() {
  tft.init();
  tft.setRotation(1);                       // 480 x 320 landscape
  C_BG = TFT_BLACK;
  C_CARD = tft.color565(52, 52, 52);
  C_DIGIT = tft.color565(246, 239, 226);
  C_ORANGE = tft.color565(244, 162, 89);
  C_MUTED = tft.color565(140, 140, 140);
  C_TEXT = tft.color565(235, 235, 235);
  C_GREEN = tft.color565(80, 200, 120);
  C_RED = tft.color565(230, 80, 70);
  C_BLUE = tft.color565(91, 141, 239);
  C_PANEL = tft.color565(28, 28, 28);
  tft.fillScreen(C_BG);
  uiPrefs.begin("dcui", false);
  if (uiPrefs.getBytesLength("tcal") == sizeof(calData)) {
    uiPrefs.getBytes("tcal", calData, sizeof(calData));
    tft.setTouch(calData);
    haveCal = true;
  }
}

bool uiHasCalibration() { return haveCal; }

void uiCalibrate() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Touch calibration", 240, 120, 4);
  tft.drawString("Tap each corner arrow as it appears", 240, 160, 2);
  delay(1200);
  tft.calibrateTouch(calData, C_ORANGE, C_BG, 15);
  uiPrefs.putBytes("tcal", calData, sizeof(calData));
  haveCal = true;
  fullRedraw = true;
}

void uiShow(UiScreen s) {
  if (s == SCR_SETTINGS || s == SCR_SETUP) { work = gSettings; sel = -1; }
  screen = s;
  fullRedraw = true;
}

UiScreen uiScreen() { return screen; }

void uiKnob(int d) {
  if (screen != SCR_SETTINGS && screen != SCR_SETUP) return;
  if (sel >= 0 && sel < 7) {
    int m = work.day[sel].hour * 60 + work.day[sel].minute + d * 5;
    m = (m % 1440 + 1440) % 1440;
    work.day[sel].hour = m / 60;
    work.day[sel].minute = m % 60;
  } else if (sel == 7) {
    int g = (int)work.sleepGoalMin + d * 15;
    work.sleepGoalMin = g < 240 ? 240 : (g > 720 ? 720 : g);
  }
  fullRedraw = true;
}

void uiKnobPress() {
  if (screen != SCR_SETTINGS && screen != SCR_SETUP) return;
  sel = (sel + 1) % 8;
  fullRedraw = true;
}

UiAction uiPollTouch() {
  uint16_t x, y;
  bool down = haveCal && tft.getTouch(&x, &y, 500);
  bool tap = down && !touchWasDown;
  touchWasDown = down;
  if (!tap) return UA_NONE;
  for (int i = 0; i < nBtns; i++) {
    const Btn& b = btns[i];
    if (x < b.x || x >= b.x + b.w || y < b.y || y >= b.y + b.h) continue;
    switch (b.action) {
      case BA_SEL_DAY:  sel = b.arg; fullRedraw = true; return UA_NONE;
      case BA_SEL_GOAL: sel = 7; fullRedraw = true; return UA_NONE;
      case BA_TOGGLE_RING: work.day[b.arg].ring ^= 1; fullRedraw = true; return UA_NONE;
      case BA_TOGGLE_DATA: work.dataMode ^= 1; fullRedraw = true; return UA_NONE;
      default: break;
    }
    UiAction a = (UiAction)b.action;
    if (a == UA_SAVED) { gSettings = work; settingsSave(); uiShow(SCR_HOME); }
    else if (a == UA_CANCEL) uiShow(SCR_HOME);
    else if (a == UA_OPEN_SETTINGS) uiShow(SCR_SETTINGS);
    return a;
  }
  return UA_NONE;
}

// ---------------- settings / setup ----------------
static void drawSettings(bool setup) {
  tft.fillScreen(C_BG);
  nBtns = 0;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_ORANGE, C_BG);
  tft.drawString(setup ? "Setup: wake time for every day" : "Wake schedule", 14, 8, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("Tap a time, turn the knob (5 min). Knob press = next field.", 14, 36, 2);
  char buf[24];
  for (int d = 0; d < 7; d++) {
    int y = 58 + d * 30;
    bool s = (sel == d);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString(DAYS[d], 18, y + 6, 2);
    uint16_t f = s ? C_ORANGE : C_CARD;
    tft.fillRoundRect(70, y, 96, 26, 6, f);
    snprintf(buf, sizeof(buf), "%02d:%02d", work.day[d].hour, work.day[d].minute);
    tft.setTextColor(s ? TFT_BLACK : C_DIGIT, f);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(buf, 118, y + 14, 4);
    addBtn(70, y, 96, 26, BA_SEL_DAY, d);
    uint16_t rf = work.day[d].ring ? C_BLUE : C_CARD;
    tft.fillRoundRect(176, y, 90, 26, 6, rf);
    tft.setTextColor(C_TEXT, rf);
    tft.drawString(work.day[d].ring ? "RING" : "SILENT", 221, y + 14, 2);
    addBtn(176, y, 90, 26, BA_TOGGLE_RING, d);
    tft.setTextDatum(TL_DATUM);
  }
  // right column
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString("Sleep goal", 300, 62, 2);
  uint16_t gf = (sel == 7) ? C_ORANGE : C_CARD;
  tft.fillRoundRect(300, 82, 160, 34, 6, gf);
  snprintf(buf, sizeof(buf), "%dh%02d", work.sleepGoalMin / 60, work.sleepGoalMin % 60);
  tft.setTextColor(sel == 7 ? TFT_BLACK : C_DIGIT, gf);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(buf, 380, 100, 4);
  addBtn(300, 82, 160, 34, BA_SEL_GOAL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString("Data collection", 300, 128, 2);
  uint16_t df = work.dataMode ? C_GREEN : C_CARD;
  tft.fillRoundRect(300, 148, 160, 30, 6, df);
  tft.setTextColor(TFT_BLACK, df);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(work.dataMode ? "ON" : "OFF", 380, 163, 4);
  addBtn(300, 148, 160, 30, BA_TOGGLE_DATA);
  button(300, 190, 160, 30, "Touch cal", C_CARD, C_TEXT, UA_TOUCH_CAL);
  button(300, 272, 160, 40, "SAVE", C_ORANGE, TFT_BLACK, UA_SAVED);
  if (!setup) button(300, 228, 160, 36, "Cancel", C_CARD, C_TEXT, UA_CANCEL);
}

// ---------------- home ----------------
static int lastMin = -2;
static char lastTop[64] = "", lastStatus[200] = "";

static void drawCards(int hh, int mm, bool known) {
  char buf[4];
  const int y = 40, w = 210, h = 140;
  for (int i = 0; i < 2; i++) {
    int x = 22 + i * 226;
    tft.fillRoundRect(x, y, w, h, 14, C_CARD);
    tft.setTextColor(C_DIGIT, C_CARD);
    tft.setTextDatum(MC_DATUM);
    if (known) snprintf(buf, sizeof(buf), "%02d", i == 0 ? hh : mm); else strcpy(buf, "--");
    tft.drawString(buf, x + w / 2, y + h / 2 + 2, 8);
    tft.drawFastHLine(x, y + h / 2, w, C_BG);             // the flip-card split
    tft.drawFastHLine(x, y + h / 2 + 1, w, C_BG);
  }
}

static void fmtTime(time_t t, char* out, size_t n) {
  if (!t) { snprintf(out, n, "--:--"); return; }
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(out, n, "%s %02d:%02d", DAYS[lt.tm_wday], lt.tm_hour, lt.tm_min);
}

static void statusLines(const UiModel& m, char* title, char* sub, size_t n) {
  char w[16];
  fmtTime(m.wake, w, sizeof(w));
  switch (m.state) {
    case ST_IDLE:     snprintf(title, n, m.now ? "Next wake %s" : "Waiting for the time...", w); break;
    case ST_NAG:      snprintf(title, n, "Put your phone in!"); break;
    case ST_LOCKED:   snprintf(title, n, "Locked until %s", w); break;
    case ST_SLEEPING: snprintf(title, n, "Good night - wake %s", w); break;
    case ST_ALARM:    snprintf(title, n, m.awaitingAwakeTap ? "WAKE UP - tap I'M AWAKE" : "WAKE UP - get out of bed"); break;
    case ST_UP:       snprintf(title, n, "Up %d / %d min", (int)m.upMin, UP_REQUIRED_MIN); break;
    case ST_UNLOCKED: snprintf(title, n, "Unlocked - take your phone"); break;
    default:          title[0] = 0;
  }
  snprintf(sub, n, "%s  |  %s  |  goal %dh%02d  |  %s",
           m.ringDay ? "ring" : "silent", m.phoneLocked ? "phone locked" : "no phone",
           gSettings.sleepGoalMin / 60, gSettings.sleepGoalMin % 60,
           m.dataMode ? "DATA MODE" : (m.aiTrained ? "AI" : "AI: untrained"));
}

static void drawHome(const UiModel& m, bool force) {
  if (force) { tft.fillScreen(C_BG); lastMin = -2; lastTop[0] = lastStatus[0] = 0; }

  // top bar
  char top[64];
  struct tm lt;
  if (m.now) { localtime_r(&m.now, &lt); snprintf(top, sizeof(top), "%s %d %s", DAYS[lt.tm_wday], lt.tm_mday, MONTHS[lt.tm_mon]); }
  else snprintf(top, sizeof(top), "time unknown");
  char sig[64];
  snprintf(sig, sizeof(sig), "%s|%d%d%d", top, m.wifi, m.node, m.cloud);
  if (strcmp(sig, lastTop) != 0) {
    strcpy(lastTop, sig);
    tft.fillRect(0, 0, 480, 34, C_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString(top, 14, 9, 2);
    const char* names[3] = {"WIFI", "NODE", "CLOUD"};
    bool ok[3] = {m.wifi, m.node, m.cloud};
    for (int i = 0; i < 3; i++) {
      int x = 270 + i * 70;
      tft.fillCircle(x, 17, 5, ok[i] ? C_GREEN : C_RED);
      tft.setTextColor(C_MUTED, C_BG);
      tft.drawString(names[i], x + 10, 9, 2);
    }
  }

  // clock cards
  int curMin = m.now ? (localtime_r(&m.now, &lt), lt.tm_hour * 60 + lt.tm_min) : -1;
  if (curMin != lastMin) { lastMin = curMin; drawCards(curMin / 60, curMin % 60, curMin >= 0); }

  // status panel + buttons
  char title[96], sub[120];
  statusLines(m, title, sub, sizeof(title));
  char status[200];
  snprintf(status, sizeof(status), "%s|%s|%d|%d|%d|%.0f|%d|%s|%d", title, sub, m.alarmFlash, m.probe, m.probe ? m.probeLeftS : 0,
           m.holdFrac < 0 ? -1.0f : m.holdFrac * 20, m.awaitingAwakeTap, m.message ? m.message : "", (int)m.state);
  if (strcmp(status, lastStatus) == 0 && !force) return;
  strcpy(lastStatus, status);

  nBtns = 0;
  uint16_t panel = m.alarmFlash ? C_ORANGE : (m.state == ST_NAG ? C_RED : C_PANEL);
  if (m.probe) panel = C_BLUE;
  uint16_t ink = (panel == C_PANEL) ? C_TEXT : TFT_BLACK;
  tft.fillRect(0, 190, 480, 130, panel);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(ink, panel);
  if (m.probe) {
    char p[48];
    snprintf(p, sizeof(p), "Still awake? Tap!  (%d s)", m.probeLeftS);
    tft.drawString(p, 14, 198, 4);
  } else {
    tft.drawString(title, 14, 198, 4);
  }
  tft.setTextColor(panel == C_PANEL ? C_MUTED : TFT_BLACK, panel);
  tft.drawString(m.message ? m.message : sub, 14, 230, 2);

  // progress bar: early-unlock hold, or minutes up
  float frac = m.holdFrac >= 0 ? m.holdFrac : (m.state == ST_UP ? m.upMin / UP_REQUIRED_MIN : -1);
  if (frac >= 0) {
    if (frac > 1) frac = 1;
    tft.drawRoundRect(14, 252, 452, 12, 4, ink);
    tft.fillRoundRect(16, 254, (int)(448 * frac), 8, 3, m.holdFrac >= 0 ? C_RED : C_GREEN);
  }

  // buttons (bottom row)
  const int by = 272, bh = 42;
  int x = 14;
  if (m.dataMode) {
    if (m.state == ST_ALARM || m.probe)
      { button(x, by, 200, bh, "I'M AWAKE", C_GREEN, TFT_BLACK, UA_AWAKE); x += 210; }
    else if (m.state == ST_UP)
      { button(x, by, 200, bh, "BACK IN BED", C_CARD, C_TEXT, UA_BACK_IN_BED); x += 210; }
    else if (m.state == ST_IDLE || m.state == ST_NAG || m.state == ST_LOCKED || m.state == ST_SLEEPING)
      { button(x, by, 160, bh, "BED", C_CARD, C_TEXT, UA_BED); x += 170; }
  }
  button(346, by, 120, bh, "SET", C_CARD, C_TEXT, UA_OPEN_SETTINGS);
}

static void drawLockPrompt(const UiModel& m) {
  tft.fillScreen(C_BG);
  nBtns = 0;
  char w[16], line[64];
  fmtTime(m.wake, w, sizeof(w));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_ORANGE, C_BG);
  tft.drawString("Phone inserted", 240, 70, 4);
  tft.setTextColor(C_TEXT, C_BG);
  snprintf(line, sizeof(line), "Lock it until %s ?", w);
  tft.drawString(line, 240, 120, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("It is before bedtime. The button also ejects.", 240, 160, 2);
  button(40, 220, 180, 60, "LOCK", C_ORANGE, TFT_BLACK, UA_LOCK_NOW);
  button(260, 220, 180, 60, "EJECT", C_CARD, C_TEXT, UA_EJECT);
}

void uiRender(const UiModel& m) {
  bool force = fullRedraw;
  fullRedraw = false;
  switch (screen) {
    case SCR_SETTINGS: if (force) drawSettings(false); break;
    case SCR_SETUP:    if (force) drawSettings(true); break;
    case SCR_LOCK_PROMPT: if (force) drawLockPrompt(m); break;
    default:           drawHome(m, force); break;
  }
}
