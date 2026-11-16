// app.cpp — the night: state machine, lock/unlock, alarm, data-collection labels, summary.
//
//  IDLE --(bedtime window, tray empty)--> NAG --(tray down)--> LOCKED --(AI: asleep x3)--> SLEEPING
//  LOCKED/SLEEPING --(wake time)--> ALARM --(up)--> UP --(30 min up in total)--> UNLOCKED --(tray up)--> IDLE
//  UP --(asleep / lying in bed 2 min)--> ALARM again          NAG --(wake time, no phone)--> ALARM
//  Locked states: hold the button 2 min = early unlock; tray popping up without a command = emergency pin.
//
// "Asleep / in bed / up" comes from the on-device AI, or — in data-collection mode — from the labels
// the user gives (tap I'M AWAKE, missed probe, button = I'm up). Those labels are also uploaded, so
// the same nights become AI training data and an accuracy test set.
#include "shared.h"
#include "ui.h"
#include <WiFi.h>
#include <Preferences.h>

AppStatus gApp;
static Preferences prefs;

// ---------- night/session state (persisted) ----------
struct Persist {
  uint8_t magic, state, phoneLocked, silenced, ringDay, backToSleep, unlockReason, pad;
  uint32_t wakeAt, firstUpAt;
  float upSec;                      // float: it grows by ~0.02 s per tick
};
#define PERSIST_MAGIC 0x5B
static Persist P;

static uint32_t stateSinceMs = 0;
static uint32_t lastTickMs = 0;
static uint32_t lastPersistMs = 0;

// unlock bookkeeping
enum UnlockReason : uint8_t { UR_NONE, UR_DONE, UR_EARLY, UR_EMERGENCY, UR_EJECT };
static bool releasePending = false;
static uint8_t releaseTries = 0;
static uint32_t releaseStartMs = 0;
static const char* uiMessage = nullptr;

// inputs
static uint32_t btnDownMs = 0;          // 0 = not pressed
static uint32_t knobBtnDownMs = 0;
static bool prevTray = false;

// AI / label bookkeeping
static uint32_t lastAiAt = 0;
static uint8_t asleepStreak = 0;
static float inBedSec = 0;
static bool nodeWarned = false, nagNotified = false;
static uint32_t ringPhaseStartMs = 0;

// data mode
static WakeClass label = WC_UNKNOWN;
static time_t bedAt = 0;
static bool awaitingAwakeTap = false;
static bool probeActive = false;
static uint32_t probeDeadlineMs = 0, nextProbeMs = 0;

const char* stateName(AppState s) {
  static const char* n[] = {"IDLE", "NAG", "LOCKED", "SLEEPING", "ALARM", "UP", "UNLOCKED"};
  return s < ST__COUNT ? n[s] : "?";
}
const char* wakeClassName(WakeClass c) {
  switch (c) { case WC_ASLEEP: return "ASLEEP"; case WC_AWAKE_IN_BED: return "AWAKE_IN_BED"; case WC_UP: return "UP"; default: return "UNKNOWN"; }
}

static AppState st() { return (AppState)P.state; }
static bool lockedState(AppState s) { return s == ST_LOCKED || s == ST_SLEEPING || s == ST_ALARM || s == ST_UP; }

static void persist() {
  P.magic = PERSIST_MAGIC;
  prefs.putBytes("p", &P, sizeof(P));
  lastPersistMs = millis();
}

static void hhmm(time_t t, char* out, size_t n) {
  if (!t) { snprintf(out, n, "--:--"); return; }
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

static void setState(AppState s) {
  if (s == st()) return;
  Serial.printf("[APP] %s -> %s\n", stateName(st()), stateName(s));
  P.state = s;
  stateSinceMs = millis();
  persist();
  postEvent("state", stateName(s));
  if (s != ST_NAG) buzzer(BZ_OFF);
  if (s == ST_ALARM) ringPhaseStartMs = millis();
}

static void labelEvent(const char* what, WakeClass newLabel) {
  label = newLabel;
  postEvent("label", what);
  Serial.printf("[LABEL] %s\n", what);
}

// ---------- session start / end ----------
static void startNight(time_t wake, bool ring, bool phoneIn) {
  P.wakeAt = (uint32_t)wake;
  P.ringDay = ring;
  P.phoneLocked = phoneIn;
  P.silenced = 0;
  P.upSec = 0;
  P.firstUpAt = 0;
  P.backToSleep = 0;
  P.unlockReason = UR_NONE;
  nodeWarned = nagNotified = false;
  asleepStreak = 0;
  inBedSec = 0;
}

static void endNight(UnlockReason why) {
  if (P.wakeAt && (st() == ST_ALARM || st() == ST_UP || st() == ST_UNLOCKED || why == UR_EARLY || why == UR_EMERGENCY)) {
    char w[8], f[8], u[8], msg[300];
    hhmm(P.wakeAt, w, sizeof(w));
    hhmm(P.firstUpAt, f, sizeof(f));
    hhmm(nowUtc(), u, sizeof(u));
    const char* how = why == UR_DONE ? "ครบ 30 นาที" : why == UR_EARLY ? "กดค้างปลดก่อนเวลา" :
                      why == UR_EMERGENCY ? "ใช้เข็มปลดฉุกเฉิน" : "-";
    snprintf(msg, sizeof(msg), "☀️ **Doomclock สรุปเช้านี้** — ตั้งปลุก %s · ลุกครั้งแรก %s · กลับไปนอน %d ครั้ง · ปลดล็อก %s (%s)",
             w, f, P.backToSleep, u, how);
    postNotify(msg);
  }
  P.wakeAt = 0;
  P.phoneLocked = 0;
  probeActive = awaitingAwakeTap = false;
  label = WC_UNKNOWN;
  bedAt = 0;
  setState(ST_IDLE);
  persist();
}

static void beginRelease(UnlockReason why) {
  P.unlockReason = why;
  releasePending = true;
  releaseTries = 1;
  releaseStartMs = millis();
  servoRelease();
  if (why != UR_EJECT) setState(ST_UNLOCKED);
}

// ---------- the current "asleep / in bed / up" signal ----------
static WakeClass currentClass(bool* fresh) {
  AiState ai;
  uint32_t bedRx;
  lockState(); ai = gAi; bedRx = gBed.rxMs; unlockState();
  bool nodeFresh = bedRx && millis() - bedRx < BED_FRESH_MS;
  if (fresh) *fresh = nodeFresh;
  if (gSettings.dataMode) return label;
  return nodeFresh ? ai.cls : WC_UNKNOWN;
}

float wakeRelMinutes(time_t t) {
  time_t ref;
  lockState(); ref = gApp.nextWake; unlockState();
  if (!ref || !timeValid()) return NAN;
  return (float)((double)t - (double)ref) / 60.0f;
}

// ---------- inputs ----------
static void handleInputs(UiAction& tapped) {
  InputEv e;
  while (xQueueReceive(gInputQ, &e, 0) == pdTRUE) {
    switch (e.kind) {
      case IN_KNOB:
        if (uiScreen() == SCR_SETTINGS || uiScreen() == SCR_SETUP) { uiKnob(e.delta); break; }
        if (!timeValid()) break;
        if (st() == ST_IDLE) nudgeNextWake(nowUtc(), e.delta * KNOB_STEP_MIN);
        else if (st() == ST_NAG || st() == ST_LOCKED || st() == ST_SLEEPING) {
          long w = (long)P.wakeAt + e.delta * KNOB_STEP_MIN * 60;
          if (w > (long)nowUtc() + 60) { P.wakeAt = (uint32_t)w; persist(); }
        }
        buzzer(BZ_CLICK);
        break;
      case IN_KNOB_DOWN: knobBtnDownMs = e.ms; break;
      case IN_KNOB_UP:
        if (knobBtnDownMs && e.ms - knobBtnDownMs > 30) {
          if (uiScreen() == SCR_SETTINGS || uiScreen() == SCR_SETUP) uiKnobPress();
          else if (uiScreen() == SCR_HOME) uiShow(SCR_SETTINGS);
          buzzer(BZ_CLICK);
        }
        knobBtnDownMs = 0;
        break;
      case IN_BTN_DOWN: if (!btnDownMs) btnDownMs = e.ms ? e.ms : 1; break;
      case IN_BTN_UP: {
        if (!btnDownMs) break;
        uint32_t held = e.ms - btnDownMs;
        btnDownMs = 0;
        if (held < 30 || held > 3000) break;                  // bounce, or the end of a long hold
        // short press
        bool tray; lockState(); tray = gSens.trayDown; unlockState();
        if (uiScreen() == SCR_LOCK_PROMPT || (st() == ST_IDLE && tray)) { tapped = UA_EJECT; break; }
        if (gSettings.dataMode && (st() == ST_ALARM || st() == ST_UP || st() == ST_LOCKED || st() == ST_SLEEPING))
          tapped = (UiAction)250;                              // "I'm up" label (handled below)
        break;
      }
    }
  }
}

// ---------- serial commands (run inside TaskApp to keep one owner for servo/buzzer) ----------
static volatile char pendingCmd = 0;
void appCommand(char c) { pendingCmd = c; }

static void runCommand(char c) {
  time_t now = timeValid() ? nowUtc() : 0;
  if (c == 't') { Serial.println("[APP] touch calibration"); uiCalibrate(); uiShow(settingsValid() ? SCR_HOME : SCR_SETUP); }
  else if (c == 'v') { Serial.println("[APP] servo release test"); servoRelease(); }
  else if (c == 'b') { Serial.println("[APP] buzzer test"); buzzer(BZ_ALARM); }
  else if (c == 'A' && now) {
    time_t w = now + 120;
    if (st() == ST_IDLE) {
      bool r; nudgeNextWake(now, (int)((w - nextWake(now, &r)) / 60));
      Serial.println("[APP] demo: next wake moved to ~2 min from now (only if within 12 h of the schedule)");
    } else if (st() == ST_NAG || st() == ST_LOCKED || st() == ST_SLEEPING) {
      P.wakeAt = (uint32_t)w; persist();
      Serial.println("[APP] demo: tonight's wake moved to 2 min from now");
    }
  }
}

// ---------- main tick ----------
static UiModel model;

static void tick() {
  uint32_t ms = millis();
  float dt = (ms - lastTickMs) / 1000.0f;
  lastTickMs = ms;
  if (dt > 2) dt = 2;

  bool tray; lockState(); tray = gSens.trayDown; unlockState();
  time_t now = timeValid() ? nowUtc() : 0;

  if (pendingCmd) { char c = pendingCmd; pendingCmd = 0; runCommand(c); }
  UiAction tap = uiPollTouch();
  handleInputs(tap);

  // ---- settings / calibration actions ----
  if (tap == UA_TOUCH_CAL) { uiCalibrate(); uiShow(SCR_SETTINGS); }
  if (tap == UA_SAVED) { buzzer(BZ_OK); postEvent("settings", gSettings.dataMode ? "saved (data mode on)" : "saved"); }

  bool fresh;
  WakeClass cls = currentClass(&fresh);
  bool newAi = false;
  { uint32_t at; lockState(); at = gAi.atMs; unlockState(); if (at != lastAiAt) { lastAiAt = at; newAi = true; } }

  // ---- data-mode labels from taps / button ----
  if (gSettings.dataMode) {
    if (tap == UA_BED) { bedAt = now; labelEvent("BED", WC_AWAKE_IN_BED); buzzer(BZ_OK); }
    if (bedAt && label == WC_AWAKE_IN_BED && now && now - bedAt >= 30 * 60 && (st() == ST_LOCKED || st() == ST_SLEEPING || st() == ST_IDLE || st() == ST_NAG))
      labelEvent("ASLEEP_ASSUMED", WC_ASLEEP);                // lights out + 30 min
    if (tap == UA_AWAKE) {
      labelEvent("AWAKE", WC_AWAKE_IN_BED);
      awaitingAwakeTap = false;
      probeActive = false;
      P.silenced = 1;
      nextProbeMs = ms + PROBE_INTERVAL_S * 1000UL;
      buzzer(BZ_OK);
    }
    if (tap == UA_BACK_IN_BED && st() == ST_UP) {
      labelEvent("BACK_IN_BED", WC_AWAKE_IN_BED);
      P.backToSleep++;
      P.silenced = 1;
      nextProbeMs = ms + PROBE_INTERVAL_S * 1000UL;
      setState(ST_ALARM);
    }
    if ((int)tap == 250) { labelEvent("UP", WC_UP); buzzer(BZ_OK); }
    // probes while awake in bed after the alarm
    if (st() == ST_ALARM && label == WC_AWAKE_IN_BED && !probeActive && nextProbeMs && (int32_t)(ms - nextProbeMs) >= 0) {
      probeActive = true;
      probeDeadlineMs = ms + PROBE_WINDOW_S * 1000UL;
      buzzer(BZ_PROBE);
      postEvent("label", "PROBE");
    }
    if (probeActive && (int32_t)(ms - probeDeadlineMs) >= 0) {     // no answer -> asleep again
      probeActive = false;
      labelEvent("PROBE_MISS", WC_ASLEEP);
      P.silenced = 0;
      P.backToSleep++;
      ringPhaseStartMs = ms;
      nextProbeMs = 0;
    }
    cls = label;
  }

  // ---- lock prompt (tray down before bedtime) ----
  if (uiScreen() == SCR_LOCK_PROMPT) {
    if (tap == UA_LOCK_NOW && now) {
      bool ring; time_t w = nextWake(now, &ring);
      startNight(w, ring, true);
      setState(ST_LOCKED);
      uiShow(SCR_HOME);
    } else if (tap == UA_EJECT || !tray) {
      if (tray) beginRelease(UR_EJECT);
      uiShow(SCR_HOME);
    }
  } else if (tap == UA_EJECT && st() == ST_IDLE && tray) {
    beginRelease(UR_EJECT);
  }

  // ---- early unlock: hold 2 min ----
  float holdFrac = -1;
  if (btnDownMs && lockedState(st()) && P.phoneLocked) {
    uint32_t held = ms - btnDownMs;
    holdFrac = held / (float)EARLY_UNLOCK_HOLD_MS;
    if (held >= EARLY_UNLOCK_HOLD_MS) {
      btnDownMs = 0;
      postEvent("earlyUnlock", "held button 2 min");
      postNotify("🔓 **Doomclock:** ปลดล็อกก่อนเวลา (กดปุ่มค้าง 2 นาที)");
      beginRelease(UR_EARLY);
    }
  }

  // ---- release follow-up: tray must come up ----
  if (releasePending) {
    if (!tray) {
      releasePending = false;
      uiMessage = nullptr;
      buzzer(BZ_OK);
      if (P.unlockReason != UR_EJECT) endNight((UnlockReason)P.unlockReason);
    } else if (!servoBusy() && ms - releaseStartMs > RELEASE_CONFIRM_MS) {
      if (releaseTries < RELEASE_RETRIES) { releaseTries++; releaseStartMs = ms; servoRelease(); }
      else {
        releasePending = false;
        uiMessage = "Release failed - use the emergency pin";
        buzzer(BZ_ERR);
        postEvent("releaseFailed", "tray did not come up");
        postNotify("⚠️ **Doomclock:** สั่งปลดล็อกแล้วแต่ถาดไม่เด้ง ต้องใช้เข็มปลดฉุกเฉิน");
      }
    }
  }

  // ---- emergency pin: tray came up while locked and nobody asked ----
  if (prevTray && !tray && !releasePending && lockedState(st()) && P.phoneLocked) {
    postEvent("emergencyUnlock", "tray up without a release command");
    postNotify("🚨 **Doomclock:** ถาดเด้งออกโดยไม่ได้สั่ง (ใช้เข็มปลดฉุกเฉิน)");
    endNight(UR_EMERGENCY);
  }

  // ---- state machine ----
  AppState s = st();
  time_t idleWake = 0;
  bool idleRing = true;
  if (now && settingsValid()) idleWake = nextWake(now, &idleRing);

  switch (s) {
    case ST_IDLE: {
      if (!now || !settingsValid() || uiScreen() != SCR_HOME) break;
      time_t nagStart = idleWake - (time_t)(gSettings.sleepGoalMin + NAG_MARGIN_MIN) * 60;
      bool inWindow = now >= nagStart && now < idleWake;
      if (!prevTray && tray && !releasePending) {
        if (inWindow) { startNight(idleWake, idleRing, true); setState(ST_LOCKED); }
        else uiShow(SCR_LOCK_PROMPT);
      } else if (inWindow && !tray) {
        startNight(idleWake, idleRing, false);
        setState(ST_NAG);
        buzzer(BZ_NAG);
      } else if (inWindow && tray && !releasePending) {
        startNight(idleWake, idleRing, true);                 // tray was already down when the window opened
        setState(ST_LOCKED);
      }
      break;
    }
    case ST_NAG:
      if (!nagNotified) {
        nagNotified = true;
        char w[8], msg[160];
        hhmm(P.wakeAt, w, sizeof(w));
        snprintf(msg, sizeof(msg), "📵 **Doomclock:** ถึงเวลาเข้านอนแล้ว (ปลุก %s) แต่ยังไม่ได้ใส่มือถือ", w);
        postNotify(msg);
      }
      if (tray) { P.phoneLocked = 1; setState(ST_LOCKED); }
      else if (now >= (time_t)P.wakeAt) setState(ST_ALARM);
      break;
    case ST_LOCKED:
      if (newAi) asleepStreak = (cls == WC_ASLEEP) ? asleepStreak + 1 : 0;
      if (asleepStreak >= ASLEEP_CONFIRM_WIN) setState(ST_SLEEPING);
      if (now >= (time_t)P.wakeAt) setState(ST_ALARM);
      break;
    case ST_SLEEPING:
      if (now >= (time_t)P.wakeAt) setState(ST_ALARM);
      break;
    case ST_ALARM:
      if (gSettings.dataMode && label != WC_AWAKE_IN_BED && label != WC_UP && !probeActive) awaitingAwakeTap = true;
      if (cls == WC_UP) {
        if (!P.firstUpAt) P.firstUpAt = (uint32_t)now;
        inBedSec = 0;
        probeActive = false;
        setState(ST_UP);
      }
      break;
    case ST_UP:
      if (cls == WC_UP) { P.upSec += dt; inBedSec = 0; }
      else if (!gSettings.dataMode && (cls == WC_ASLEEP || cls == WC_AWAKE_IN_BED)) {
        inBedSec += dt;
        if (inBedSec >= BACK_TO_BED_S) { P.backToSleep++; P.silenced = 0; setState(ST_ALARM); }
      }
      if (P.upSec >= UP_REQUIRED_MIN * 60) {
        if (P.phoneLocked && tray) beginRelease(UR_DONE);
        else endNight(UR_DONE);
      }
      if (ms - lastPersistMs > 60000) persist();
      break;
    case ST_UNLOCKED:
      if (!releasePending && !tray) endNight((UnlockReason)P.unlockReason);
      break;
    default: break;
  }

  // ---- alarm sound: 10 min on / 5 min off, silenced after "I'm awake" in data mode ----
  bool ringing = false;
  if (st() == ST_ALARM && P.ringDay && !P.silenced) {
    uint32_t phase = ((ms - ringPhaseStartMs) / 1000) % (RING_ON_S + RING_OFF_S);
    ringing = phase < RING_ON_S;
  }
  static bool wasRinging = false;
  if (ringing != wasRinging) { buzzer(ringing ? BZ_ALARM : BZ_OFF); wasRinging = ringing; }
  else if (ringing && buzzerIdle()) buzzer(BZ_ALARM);              // a UI click interrupted the alarm
  if (st() == ST_NAG && buzzerIdle()) buzzer(BZ_NAG);

  // ---- node silent during a locked night ----
  if (lockedState(st())) {
    uint32_t rx; lockState(); rx = gBed.rxMs; unlockState();
    if (!nodeWarned && (!rx || ms - rx > NODE_STALE_WARN_S * 1000UL) && ms - stateSinceMs > NODE_STALE_WARN_S * 1000UL) {
      nodeWarned = true;
      postNotify("⚠️ **Doomclock:** ไม่ได้รับข้อมูลจาก bed node ระหว่างล็อก ระบบตัดสินการตื่นไม่ได้ (กดปุ่มค้าง 2 นาทีเพื่อปลดได้)");
    }
  }

  prevTray = tray;

  // ---- publish status for records / other tasks ----
  time_t ref = (st() == ST_IDLE) ? idleWake : (time_t)P.wakeAt;
  lockState();
  gApp.state = st();
  gApp.label = label;
  gApp.dataMode = gSettings.dataMode;
  gApp.nextWake = ref;
  gApp.upMin = P.upSec / 60.0f;
  gApp.wakeRelMin = (ref && now) ? (float)((double)now - (double)ref) / 60.0f : NAN;
  AiState ai = gAi;
  unlockState();

  // ---- screen ----
  model.now = now;
  model.state = st();
  model.wake = ref;
  model.ringDay = (st() == ST_IDLE) ? idleRing : P.ringDay;
  model.upMin = P.upSec / 60.0f;
  model.holdFrac = holdFrac;
  model.alarmFlash = ringing && ((ms / 500) % 2 == 0);
  model.probe = probeActive;
  model.probeLeftS = probeActive ? (int)((probeDeadlineMs - ms) / 1000) : 0;
  model.awaitingAwakeTap = awaitingAwakeTap && st() == ST_ALARM;
  model.dataMode = gSettings.dataMode;
  model.label = label;
  model.ai = ai.cls;
  model.aiTrained = ai.trained;
  model.wifi = WiFi.status() == WL_CONNECTED;
  model.node = fresh;
  model.cloud = gCloudOk;
  model.phoneLocked = P.phoneLocked && lockedState(st());
  model.message = uiMessage;
  uiRender(model);
}

void appBegin() {
  prefs.begin("dcapp", false);
  memset(&P, 0, sizeof(P));
  if (prefs.getBytesLength("p") == sizeof(P)) prefs.getBytes("p", &P, sizeof(P));
  if (P.magic != PERSIST_MAGIC || P.state >= ST__COUNT) memset(&P, 0, sizeof(P));
  // after a power cut: keep a locked night locked; a half-done release is retried
  if (P.state == ST_NAG) P.state = ST_IDLE;
  if (P.state == ST_UNLOCKED) { releasePending = true; releaseTries = 1; releaseStartMs = millis(); servoRelease(); }
  if (lockedState((AppState)P.state)) Serial.printf("[APP] restored %s after reboot (wake %lu)\n", stateName((AppState)P.state), (unsigned long)P.wakeAt);
  lockState(); prevTray = gSens.trayDown; unlockState();
  stateSinceMs = lastTickMs = millis();
  ringPhaseStartMs = millis();
}

void TaskApp(void*) {
  uiBegin();                                                 // no forced touch calibration: defaults until 't' / Settings -> Touch cal
  uiShow(settingsValid() ? SCR_HOME : SCR_SETUP);           // first boot: the week must be set
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
    buzzerTick();
    servoTick();
    tick();
  }
}
