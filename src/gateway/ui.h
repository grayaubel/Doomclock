// ui.h — touch screen (ILI9488 480x320 + XPT2046), owned by TaskApp only (one task touches SPI).
#pragma once
#include "shared.h"

enum UiScreen : uint8_t { SCR_HOME, SCR_SETTINGS, SCR_SETUP, SCR_LOCK_PROMPT };

enum UiAction : uint8_t {
  UA_NONE,
  UA_OPEN_SETTINGS,
  UA_SAVED,           // settings were saved (gSettings + NVS updated)
  UA_CANCEL,
  UA_LOCK_NOW,        // lock prompt: lock until the next wake
  UA_EJECT,           // lock prompt: give the phone back
  UA_AWAKE,           // data mode: "I'm awake" / answered a probe
  UA_BED,             // data mode: lights out, going to sleep
  UA_BACK_IN_BED,     // data mode: got back into bed after getting up
  UA_TOUCH_CAL,
};

struct UiModel {
  time_t now = 0;                 // 0 = time unknown
  AppState state = ST_IDLE;
  time_t wake = 0;                // this night's / next wake
  bool ringDay = true;
  float upMin = 0;
  float holdFrac = -1;            // early-unlock hold progress 0..1, -1 = not held
  bool alarmFlash = false;        // ALARM state and currently ringing
  bool probe = false;             // data mode: waiting for a tap
  int probeLeftS = 0;
  bool awaitingAwakeTap = false;  // data mode: alarm not yet acknowledged
  bool dataMode = false;
  WakeClass label = WC_UNKNOWN;
  WakeClass ai = WC_UNKNOWN;
  bool aiTrained = false;
  bool wifi = false, node = false, cloud = false;
  bool phoneLocked = false;
  const char* message = nullptr;  // one-line notice (e.g. "Release failed")
};

void uiBegin();
bool uiHasCalibration();
void uiCalibrate();                       // blocking, stores the result in NVS
void uiShow(UiScreen s);                  // switch screen (settings/setup take a working copy)
UiScreen uiScreen();
void uiKnob(int delta);                   // settings: adjust the selected field
void uiKnobPress();                       // settings: next field
UiAction uiPollTouch();                   // call every tick; returns the tapped action
void uiRender(const UiModel& m);          // redraws only what changed
