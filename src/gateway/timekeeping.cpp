// timekeeping.cpp — one clock for the whole gateway: the ESP32 system clock (UTC),
// seeded from the DS3231 at boot (works with no internet) and corrected by NTP.
#include "shared.h"
#include <Wire.h>
#include <RTClib.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include <Preferences.h>
#include "build_time.h"                      // BUILD_UNIX, written by scripts/build_time.py on every build

#define UPLOAD_LAG_S 20                      // build -> link -> upload -> boot takes roughly this long

static RTC_DS3231 rtc;
static bool rtcOk = false;
static volatile bool ntpFresh = false;      // set by the SNTP callback, consumed by clockService()

static void onNtpSync(struct timeval*) { ntpFresh = true; }

void clockBegin(bool i2cOk) {
  setenv("TZ", "ICT-7", 1);                  // Asia/Bangkok, no DST
  tzset();
  rtcOk = i2cOk && rtc.begin(&Wire);
  time_t rtcT = 0;
  if (rtcOk && !rtc.lostPower()) {
    DateTime d = rtc.now();                  // the RTC stores UTC
    if (d.year() >= 2024) rtcT = (time_t)d.unixtime();
  }

  // First boot after an upload: the build time is a better guess than an RTC that is unset or
  // behind the build (it can't be earlier than the firmware). Only once per firmware, so a later
  // reboot without an RTC doesn't jump back to the build time. NTP corrects it when online.
  Preferences tp;
  tp.begin("time", false);
  bool firstBoot = tp.getULong("build", 0) != BUILD_UNIX;
  if (firstBoot) tp.putULong("build", BUILD_UNIX);
  tp.end();
  time_t buildT = (time_t)BUILD_UNIX + UPLOAD_LAG_S;
  if (firstBoot && rtcT < buildT) {
    struct timeval tv = {buildT, 0};
    settimeofday(&tv, nullptr);
    if (rtcOk) rtc.adjust(DateTime((uint32_t)buildT));
    Serial.printf("[TIME] first boot of this firmware: clock set from build time (%s) — NTP will fine-tune it\n",
                  rtcT ? "RTC was behind" : "RTC unset");
  } else if (rtcT) {
    struct timeval tv = {rtcT, 0};
    settimeofday(&tv, nullptr);
  }
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime("ICT-7", "pool.ntp.org", "time.google.com");
  Serial.printf("[TIME] DS3231 %s, clock %s\n", rtcOk ? "ok" : "NOT FOUND", timeValid() ? "set from RTC" : "unknown until NTP");
}

void clockService() {
  if (!ntpFresh) return;
  ntpFresh = false;
  if (rtcOk && timeValid() && xSemaphoreTake(gI2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    rtc.adjust(DateTime((uint32_t)time(nullptr)));
    xSemaphoreGive(gI2cMutex);
    Serial.println("[TIME] NTP sync -> DS3231 updated");
  }
}

bool timeValid() { return time(nullptr) > 1700000000; }   // after Nov 2023
time_t nowUtc() { return time(nullptr); }

void isoLocal(time_t t, char* out, size_t cap) {
  struct tm lt;
  localtime_r(&t, &lt);
  strftime(out, cap, "%Y-%m-%dT%H:%M:%S+07:00", &lt);
}
