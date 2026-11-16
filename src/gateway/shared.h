// shared.h — state shared between gateway tasks, and the functions each module exposes.
#pragma once
#include <Arduino.h>
#include <time.h>
#include "doom_protocol.h"
#include "config.h"

struct GatewaySensors {
  float tempC = NAN, hum = NAN, lux = NAN;
  int hallRaw = 0;
  bool hallDo = false;
  bool trayDown = false;          // "phoneInserted" on the dashboard
};

struct BedState {
  BedPacket last{};
  bool have = false;
  uint32_t rxMs = 0;              // millis() when received
  time_t windowEnd = 0;           // UTC time the window ended (rx time - age_s)
};

struct LinkStats {
  uint32_t rx = 0;                // unique packets
  uint32_t dup = 0;               // duplicates (ACK was lost, node resent)
  uint32_t lost = 0;              // seq gaps
  uint32_t bad = 0;               // wrong size / CRC / version
  uint16_t bootId = 0;
  uint32_t lastSeq = 0;
  bool online = false;
};

// ---- night logic types ----
enum AppState : uint8_t { ST_IDLE, ST_NAG, ST_LOCKED, ST_SLEEPING, ST_ALARM, ST_UP, ST_UNLOCKED, ST__COUNT };
enum WakeClass : int8_t { WC_UNKNOWN = -1, WC_ASLEEP = 0, WC_AWAKE_IN_BED = 1, WC_UP = 2 };
const char* stateName(AppState s);
const char* wakeClassName(WakeClass c);

struct AiState {
  WakeClass cls = WC_UNKNOWN;     // smoothed decision used by the state machine
  WakeClass raw = WC_UNKNOWN;     // this window's prediction
  float conf = 0;                 // probability of `raw`
  bool trained = false;           // false = placeholder heuristic, not a trained model
  uint32_t atMs = 0;              // millis() of the last prediction
};

struct AppStatus {                // written by TaskApp, read by record.cpp
  AppState state = ST_IDLE;
  WakeClass label = WC_UNKNOWN;   // ground-truth label in data-collection mode
  bool dataMode = false;
  float wakeRelMin = NAN;         // minutes since this night's wake time (negative before it)
  float upMin = 0;                // cumulative minutes "up" this morning
  time_t nextWake = 0;
};

// ---- shared data (guard with lockState/unlockState) ----
extern GatewaySensors gSens;
extern BedState gBed;
extern LinkStats gLink;
extern AiState gAi;
extern AppStatus gApp;
extern volatile uint32_t gLastBedPostMs;
extern SemaphoreHandle_t gStateMutex;
extern SemaphoreHandle_t gI2cMutex;
inline void lockState()   { xSemaphoreTake(gStateMutex, portMAX_DELAY); }
inline void unlockState() { xSemaphoreGive(gStateMutex); }

// ---- queues ----
struct CloudMsg  { char json[1300]; };   // worst case record is ~1150 chars
struct NotifyMsg { char text[400]; };
extern QueueHandle_t gCloudQ, gNotifyQ;

// ---- record.cpp ----
// Builds one NETPIE shadow record: gateway sensors + app state + (optionally) the latest bed window.
size_t buildRecord(char* out, size_t cap, const char* action, const char* note, bool withBed);
void postEvent(const char* action, const char* note = "");
void postBedRecord();
void postNotify(const char* text);

// ---- timekeeping.cpp ----
void clockBegin(bool i2cOk);       // DS3231 -> system clock at boot (skipped if the I2C bus is broken)
void clockService();               // NTP -> DS3231 once synced (call from cloud task)
bool timeValid();
time_t nowUtc();
void isoLocal(time_t t, char* out, size_t cap);

// ---- settings.cpp: 7-day wake schedule + sleep goal, kept in NVS ----
struct DaySlot { uint8_t hour, minute, ring; };   // ring 1 = sound, 0 = silent (AI still decides, phone still jailed)
struct Settings {
  uint8_t magic;                   // SETTINGS_MAGIC once the first-boot setup was saved
  DaySlot day[7];                  // index = tm_wday (0 = Sunday)
  uint16_t sleepGoalMin;           // target sleep, minutes
  uint8_t dataMode;                // 1 = data-collection (labelling) mode
};
#define SETTINGS_MAGIC 0xD7
extern Settings gSettings;
void settingsBegin();
bool settingsValid();
void settingsSave();
time_t scheduledWakeAfter(time_t from, bool* ring);   // next scheduled wake strictly after `from`
time_t nextWake(time_t now, bool* ring);              // includes the one-shot knob override
void nudgeNextWake(time_t now, int minutes);          // knob: move only the next wake
void clearWakeOverride();

// ---- io.cpp: knob + button (ISR, E2), buzzer, servo ----
enum InKind : uint8_t { IN_KNOB, IN_KNOB_DOWN, IN_KNOB_UP, IN_BTN_DOWN, IN_BTN_UP };
struct InputEv { uint8_t kind; int8_t delta; uint32_t ms; };
extern QueueHandle_t gInputQ;
void ioBegin();
enum BuzzPattern : uint8_t { BZ_OFF, BZ_ALARM, BZ_NAG, BZ_PROBE, BZ_CLICK, BZ_OK, BZ_ERR };
void buzzer(BuzzPattern p);        // start a pattern (looping ones keep going until BZ_OFF)
void buzzerTick();                 // call every ~20 ms
bool buzzerIdle();
void servoRelease();               // pull the latch, return, stop PWM
void servoTick();
bool servoBusy();

// ---- ai.cpp: on-device classifier (E7) ----
void aiPushWindow(const BedPacket& p, time_t windowEnd, float lux, float wakeRelMin);
void aiMarkStale();

// ---- app.cpp / ui.cpp ----
void appBegin();
void TaskApp(void*);
float wakeRelMinutes(time_t t);    // minutes relative to the current night's wake time
void appCommand(char c);           // serial: 'A' demo wake in 2 min, 'v' servo test, 'b' buzzer test

// ---- link.cpp ----
void linkBegin();
void TaskLink(void*);

// ---- cloud.cpp ----
void cloudBegin();
void TaskCloud(void*);
extern volatile bool gCloudOk;
extern volatile uint32_t gOfflineStored, gOfflineDropped;

// ---- notify.cpp ----
void TaskNotify(void*);
