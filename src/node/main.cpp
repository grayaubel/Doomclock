/*
 * Doomclock — BED SENSOR NODE
 *
 * Sensors : MPU6050 (own register-level driver, mpu6050.cpp)  -> movement on the mattress
 *           VL53L0X (ToF)                                      -> is someone on the bed, lying or sitting
 *           KY-038 mic (optional, ENABLE_MIC)                  -> extra AI feature
 * Storage : SD card (optional, ENABLE_SD)                      -> backup copy of every window
 * Link    : ESP-NOW -> gateway, application-level ACK + retry, RAM backlog while the
 *           gateway is unreachable, automatic Wi-Fi channel scan (the gateway's channel
 *           is whatever channel the phone hotspot uses).
 *
 * Every 10 s the node turns ~250 IMU samples and ~50 ToF readings into one BedPacket
 * (see include/doom_protocol.h). Sending features instead of raw 25 Hz data keeps the
 * radio quiet and still captures every movement, which a single snapshot every 30 s
 * would miss.
 *
 * Tasks: TaskSense (prio 3, core 1) — fixed-rate sampling with vTaskDelayUntil
 *        TaskComm  (prio 2, core 0) — backlog, send, wait ACK, retry, channel scan
 */
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <Preferences.h>
#include <VL53L0X.h>
#include <math.h>
#include "doom_protocol.h"
#include "mpu6050.h"

// ============================ CONFIG ============================
#define NODE_ID            1
#define ENABLE_MIC         0      // 1 = read KY-038 analog output
#define ENABLE_SD          0      // 1 = append every window to /bed_log.csv on the SD card

#define PIN_SDA            21
#define PIN_SCL            22
#define PIN_MIC_AO         34     // must be an ADC1 pin: ADC2 is unusable while the radio is on
#define PIN_SD_CS          5      // SD on VSPI: SCK 18, MISO 19, MOSI 23
#define PIN_LED            2      // on-board LED: lit = gateway not answering

// "moving" thresholds for move_count — tune after looking at real nights
#define MOVE_JERK_MG       20.0f
#define MOVE_GYRO_DPS      3.0f

#define ACK_TIMEOUT_MS     150
#define TX_ATTEMPTS        3      // tries per packet per round
#define SCAN_AFTER_FAILS   3      // failed rounds before scanning all channels
#define BACKLOG_MAX        90     // 90 windows = 15 min of data kept while the gateway is down
#define BACKOFF_MAX_MS     16000

#if ENABLE_SD
#include <SPI.h>
#include <SD.h>
#endif

// ============================ STATE =============================
struct Entry {
  BedPacket p;
  uint32_t endMs;     // millis() at window end, for age_s
  uint8_t tries;
};

static Mpu6050 imu;
static VL53L0X tof;
static bool imuOk = false, tofOk = false, sdOk = false;
static float gBias[3] = {0, 0, 0};          // gyro bias, deg/s

static QueueHandle_t windowQ;               // TaskSense -> TaskComm
static SemaphoreHandle_t ackSem;            // recv callback -> TaskComm
static volatile uint32_t ackSeq = 0;
static volatile uint32_t gwUnix = 0, gwUnixAtMs = 0;
static uint8_t ackMac[6];
static uint8_t gwMac[6];
static bool gwKnown = false;
static uint16_t bootId;
static uint8_t curChannel = 1;
static Preferences prefs;
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// backlog ring, owned by TaskComm only
static Entry ring[BACKLOG_MAX];
static int ringHead = 0, ringCount = 0;
static uint16_t droppedTotal = 0;

static inline uint16_t u16c(float v) { return v <= 0 ? 0 : v >= 65535.0f ? 65535 : (uint16_t)(v + 0.5f); }
static inline int8_t i8c(float v) { return v <= -127 ? -127 : v >= 127 ? 127 : (int8_t)lroundf(v); }

// ============================ ESP-NOW ===========================
static void handleRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len != (int)sizeof(AckPacket)) return;
  AckPacket a;
  memcpy(&a, data, sizeof(a));
  if (!doom_check(a) || a.type != DOOM_PKT_ACK || a.node_id != NODE_ID || a.boot_id != bootId) return;
  memcpy(ackMac, mac, 6);
  if (a.unix_time) { gwUnix = a.unix_time; gwUnixAtMs = millis(); }
  ackSeq = a.seq;
  xSemaphoreGive(ackSem);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) { handleRecv(info->src_addr, data, len); }
#else
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) { handleRecv(mac, data, len); }
#endif

static void setChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);              // channel can only be forced in this mode while unconnected
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  curChannel = ch;
}

static void addPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;                               // 0 = follow the current channel
  p.encrypt = false;
  esp_now_add_peer(&p);
}

// One transmission + wait for the matching ACK.
static bool sendOnce(Entry& e, uint32_t timeoutMs) {
  BedPacket& p = e.p;
  e.tries = e.tries < 255 ? e.tries + 1 : 255;
  p.attempts = e.tries;
  uint32_t age = (millis() - e.endMs) / 1000;
  p.age_s = age > 65535 ? 65535 : age;
  p.backlog = ringCount;
  p.dropped_total = droppedTotal;
  doom_seal(p);

  xSemaphoreTake(ackSem, 0);                   // clear a stale ACK
  if (esp_now_send(gwKnown ? gwMac : BCAST, (const uint8_t*)&p, sizeof(p)) != ESP_OK) return false;

  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    uint32_t left = timeoutMs - (millis() - t0);
    if (xSemaphoreTake(ackSem, pdMS_TO_TICKS(left)) == pdTRUE && ackSeq == p.seq) {
      if (!gwKnown || memcmp(gwMac, ackMac, 6) != 0) {
        memcpy(gwMac, ackMac, 6);
        addPeer(gwMac);                        // switch from broadcast to unicast (MAC-level retries too)
        gwKnown = true;
        Serial.printf("[LINK] gateway %02X:%02X:%02X:%02X:%02X:%02X on channel %u\n",
                      gwMac[0], gwMac[1], gwMac[2], gwMac[3], gwMac[4], gwMac[5], curChannel);
      }
      return true;
    }
  }
  return false;
}

// Gateway silent for several rounds: it may have moved to another channel (new hotspot).
static bool scanChannels(Entry& e) {
  uint8_t start = curChannel;
  gwKnown = false;                             // use broadcast while scanning
  Serial.println("[LINK] scanning channels 1-13 ...");
  for (uint8_t ch = 1; ch <= 13; ch++) {
    setChannel(ch);
    if (sendOnce(e, 80)) {
      if (ch != prefs.getUChar("ch", 0)) prefs.putUChar("ch", ch);
      return true;
    }
  }
  setChannel(start);
  Serial.println("[LINK] gateway not found");
  return false;
}

// ============================ SD (optional) =====================
#if ENABLE_SD
static void sdLog(const Entry& e) {
  if (!sdOk) return;
  bool fresh = !SD.exists("/bed_log.csv");
  File f = SD.open("/bed_log.csv", FILE_APPEND);
  if (!f) return;
  if (fresh) f.println("unix_time,boot_id,seq,imu_n,move_count,acc_dev_mean_mg,acc_dev_max_mg,jerk_mean_mg,jerk_max_mg,"
                       "gyro_mean_cdps,gyro_max_cdps,tilt_x,tilt_y,tof_n,tof_valid,tof_mean_mm,tof_min_mm,tof_max_mm,"
                       "tof_std_mm,mic_p2p_mean,mic_p2p_max");
  uint32_t t = 0;
  if (gwUnix) t = gwUnix + (int32_t)(e.endMs - gwUnixAtMs) / 1000;
  const BedPacket& p = e.p;
  f.printf("%lu,%u,%lu,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u\n",
           (unsigned long)t, p.boot_id, (unsigned long)p.seq, p.imu_n, p.move_count, p.acc_dev_mean_mg,
           p.acc_dev_max_mg, p.jerk_mean_mg, p.jerk_max_mg, p.gyro_mean_cdps, p.gyro_max_cdps,
           p.tilt_x_deg, p.tilt_y_deg, p.tof_n, p.tof_valid, p.tof_mean_mm, p.tof_min_mm, p.tof_max_mm,
           p.tof_std_mm, p.mic_p2p_mean, p.mic_p2p_max);
  f.close();
}
#endif

// ============================ TASKS =============================
struct WindowAcc {
  uint16_t n, moves;
  float accDevSum, accDevMax, jerkSum, jerkMax, gyroSum, gyroMax, tiltXSum, tiltYSum;
  uint8_t tofN, tofValid;
  uint32_t tofSum; uint64_t tofSq; uint16_t tofMin, tofMax;
  uint32_t micSum; uint16_t micMax, micN;
  void reset() { memset(this, 0, sizeof(*this)); tofMin = 0xFFFF; }
};

void TaskSense(void*) {
  const TickType_t period = pdMS_TO_TICKS(1000 / DOOM_IMU_HZ);
  const int tofEvery = DOOM_IMU_HZ / DOOM_TOF_HZ;
  TickType_t last = xTaskGetTickCount();
  WindowAcc w; w.reset();
  uint32_t winStart = millis(), seq = 0;
  float pax = 0, pay = 0, paz = 0, aBase = 1.0f;
  bool havePrev = false;
  int tick = 0;

  for (;;) {
    vTaskDelayUntil(&last, period);
    tick++;

    // ---- IMU @ 25 Hz ----
    Mpu6050::Raw r;
    if (imuOk && imu.read(r)) {
      float ax = r.ax / Mpu6050::ACC_LSB_PER_G, ay = r.ay / Mpu6050::ACC_LSB_PER_G, az = r.az / Mpu6050::ACC_LSB_PER_G;
      float gx = r.gx / Mpu6050::GYRO_LSB_PER_DPS - gBias[0];
      float gy = r.gy / Mpu6050::GYRO_LSB_PER_DPS - gBias[1];
      float gz = r.gz / Mpu6050::GYRO_LSB_PER_DPS - gBias[2];
      float amag = sqrtf(ax * ax + ay * ay + az * az);
      float gmag = sqrtf(gx * gx + gy * gy + gz * gz);
      if (!havePrev) aBase = amag;
      aBase += (amag - aBase) / (DOOM_IMU_HZ * 60.0f);   // ~1 min baseline -> sensor offset cancels out
      float dev = fabsf(amag - aBase) * 1000.0f;
      float jerk = havePrev ? sqrtf((ax - pax) * (ax - pax) + (ay - pay) * (ay - pay) + (az - paz) * (az - paz)) * 1000.0f : 0;
      pax = ax; pay = ay; paz = az; havePrev = true;
      if (gmag < 1.0f) {                                   // slow gyro drift compensation while still
        gBias[0] += (r.gx / Mpu6050::GYRO_LSB_PER_DPS - gBias[0]) * 0.001f;
        gBias[1] += (r.gy / Mpu6050::GYRO_LSB_PER_DPS - gBias[1]) * 0.001f;
        gBias[2] += (r.gz / Mpu6050::GYRO_LSB_PER_DPS - gBias[2]) * 0.001f;
      }
      w.n++;
      w.accDevSum += dev;  if (dev > w.accDevMax) w.accDevMax = dev;
      w.jerkSum += jerk;   if (jerk > w.jerkMax) w.jerkMax = jerk;
      w.gyroSum += gmag;   if (gmag > w.gyroMax) w.gyroMax = gmag;
      w.tiltXSum += atan2f(ay, az) * 57.2958f;
      w.tiltYSum += atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2958f;
      if (jerk > MOVE_JERK_MG || gmag > MOVE_GYRO_DPS) w.moves++;
    }

    // ---- ToF @ 5 Hz (sensor runs back-to-back, we take the latest result) ----
    if (tofOk && tick % tofEvery == 0) {
      w.tofN++;
      if (tof.readReg(VL53L0X::RESULT_INTERRUPT_STATUS) & 0x07) {
        uint16_t mm = tof.readRangeContinuousMillimeters();
        if (!tof.timeoutOccurred() && mm < 8000) {
          w.tofValid++; w.tofSum += mm; w.tofSq += (uint64_t)mm * mm;
          if (mm < w.tofMin) w.tofMin = mm;
          if (mm > w.tofMax) w.tofMax = mm;
        }
      }
    }

#if ENABLE_MIC
    // ---- mic: peak-to-peak of a short burst every tick ----
    {
      uint16_t mn = 4095, mx = 0;
      for (int i = 0; i < 64; i++) { uint16_t v = analogRead(PIN_MIC_AO); if (v < mn) mn = v; if (v > mx) mx = v; }
      uint16_t p2p = mx - mn;
      w.micSum += p2p; w.micN++; if (p2p > w.micMax) w.micMax = p2p;
    }
#endif

    // ---- close the window ----
    if (millis() - winStart >= DOOM_WINDOW_MS) {
      winStart += DOOM_WINDOW_MS;
      Entry e; memset(&e, 0, sizeof(e));
      BedPacket& p = e.p;
      p.magic = DOOM_MAGIC; p.ver = DOOM_PROTO_VER; p.type = DOOM_PKT_BED;
      p.node_id = NODE_ID; p.boot_id = bootId; p.seq = seq++;
      p.flags = (imuOk ? BED_F_IMU_OK : 0) | (tofOk ? BED_F_TOF_OK : 0) |
                (ENABLE_MIC ? BED_F_MIC_ON : 0) | (sdOk ? BED_F_SD_OK : 0);
      p.imu_n = w.n; p.move_count = w.moves;
      if (w.n) {
        p.acc_dev_mean_mg = u16c(w.accDevSum / w.n); p.acc_dev_max_mg = u16c(w.accDevMax);
        p.jerk_mean_mg = u16c(w.jerkSum / w.n);      p.jerk_max_mg = u16c(w.jerkMax);
        p.gyro_mean_cdps = u16c(w.gyroSum / w.n * 100); p.gyro_max_cdps = u16c(w.gyroMax * 100);
        p.tilt_x_deg = i8c(w.tiltXSum / w.n);         p.tilt_y_deg = i8c(w.tiltYSum / w.n);
      }
      p.tof_n = w.tofN; p.tof_valid = w.tofValid;
      if (w.tofValid) {
        float mean = (float)w.tofSum / w.tofValid;
        float var = (float)w.tofSq / w.tofValid - mean * mean;
        p.tof_mean_mm = u16c(mean); p.tof_min_mm = w.tofMin; p.tof_max_mm = w.tofMax;
        p.tof_std_mm = u16c(var > 0 ? sqrtf(var) : 0);
      }
      if (w.micN) { p.mic_p2p_mean = u16c((float)w.micSum / w.micN); p.mic_p2p_max = w.micMax; }
      e.endMs = millis();
      Serial.printf("[WIN] seq=%lu imu=%u moves=%u jerk=%u/%u mg gyro=%u cdps tof=%u mm (%u/%u ok)\n",
                    (unsigned long)p.seq, p.imu_n, p.move_count, p.jerk_mean_mg, p.jerk_max_mg,
                    p.gyro_mean_cdps, p.tof_mean_mm, p.tof_valid, p.tof_n);
      if (xQueueSend(windowQ, &e, 0) != pdTRUE) Serial.println("[WIN] queue full, window lost");
      w.reset();
    }
  }
}

void TaskComm(void*) {
  uint32_t nextTry = 0, backoff = 1000;
  int failRounds = 0;
  for (;;) {
    Entry in;
    while (xQueueReceive(windowQ, &in, ringCount ? 0 : pdMS_TO_TICKS(500)) == pdTRUE) {
#if ENABLE_SD
      sdLog(in);
#endif
      if (ringCount == BACKLOG_MAX) {                    // full: drop the oldest window
        ringHead = (ringHead + 1) % BACKLOG_MAX; ringCount--; droppedTotal++;
      }
      ring[(ringHead + ringCount) % BACKLOG_MAX] = in;
      ringCount++;
    }
    if (!ringCount || (int32_t)(millis() - nextTry) < 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

    Entry& e = ring[ringHead];
    bool ok = false;
    for (int a = 0; a < TX_ATTEMPTS && !ok; a++) ok = sendOnce(e, ACK_TIMEOUT_MS);
    if (!ok && ++failRounds >= SCAN_AFTER_FAILS) { failRounds = 0; ok = scanChannels(e); }

    if (ok) {
      ringHead = (ringHead + 1) % BACKLOG_MAX; ringCount--;
      failRounds = 0; backoff = 1000;
      nextTry = millis() + 30;                           // drain a backlog quickly but politely
      digitalWrite(PIN_LED, LOW);
    } else {
      nextTry = millis() + backoff;
      backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
      digitalWrite(PIN_LED, HIGH);
      Serial.printf("[LINK] no ACK for seq %lu (backlog %d), retry in %lu ms\n",
                    (unsigned long)e.p.seq, ringCount, (unsigned long)backoff);
    }
  }
}

// ============================ SETUP =============================
static void i2cScan() {
  Serial.print("[I2C] found:");
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
  }
  Serial.println(n ? "  (MPU6050 = 0x68/0x69, VL53L0X = 0x29)" : " nothing — check wiring");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== Doomclock bed node #%d ===\n", NODE_ID);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  i2cScan();

  imuOk = imu.begin(Wire, 0x68) || imu.begin(Wire, 0x69);
  if (imuOk) {
    Serial.printf("[IMU] WHO_AM_I = 0x%02X%s\n", imu.whoAmI(), imu.whoAmI() == 0x68 ? "" : " (clone chip, usually fine)");
    // gyro bias: keep the mattress still for ~1 s after power-up
    float s[3] = {0, 0, 0}; int n = 0;
    for (int i = 0; i < 50; i++) {
      Mpu6050::Raw r;
      if (imu.read(r)) { s[0] += r.gx; s[1] += r.gy; s[2] += r.gz; n++; }
      delay(20);
    }
    for (int k = 0; k < 3 && n; k++) gBias[k] = s[k] / n / Mpu6050::GYRO_LSB_PER_DPS;
  } else Serial.println("[IMU] MPU6050 not found");

  tof.setBus(&Wire);
  tof.setTimeout(100);
  tofOk = tof.init();
  if (tofOk) tof.startContinuous(0);
  Serial.printf("[TOF] VL53L0X %s\n", tofOk ? "ok" : "not found");

#if ENABLE_MIC
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MIC_AO, ADC_11db);
#endif
#if ENABLE_SD
  sdOk = SD.begin(PIN_SD_CS);
  Serial.printf("[SD] %s\n", sdOk ? "ok" : "not found");
#endif

  bootId = (uint16_t)(esp_random() & 0xFFFF);
  if (!bootId) bootId = 1;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  prefs.begin("node", false);
  setChannel(prefs.getUChar("ch", 1));
  Serial.printf("[LINK] node MAC %s, boot_id %u, start channel %u\n", WiFi.macAddress().c_str(), bootId, curChannel);
  if (esp_now_init() != ESP_OK) Serial.println("[LINK] esp_now_init FAILED");
  esp_now_register_recv_cb(onRecv);
  addPeer(BCAST);

  ackSem = xSemaphoreCreateBinary();
  windowQ = xQueueCreate(6, sizeof(Entry));
  xTaskCreatePinnedToCore(TaskSense, "sense", 4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(TaskComm, "comm", ENABLE_SD ? 6144 : 4096, nullptr, 2, nullptr, 0);
}

void loop() { vTaskDelete(nullptr); }
