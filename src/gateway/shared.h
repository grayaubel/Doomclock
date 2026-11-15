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

// ---- shared data (guard with lockState/unlockState) ----
extern GatewaySensors gSens;
extern BedState gBed;
extern LinkStats gLink;
extern volatile uint32_t gLastBedPostMs;
extern SemaphoreHandle_t gStateMutex;
extern SemaphoreHandle_t gI2cMutex;
inline void lockState()   { xSemaphoreTake(gStateMutex, portMAX_DELAY); }
inline void unlockState() { xSemaphoreGive(gStateMutex); }

// ---- queues ----
struct CloudMsg  { char json[1100]; };   // worst case record is ~1000 chars
struct NotifyMsg { char text[400]; };
extern QueueHandle_t gCloudQ, gNotifyQ;

// ---- record.cpp ----
// Builds one NETPIE shadow record: gateway sensors + (optionally) the latest bed window.
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
