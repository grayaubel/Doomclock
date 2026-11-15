/*
 * Doomclock — GATEWAY (the clock)
 *
 * Stage 1 (checkpoint 20 Nov): data path node -> gateway -> NETPIE -> dashboard.
 *   Sensors here: BH1750 (light), DHT11 (temp/humidity), KY-024 Hall (phone tray down?), DS3231 (time)
 *   ESP-NOW from the bed node with ACK + duplicate/loss counting      (link.cpp)
 *   NETPIE over TLS with store-and-forward on LittleFS                (cloud.cpp)
 *   Discord alerts                                                    (notify.cpp)
 * Stage 2 (next): alarm schedule, state machine, servo, TFT UI, AI.
 *
 * FreeRTOS tasks (E1):
 *   TaskLink    prio 3  core 1  ESP-NOW rx + ACK — highest: the node waits only 150 ms for an ACK
 *   TaskSensors prio 2  core 1  BH1750 / DHT11 / Hall
 *   TaskCloud   prio 1  core 0  Wi-Fi, MQTT, offline queue, NTP->RTC
 *   TaskNotify  prio 0  core 0  Discord HTTPS (slow TLS handshakes must not starve anything)
 *   Arduino loop (prio 1): serial debug commands
 * Shared state is guarded by gStateMutex, the I2C bus by gI2cMutex; tasks talk via queues.
 *
 * Serial commands: s = stats   h = hall raw   c = calibrate hall (tray UP!)   d = test Discord
 */
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <BH1750.h>
#include <DHT.h>
#include <Preferences.h>
#include <esp_system.h>
#include "shared.h"
#include "secrets.h"

GatewaySensors gSens;
BedState gBed;
LinkStats gLink;
volatile uint32_t gLastBedPostMs = 0;
SemaphoreHandle_t gStateMutex, gI2cMutex;
QueueHandle_t gCloudQ, gNotifyQ;

static BH1750 lightMeter;
static DHT dht(PIN_DHT, DHT11);
static bool bhOk = false;
static Preferences prefs;
static int hallBaseline = HALL_BASELINE_DEF;

static int readHall() {
  int s = 0;
  for (int i = 0; i < 8; i++) s += analogRead(PIN_HALL_AO);
  return s / 8;
}

void TaskSensors(void*) {
  TickType_t last = xTaskGetTickCount();
  int tick = 0, deb = 0;
  bool tray = false;
  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(100));
    tick++;

    // ---- KY-024 every 100 ms, hysteresis + debounce ----
    int raw = readHall();
    int delta = abs(raw - hallBaseline);
    bool want = tray ? (delta > HALL_OFF_DELTA) : (delta > HALL_ON_DELTA);
    if (want != tray) { if (++deb >= HALL_DEBOUNCE) { tray = want; deb = 0; } } else deb = 0;
    bool trayChanged = false;
    lockState();
    gSens.hallRaw = raw;
    gSens.hallDo = digitalRead(PIN_HALL_DO);
    if (gSens.trayDown != tray) { gSens.trayDown = tray; trayChanged = true; }
    unlockState();
    if (trayChanged) postEvent(tray ? "phoneInserted" : "phoneRemoved", "");

    // ---- BH1750 every 1 s ----
    if (tick % 10 == 0 && bhOk && xSemaphoreTake(gI2cMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      float lux = lightMeter.readLightLevel();
      xSemaphoreGive(gI2cMutex);
      lockState(); gSens.lux = lux >= 0 ? lux : NAN; unlockState();
    }

    // ---- DHT11 every 2 s (sensor minimum is 1 s) ----
    if (tick % 20 == 5) {
      float t = dht.readTemperature(), h = dht.readHumidity();
      lockState();
      if (!isnan(t)) gSens.tempC = t;
      if (!isnan(h)) gSens.hum = h;
      unlockState();
    }
  }
}

static void printStats() {
  GatewaySensors s; BedState b; LinkStats l;
  lockState(); s = gSens; b = gBed; l = gLink; unlockState();
  Serial.printf("[STAT] T=%.1fC H=%.0f%% lux=%.0f hall=%d tray=%s | node %s rx=%lu lost=%lu dup=%lu bad=%lu | "
                "cloud=%s offline stored=%lu dropped=%lu | time %s\n",
                s.tempC, s.hum, s.lux, s.hallRaw, s.trayDown ? "DOWN" : "up",
                l.online ? "online" : "offline", (unsigned long)l.rx, (unsigned long)l.lost,
                (unsigned long)l.dup, (unsigned long)l.bad, gCloudOk ? "OK" : "down",
                (unsigned long)gOfflineStored, (unsigned long)gOfflineDropped, timeValid() ? "valid" : "unknown");
  if (b.have)
    Serial.printf("[BED ] seq=%lu moves=%u jerk=%u mg gyro=%u cdps tof=%u mm (%u/%u)\n",
                  (unsigned long)b.last.seq, b.last.move_count, b.last.jerk_mean_mg, b.last.gyro_mean_cdps,
                  b.last.tof_mean_mm, b.last.tof_valid, b.last.tof_n);
}

// Before starting I2C, look at the bare SDA/SCL lines. An idle I2C bus must read HIGH on both
// (pull-ups on the modules). LOW = wire touching GND or a module holding the line;
// SDA following SCL = the two lines are in the same breadboard strip (shorted).
static bool checkI2cLines() {
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, INPUT_PULLUP);
  delay(5);
  int sda = digitalRead(PIN_SDA), scl = digitalRead(PIN_SCL);
  pinMode(PIN_SCL, OUTPUT);
  digitalWrite(PIN_SCL, LOW);
  delayMicroseconds(50);
  bool shorted = sda && digitalRead(PIN_SDA) == LOW;
  pinMode(PIN_SCL, INPUT_PULLUP);
  Serial.printf("[I2C] idle lines: SDA(21)=%s SCL(22)=%s%s\n", sda ? "HIGH" : "LOW", scl ? "HIGH" : "LOW",
                shorted ? "  -> SDA and SCL are SHORTED together (same breadboard row?)"
                : (!sda || !scl) ? "  -> a line is stuck LOW (touching GND, or a module wired wrong)" : "  ok");
  return sda && scl && !shorted;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Doomclock gateway ===");

  gStateMutex = xSemaphoreCreateMutex();
  gI2cMutex = xSemaphoreCreateMutex();
  gCloudQ = xQueueCreate(10, sizeof(CloudMsg));
  gNotifyQ = xQueueCreate(6, sizeof(NotifyMsg));

  bool i2cOk = checkI2cLines();
  if (i2cOk) {
    Wire.begin(PIN_SDA, PIN_SCL, 400000);
    Wire.setTimeOut(20);
    Serial.print("[I2C] found:");
    int n = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
    }
    Serial.println(n ? "  (BH1750 = 0x23 or 0x5C, DS3231 = 0x68 + EEPROM 0x57)" : " nothing — check SDA=21, SCL=22, VCC=3V3, GND");
  } else {
    // Touching a broken bus makes the I2C driver hang, so run without BH1750 / DS3231 until it is fixed.
    Serial.println("[I2C] SKIPPED — fix the SDA/SCL wiring. Running without BH1750 and DS3231.");
  }
  pinMode(PIN_HALL_DO, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_HALL_AO, ADC_11db);
  prefs.begin("gw", false);
  hallBaseline = prefs.getInt("hallBase", HALL_BASELINE_DEF);

  bhOk = i2cOk && lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  dht.begin();
  Serial.printf("[HW] BH1750 %s, hall baseline %d\n", bhOk ? "ok" : "NOT FOUND", hallBaseline);
  clockBegin(i2cOk);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // modem sleep makes the radio deaf to ESP-NOW between beacons
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  linkBegin();
  cloudBegin();

  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) {
    postEvent("powerOn", rr == ESP_RST_BROWNOUT ? "brownout" : "power on");
    postNotify("⚡ **Doomclock:** Gateway เริ่มทำงานใหม่ (เปิดเครื่อง / ไฟดับแล้วกลับมา)");
  } else {
    postEvent("reboot", "software/watchdog reset");
  }

  xTaskCreatePinnedToCore(TaskLink,    "link",    6144,  nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(TaskSensors, "sensors", 6144,  nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(TaskCloud,   "cloud",   10240, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(TaskNotify,  "notify",  8192,  nullptr, 0, nullptr, 0);
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 's') printStats();
    else if (c == 'h') Serial.printf("[HALL] raw=%d baseline=%d DO=%d\n", readHall(), hallBaseline, digitalRead(PIN_HALL_DO));
    else if (c == 'c') {
      hallBaseline = readHall();
      prefs.putInt("hallBase", hallBaseline);
      Serial.printf("[HALL] baseline saved = %d (do this with the tray UP / no magnet near)\n", hallBaseline);
    }
    else if (c == 'd') postNotify("🔔 ทดสอบ Discord จาก Doomclock gateway");
  }
  vTaskDelay(pdMS_TO_TICKS(50));
}
