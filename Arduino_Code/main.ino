/*
 * Smart Alarm Clock + Phone Lockbox  -  GATEWAY & CLOUD firmware (REAL HARDWARE + WOKWI)
 * Course: Embedded System (CEDT) Final Project 2026
 *
 * Libraries: PubSubClient, DHT sensor library, BH1750, Adafruit VL53L0X,
 *            ESP32Servo, Adafruit SSD1306 + GFX, RTClib (DS3231)
 *            (+ Edge Impulse library "alarm_inferencing.h" when USE_EDGE_IMPULSE is on)
 * Core: arduino-esp32 3.x
 *
 * secrets.h must define (see secrets.example.h):
 *   WIFI_SSID, WIFI_PASSWORD, MQTT_CLIENT_ID, MQTT_TOKEN, MQTT_SECRET, DISCORD_WEBHOOK_URL
 *   optional: NETPIE_ROOT_CA, DISCORD_ROOT_CA   (PEM raw string)
 *
 * Tasks (E1): Sensor, ESPNOW rx, AI, UI/State machine, Cloud (MQTT/TLS), Notify (Discord/HTTPS)
 * ISRs  (E2): PIR, rotary encoder rotate, rotary encoder button  (deferred processing in TaskUI)
 *
 * Wokwi serial commands (WOKWI_SIM=1):
 *   z = หลับ   b = ตื่นอยู่บนเตียง   u = ลุกจากเตียง   m = ใช้โมเดล AI จริง
 *   w = ตั้งปลุกอีก 2 นาที   d = ทดสอบแจ้งเตือน Discord   x = หยุด/เริ่มส่งข้อมูลจาก node จำลอง
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <DHT.h>
#include <BH1750.h>
#include <Adafruit_VL53L0X.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <ESP32Servo.h>
#include <esp_now.h>
#include <esp_system.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include "secrets.h"

// #define USE_EDGE_IMPULSE            // เปิดเมื่อใส่ไลบรารี Edge Impulse แล้ว
#ifdef USE_EDGE_IMPULSE
#include <alarm_inferencing.h>       // ชื่อไลบรารีตามโปรเจกต์ Edge Impulse ของคุณ
#endif

// ======================= PINS =======================
#define DHTPIN            15
#define PIR_PIN           4
#define MICROSWITCH_PIN   26     // LOW = มือถืออยู่ในกล่อง
#define SERVO_PIN         27
#define ENC_CLK           32
#define ENC_DT            33
#define ENC_SW            25
#define BUZZER_PIN        13     // passive buzzer
// I2C (SDA=21, SCL=22): BH1750, VL53L0X, SSD1306 (0x3C), DS3231

// ======================= CONFIG =====================
#define WOKWI_SIM 1            // 1 = รันบน Wokwi, 0 = บอร์ดจริง (อย่าลืมแก้ก่อนส่งงาน)
#if WOKWI_SIM
  #define NET_SSID "Wokwi-GUEST"
  #define NET_PASS ""
  #define NET_CH   6
#else
  #define NET_SSID WIFI_SSID
  #define NET_PASS WIFI_PASSWORD
  #define NET_CH   0
#endif

#define DISCORD_SIM_ONLY     0     // 1 = ใน Wokwi แค่พิมพ์ข้อความ Discord ไม่ยิง HTTPS | 0 = ยิงจริง (ไม่มีผลบนบอร์ดจริง)
#define LOG_TELEMETRY        0     // 1 = พิมพ์ [Cloud] telemetry ทุก 5 วิ (ปกติปิดไว้ให้ log อ่านง่าย)
#define DBG_STATE            (WOKWI_SIM)   // พิมพ์ [DBG] เมื่อตัวแปรสำคัญเปลี่ยน

#define BED_NODE_ID          1
#define MAX_NODES            4
#define NODE_PERIOD_S        30                // sensor node ส่งทุก 30 วิ
#define BED_STALE_MS         (NODE_PERIOD_S * 3 * 1000UL)
#define WIN_N                20                // 20 ตัวอย่าง x 30 วิ = 10 นาที
#define MOVE_ACCEL_DELTA     1500              // ปรับตามข้อมูลจริง (raw LSB)
#define MOVE_GYRO_ABS        3000
#define AI_FEATURES          6
#define AI_PERIOD_MS         10000

#define LOCK_ANGLE           90
#define UNLOCK_ANGLE         0
#define SERVO_HOLD_MS        700               // timeout stop: หยุดจ่าย PWM หลังเคลื่อนที่
#define PHONE_TOF_MM         60                // ToF สำรอง ถ้าระยะน้อยกว่านี้ถือว่ามือถืออยู่

#define VERIFY_AWAKE_MS      (WOKWI_SIM ? 60UL * 1000UL : 30UL * 60UL * 1000UL)   // ต้องตื่นจริงสะสม 30 นาที
#define EMERGENCY_MS         (WOKWI_SIM ? 20UL * 1000UL : 3UL * 60UL * 1000UL)    // 3-5 นาที
#define LONG_PRESS_MS        2000
#define ALARM_STEP_MIN       5

#define GMT_OFFSET_SEC       (7 * 3600)
#define MQTT_SERVER          "mqtt.netpie.io"
#define MQTT_PORT            (WOKWI_SIM ? 1883 : 8883)
#define MQTT_TOPIC           "@shadow/data/update"
#define TELEMETRY_MS         5000UL
#define OFFLINE_TELE_MS      60000UL           // ตอนออฟไลน์เก็บ telemetry ลงแฟลชนาทีละ 1 รายการ (event เก็บครบทุกตัว)
#define Q_FILE               "/offline_queue.txt"
#define Q_TMP                "/q_tmp.txt"
#define Q_MAX_BYTES          (96 * 1024)

// ======================= TYPES ======================
enum AlarmState : uint8_t { ST_IDLE, ST_LOCKED, ST_SLEPT, ST_ALARM, ST_UP, ST_UNLOCKED };
const char* STATE_NAMES[] = {"Idle", "Locked", "Slept", "Alarm", "Up", "Unlocked"};
enum SleepClass : uint8_t { C_ASLEEP, C_AWAKE_IN_BED, C_UP };
const char* CLASS_NAMES[] = {"ASLEEP", "AWAKE_IN_BED", "UP"};

// Wire format จาก Sensor Node: 1 + 24 + 3 = 28 ไบต์ (ฝั่ง node ต้อง pack ให้ตรงกัน)
//   imu[0..5]  = IMU1 ax ay az gx gy gz,  imu[6..11] = IMU2
//   fsr[3]     = FSR1 (12 bit) + FSR2 (12 bit)  : f1 = b0 | (b1&0x0F)<<8 ,  f2 = b1>>4 | b2<<4
typedef struct __attribute__((packed)) {
  uint8_t node_id;
  int16_t imu[12];
  uint8_t fsr[3];
} NodePacket;
static_assert(sizeof(NodePacket) == 28, "NodePacket must be 28 bytes");

struct RxItem { uint8_t mac[6]; NodePacket pkt; };
struct CloudEvent { char action[24]; char note[120]; };
struct NotifyItem { char text[512]; };            // ข้อความไทย 3 ไบต์/ตัว: 240 เดิมใกล้เต็มสำหรับ morning summary

struct Shared {
  float temp, hum, lux; int distance;
  bool phoneInserted;
  uint8_t lastNode; uint16_t force1, force2;
  float moveRatio, stillMin, forceNorm;
  bool bedFresh;
  float pirRecent;
  SleepClass ai; bool aiValid;
  uint8_t state; uint16_t alarmMin; bool alarmSet;
  uint32_t alarmFiredMs;
  uint32_t emergencyLeftS;
};

// ======================= GLOBALS ====================
#if WOKWI_SIM
WiFiClient espClient;            // Wokwi: MQTT ธรรมดา พอร์ต 1883
#else
WiFiClientSecure espClient;      // บอร์ดจริง: TLS พอร์ต 8883
#endif
PubSubClient mqtt(espClient);
DHT dht(DHTPIN, DHT22);
BH1750 lightMeter;
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
Adafruit_SSD1306 display(128, 64, &Wire, -1);
RTC_DS3231 rtc;
Servo lockServo;
Preferences prefs;

SemaphoreHandle_t dataMutex, i2cMutex;
QueueHandle_t espnowQueue, eventQueue, notifyQueue;

Shared S;
#if WOKWI_SIM
volatile int simForceAI = -1;    // Wokwi: บังคับคลาส AI ให้ทดสอบ flow ได้แน่นอน (-1 = ใช้โมเดลจริง)
volatile int simAlarmReq = -1;   // SimBed ขอตั้งเวลาปลุก -> TaskUI เป็นคนเขียนจริง (กัน race)
volatile bool simPause = false;  // หยุดส่งข้อมูล node จำลอง (ทดสอบ nodeOffline)
#endif
bool hasBH1750 = false, hasVL53L0X = false, hasRTC = false, hasOLED = false, fsOk = false;
bool rtcSynced = false;
volatile bool cloudOk = false;   // TaskCloud เป็นคนตั้ง ให้ UI อ่านแทนการเรียก mqtt.connected() ข้าม task

// bed window (ใช้เฉพาะใน TaskESPNOW)
struct WinSample { bool moved; uint16_t force; };
WinSample win[WIN_N];
int winHead = 0, winCount = 0;
int16_t prevImu[12]; bool havePrev = false;
uint32_t lastRxMs[MAX_NODES + 1] = {0};
bool nodeOffline[MAX_NODES + 1] = {false};

// state machine (เป็นของ TaskUI เท่านั้น)
AlarmState state = ST_IDLE;
uint32_t verifiedMs = 0, sleepStartMs = 0, alarmFiredMs = 0, emergencyStartMs = 0;
bool emergencyActive = false;
int lastDueKey = -1;
uint32_t servoDetachAt = 0;
uint32_t pirTimes[16] = {0}; int pirIdx = 0;

// ISR shared
volatile bool pirFlag = false;
volatile bool btnFlag = false;
volatile uint32_t lastBtnMs = 0, lastEncUs = 0;
volatile int32_t encDelta = 0;
portMUX_TYPE isrMux = portMUX_INITIALIZER_UNLOCKED;

// ======================= ISRs =======================
void IRAM_ATTR pirISR() { pirFlag = true; }
void IRAM_ATTR encRotateISR() {            // ทำงานที่ขอบ FALLING ของ CLK
  uint32_t n = micros();
  if (n - lastEncUs < 1500) return;        // debounce
  lastEncUs = n;
  encDelta = encDelta + (digitalRead(ENC_DT) ? 1 : -1);   // ถ้าหมุนกลับทิศ ให้สลับ +1/-1
}
void IRAM_ATTR encButtonISR() {
  uint32_t n = millis();
  if (n - lastBtnMs > 40) { lastBtnMs = n; btnFlag = true; }
}

// ======================= HELPERS ====================
inline void lockData()   { xSemaphoreTake(dataMutex, portMAX_DELAY); }
inline void unlockData() { xSemaphoreGive(dataMutex); }

String jsonEscape(const char* s) {
  String o;
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}

void postEvent(const char* action, const char* note = "") {
  CloudEvent e; strlcpy(e.action, action, sizeof(e.action)); strlcpy(e.note, note, sizeof(e.note));
  xQueueSend(eventQueue, &e, 0);
}

void postNotify(const String& text) {
  NotifyItem n;
  size_t len = min((size_t)text.length(), sizeof(n.text) - 1);
  while (len > 0 && len < text.length() && (((uint8_t)text[len]) & 0xC0) == 0x80) len--;   // ไม่ตัดกลางตัวอักษร UTF-8
  memcpy(n.text, text.c_str(), len); n.text[len] = 0;
  xQueueSend(notifyQueue, &n, 0);
}

bool getNow(struct tm& t) {
  memset(&t, 0, sizeof(t));
  if (hasRTC && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    DateTime n = rtc.now();
    xSemaphoreGive(i2cMutex);
    if (n.year() >= 2024) {                 // RTC ยังไม่เคยตั้งเวลา/แบตหมด -> ปีจะผิด ให้ใช้ NTP แทน
      t.tm_year = n.year() - 1900; t.tm_mon = n.month() - 1; t.tm_mday = n.day();
      t.tm_hour = n.hour(); t.tm_min = n.minute(); t.tm_sec = n.second();
      return true;
    }
  }
  return getLocalTime(&t, 0);
}

String isoNow() {
  struct tm t; char b[32];
  if (!getNow(t)) return "1970-01-01T00:00:00+07:00";
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S+07:00", &t);
  return String(b);
}

void servoCmd(int angle) {                  // non-blocking + timeout stop
  if (!lockServo.attached()) lockServo.attach(SERVO_PIN, 500, 2400);
  lockServo.write(angle);
  servoDetachAt = millis() + SERVO_HOLD_MS;
}

void persistState() {                       // เรียกจาก TaskUI (และ setup ก่อนสร้าง task) เท่านั้น
  prefs.putUChar("st", (uint8_t)state);
  prefs.putUShort("am", S.alarmMin);
  prefs.putBool("as", S.alarmSet);
  prefs.putUInt("vm", verifiedMs);
}

void setAlarm(uint16_t m, bool set, bool persist = true) {
  lockData(); S.alarmMin = m; S.alarmSet = set; unlockData();
  if (persist) persistState();
}

void enterState(AlarmState ns) {
  state = ns;
  lockData(); S.state = ns; unlockData();
  persistState();
  Serial.printf("[STATE] -> %s\n", STATE_NAMES[ns]);
}

// ======================= AI ==========================
#ifdef USE_EDGE_IMPULSE
static float eiFeat[AI_FEATURES];
static int eiGetData(size_t off, size_t len, float* out) { memcpy(out, eiFeat + off, len * sizeof(float)); return 0; }
SleepClass classify(const float* f, float* conf) {
  memcpy(eiFeat, f, sizeof(eiFeat));
  signal_t sig; sig.total_length = AI_FEATURES; sig.get_data = &eiGetData;
  ei_impulse_result_t r = {0};
  if (run_classifier(&sig, &r, false) != EI_IMPULSE_OK) { *conf = 0; return C_AWAKE_IN_BED; }
  int best = 0;
  for (size_t i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++)
    if (r.classification[i].value > r.classification[best].value) best = i;
  *conf = r.classification[best].value;
  const char* l = r.classification[best].label;
  if (!strcmp(l, "ASLEEP")) return C_ASLEEP;
  if (!strcmp(l, "UP")) return C_UP;
  return C_AWAKE_IN_BED;
}
#else
// ---- PLACEHOLDER: logistic regression (softmax) น้ำหนักสมมติ ใช้ทดสอบ pipeline เท่านั้น ----
// !!! ต้องแทนที่ด้วยโมเดลที่ train จริงจาก Edge Impulse ก่อนส่งงาน (Requirement M4 / E7) !!!
// features: moveRatio, stillMin/10, forceNorm, pirRecent/5, lux/500, minSinceAlarm/60
static const float W[3][AI_FEATURES] = {
  {-4.0f,  3.0f,  2.0f, -2.0f, -1.5f, -0.5f},   // ASLEEP
  { 3.0f, -2.0f,  2.0f, -0.5f,  1.0f,  0.5f},   // AWAKE_IN_BED
  { 1.0f,  0.0f, -6.0f,  2.5f,  1.5f,  1.0f}    // UP
};
static const float B[3] = {-0.5f, -1.0f, 2.0f};
SleepClass classify(const float* f, float* conf) {
  float z[3], mx = -1e9f, sum = 0;
  for (int c = 0; c < 3; c++) {
    z[c] = B[c];
    for (int i = 0; i < AI_FEATURES; i++) z[c] += W[c][i] * f[i];
    if (z[c] > mx) mx = z[c];
  }
  int best = 0;
  for (int c = 0; c < 3; c++) { z[c] = expf(z[c] - mx); sum += z[c]; if (z[c] > z[best]) best = c; }
  *conf = z[best] / sum;
  return (SleepClass)best;
}
#endif

// ======================= CALLBACK ====================
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len != (int)sizeof(NodePacket)) return;
  RxItem it; memcpy(it.mac, info->src_addr, 6); memcpy(&it.pkt, data, sizeof(NodePacket));
  xQueueSend(espnowQueue, &it, 0);          // ไม่ใช่ ISR context -> ใช้ xQueueSend ปกติ
}

// ======================= TASKS =======================
void TaskSensor(void *pv) {
  for (;;) {
    float t = dht.readTemperature(), h = dht.readHumidity();
    float l = 0; int d = -1;
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      if (hasBH1750) { l = lightMeter.readLightLevel(); if (l < 0) l = 0; }
      if (hasVL53L0X) {
        VL53L0X_RangingMeasurementData_t m; lox.rangingTest(&m, false);
        if (m.RangeStatus != 4) d = m.RangeMilliMeter;
      }
      xSemaphoreGive(i2cMutex);
    }
    lockData();
    if (!isnan(t)) S.temp = t;
    if (!isnan(h)) S.hum = h;
    S.lux = l; S.distance = d;
    unlockData();
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

#if !WOKWI_SIM
void ackNode(const uint8_t* mac, uint8_t id) {
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t p = {}; memcpy(p.peer_addr, mac, 6); p.channel = 0; p.encrypt = false;
    esp_now_add_peer(&p);
  }
  uint8_t ack[2] = {0xA5, id};              // app-level ACK ให้ node ใช้ตัดสินใจ retry
  esp_now_send(mac, ack, sizeof(ack));
}
#else
void ackNode(const uint8_t*, uint8_t) {}   // Wokwi ไม่มี ESP-NOW
#endif

void TaskESPNOW(void *pv) {
  RxItem it;
  for (;;) {
    if (xQueueReceive(espnowQueue, &it, pdMS_TO_TICKS(1000)) == pdPASS) {
      NodePacket &p = it.pkt;
      if (p.node_id >= 1 && p.node_id <= MAX_NODES) {
        ackNode(it.mac, p.node_id);
        uint16_t f1 = p.fsr[0] | ((p.fsr[1] & 0x0F) << 8);
        uint16_t f2 = (p.fsr[1] >> 4) | (p.fsr[2] << 4);
        lockData();
        lastRxMs[p.node_id] = millis();
        S.lastNode = p.node_id;
        if (p.node_id == BED_NODE_ID) {
          bool moved = false;
          if (havePrev) {
            int32_t dA = 0, gA = 0;
            for (int k = 0; k < 2; k++) {
              for (int a = 0; a < 3; a++) dA += abs(p.imu[k*6+a] - prevImu[k*6+a]);
              for (int g = 3; g < 6; g++) gA += abs(p.imu[k*6+g]);
            }
            moved = (dA > MOVE_ACCEL_DELTA) || (gA > MOVE_GYRO_ABS);
          }
          memcpy(prevImu, p.imu, sizeof(prevImu)); havePrev = true;
          win[winHead] = {moved, (uint16_t)((f1 + f2) / 2)};
          winHead = (winHead + 1) % WIN_N; if (winCount < WIN_N) winCount++;
          S.force1 = f1; S.force2 = f2;
          // features
          int mv = 0, run = 0, longest = 0; uint32_t fs = 0;
          for (int i = 0; i < winCount; i++) {
            WinSample &w = win[(winHead - winCount + i + WIN_N) % WIN_N];
            if (w.moved) { mv++; run = 0; } else { run++; if (run > longest) longest = run; }
            fs += w.force;
          }
          S.moveRatio = (float)mv / winCount;
          S.stillMin = longest * NODE_PERIOD_S / 60.0f;
          S.forceNorm = (float)fs / winCount / 4095.0f;
        }
        unlockData();
        if (nodeOffline[p.node_id]) {
          nodeOffline[p.node_id] = false;
          postEvent("nodeOnline", "sensor node back online");
        }
      }
    }
    // เช็ค node offline + bed freshness
    uint32_t now = millis();
    lockData();
    S.bedFresh = lastRxMs[BED_NODE_ID] && (now - lastRxMs[BED_NODE_ID] < BED_STALE_MS) && winCount > 0;
    unlockData();
    for (int n = 1; n <= MAX_NODES; n++) {
      if (lastRxMs[n] && !nodeOffline[n] && now - lastRxMs[n] > BED_STALE_MS) {
        nodeOffline[n] = true;
        postEvent("nodeOffline", "sensor node offline");
        postNotify("⚠️ **Smart Alarm:** Sensor node #" + String(n) + " ขาดการเชื่อมต่อ (ไม่ได้รับข้อมูลเกิน 90 วินาที)");
      }
    }
  }
}

void TaskAI(void *pv) {
  SleepClass pending = C_AWAKE_IN_BED; int streak = 0;
  for (;;) {
    float f[AI_FEATURES]; bool fresh = false, valid = false; uint32_t firedMs = 0; SleepClass cur = C_AWAKE_IN_BED;
    lockData();
    fresh = S.bedFresh; valid = S.aiValid; cur = S.ai; firedMs = S.alarmFiredMs;
    f[0] = S.moveRatio;
    f[1] = fminf(S.stillMin / 10.0f, 1.0f);
    f[2] = S.forceNorm;
    f[3] = fminf(S.pirRecent / 5.0f, 1.0f);
    f[4] = fminf(S.lux / 500.0f, 1.0f);
    unlockData();
    f[5] = firedMs ? fminf((millis() - firedMs) / 3600000.0f, 1.0f) : 0.0f;

    if (fresh) {
      float conf; SleepClass c = classify(f, &conf);
#if WOKWI_SIM
      if (simForceAI >= 0) { c = (SleepClass)simForceAI; conf = 1.0f; }
#endif
      SleepClass commit = cur; bool ok = valid;
      if (!valid) { commit = c; ok = true; streak = 0; }               // ครั้งแรก commit ทันที
      else if (c == cur) streak = 0;
      else { if (c == pending) streak++; else { pending = c; streak = 1; } if (streak >= 2) { commit = c; streak = 0; } }
      lockData(); S.ai = commit; S.aiValid = ok; unlockData();
      if (commit != cur || !valid) Serial.printf("[AI] raw=%s (%.2f) -> committed %s\n", CLASS_NAMES[c], conf, CLASS_NAMES[commit]);
    } else {
      lockData(); S.aiValid = false; unlockData();
    }
    vTaskDelay(pdMS_TO_TICKS(AI_PERIOD_MS));
  }
}

void drawRing(int cx, int cy, int r, float frac) {
  int steps = (int)(frac * 120);
  for (int i = 0; i < steps; i++) {
    float a = -PI / 2 + (2 * PI * i / 120.0f);
    for (int k = 0; k < 3; k++) display.drawPixel(cx + (r - k) * cosf(a), cy + (r - k) * sinf(a), SSD1306_WHITE);
  }
}

void drawDisplay(AlarmState st, bool phone, SleepClass ai, bool aiValid, uint16_t aMin, bool aSet) {
  if (!hasOLED) return;
  struct tm t; bool ok = getNow(t);
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(300)) != pdTRUE) return;
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
  if (emergencyActive) {
    uint32_t el = millis() - emergencyStartMs;
    drawRing(32, 32, 28, fminf((float)el / EMERGENCY_MS, 1.0f));
    uint32_t left = (el >= EMERGENCY_MS) ? 0 : (EMERGENCY_MS - el) / 1000;
    display.setTextSize(1); display.setCursor(70, 12); display.print("EMERGENCY");
    display.setTextSize(2); display.setCursor(66, 28); display.printf("%02u:%02u", (unsigned)(left / 60), (unsigned)(left % 60));
    display.setTextSize(1); display.setCursor(66, 52); display.print("press=cancel");
  } else {
    display.setTextSize(2); display.setCursor(0, 0);
    if (ok) display.printf("%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec); else display.print("--:--:--");
    display.setTextSize(1);
    display.setCursor(0, 22); display.printf("State: %s", STATE_NAMES[st]);
    display.setCursor(0, 34); display.printf("Alarm: %02d:%02d %s", aMin / 60, aMin % 60, aSet ? "[SET]" : "[off]");
    display.setCursor(0, 46); display.printf("Phone:%s AI:%s", phone ? "IN " : "OUT", aiValid ? CLASS_NAMES[ai] : "-");
    display.setCursor(0, 56); display.print(WiFi.status() == WL_CONNECTED ? (cloudOk ? "Cloud OK" : "WiFi OK") : "Offline");
  }
  display.display();
  xSemaphoreGive(i2cMutex);
}

void TaskUI(void *pv) {
  uint32_t lastTick = millis(), lastDisp = 0, lastSec = 0;
  int phoneDeb = 0; bool phone = (digitalRead(MICROSWITCH_PIN) == LOW);   // อ่านค่าจริงตั้งแต่รอบแรก กันปลดล็อกผิดหลังรีบูต
  bool btnDown = false, longFired = false; uint32_t btnDownMs = 0;
  bool buzzOn = false;
#if DBG_STATE
  int pSt = -1, pPhone = -1, pSet = -1, pAi = -1, pValid = -1, pEm = -1, pMin = -1;
  uint32_t lastVerPrint = 0;
#endif

  for (;;) {
    uint32_t now = millis(); uint32_t dt = now - lastTick; lastTick = now;

    int dist = -1; SleepClass ai = C_AWAKE_IN_BED; bool aiValid = false; uint16_t aMin = 0; bool aSet = false;
    lockData(); dist = S.distance; ai = S.ai; aiValid = S.aiValid; aMin = S.alarmMin; aSet = S.alarmSet; unlockData();

#if WOKWI_SIM
    int req = simAlarmReq;                                  // คำสั่ง 'w' จาก SimBed
    if (req >= 0) { simAlarmReq = -1; aMin = req; aSet = true; setAlarm(aMin, aSet); }
#endif

    // ---- phone detect (microswitch + ToF สำรอง) พร้อม debounce ----
    bool raw = (digitalRead(MICROSWITCH_PIN) == LOW) || (dist > 0 && dist < PHONE_TOF_MM);
    if (raw == phone) phoneDeb = 0; else if (++phoneDeb >= 3) { phone = raw; phoneDeb = 0; }

    // ---- PIR deferred ----
    if (pirFlag) { pirFlag = false; pirTimes[pirIdx] = now; pirIdx = (pirIdx + 1) % 16; Serial.println(">> PIR: motion"); }

    // ---- encoder rotate (deferred) ----
    int d;
    portENTER_CRITICAL(&isrMux); d = encDelta; encDelta = 0; portEXIT_CRITICAL(&isrMux);
    if (d != 0) {
      int a = aMin;
      if (state == ST_IDLE) { a = ((a + d * ALARM_STEP_MIN) % 1440 + 1440) % 1440; }
      else if ((state == ST_LOCKED || state == ST_SLEPT) && d > 0) { a = (a + d * ALARM_STEP_MIN) % 1440; } // หลังล็อก: เลื่อนได้อย่างเดียว
      if (a != aMin) { aMin = a; setAlarm(aMin, aSet); }
    }

    // ---- encoder button: short / long ----
    if (btnFlag) { btnFlag = false; btnDown = true; longFired = false; btnDownMs = now; }
    if (btnDown) {
      bool low = (digitalRead(ENC_SW) == LOW);
      if (low && !longFired && now - btnDownMs >= LONG_PRESS_MS) {
        longFired = true;
        if (!emergencyActive && (state == ST_SLEPT || state == ST_ALARM || state == ST_UP)) {
          emergencyActive = true; emergencyStartMs = now;
          postEvent("emergencyStart", "emergency countdown started");
        }
      }
      if (!low) {
        if (!longFired) {
          if (emergencyActive) { emergencyActive = false; postEvent("emergencyCancel", ""); }
          else if (state == ST_IDLE) { aSet = !aSet; setAlarm(aMin, aSet); }
        }
        btnDown = false;
      }
    }

    // ---- emergency countdown ----
    if (emergencyActive) {
      if (state == ST_IDLE || state == ST_UNLOCKED) emergencyActive = false;
      else if (now - emergencyStartMs >= EMERGENCY_MS) {
        emergencyActive = false;
        servoCmd(UNLOCK_ANGLE); enterState(ST_UNLOCKED);
        postEvent("emergencyUnlock", "emergency unlock executed");
        postNotify("🆘 **Smart Alarm:** มีการปลดล็อกฉุกเฉิน (Emergency Unlock) กล่องเก็บโทรศัพท์ถูกเปิดแล้ว");
      }
    }

    // ---- alarm timer ----
    bool due = false;
    if (state == ST_LOCKED || state == ST_SLEPT) {
      struct tm t;
      if (getNow(t)) {
        int m = t.tm_hour * 60 + t.tm_min, key = t.tm_mday * 1440 + m;
        if (m == aMin && key != lastDueKey) { due = true; lastDueKey = key; }
      }
    }

    // ---- state machine (ตาม PlantUML) ----
    switch (state) {
      case ST_IDLE:
        if (aSet && phone) {                               // timed AND phoneInserted
          verifiedMs = 0; sleepStartMs = 0;                // เริ่มรอบใหม่: ล้างค่าค้างจากรอบก่อน
          servoCmd(LOCK_ANGLE); enterState(ST_LOCKED);
          postEvent("locked", "phone locked");
        }
        break;
      case ST_LOCKED:
        if (!phone) {                                      // ความผิดปกติ: สวิตช์ไม่เห็นมือถือ -> ปล่อยล็อก
          servoCmd(UNLOCK_ANGLE); enterState(ST_IDLE);
        } else if (aiValid && ai == C_ASLEEP) {            // userSlept
          sleepStartMs = now; verifiedMs = 0;
          enterState(ST_SLEPT); postEvent("sleepBegin", "user fell asleep");
        } else if (due) {                                  // กันล็อกค้างถ้าไม่ได้หลับ
          alarmFiredMs = now; enterState(ST_ALARM);
          postEvent("alarmRing", "alarm while not asleep");
        }
        break;
      case ST_SLEPT:
        if (due) { alarmFiredMs = now; enterState(ST_ALARM); postEvent("alarmRing", ""); }  // timerFinished
        break;
      case ST_ALARM:
        if (aiValid && ai == C_UP) enterState(ST_UP);      // userWokeUp
        break;
      case ST_UP:
        if (aiValid && ai != C_UP) {                       // userBackToSleep: พักตัวนับ + ปลุกใหม่
          enterState(ST_ALARM); postEvent("userBackToSleep", "");
        } else if (aiValid) {
          verifiedMs += dt;
          if (verifiedMs >= VERIFY_AWAKE_MS) {             // userWokeUpAboveThreshold
            float tNow, hNow;
            lockData(); tNow = S.temp; hNow = S.hum; unlockData();
            servoCmd(UNLOCK_ANGLE); enterState(ST_UNLOCKED);
            postEvent("sleepEnd", "wake verified, unlocked");
            String s = "☀️ **Morning summary:** ";
            if (sleepStartMs) { uint32_t m = (now - sleepStartMs) / 60000; s += "นอนหลับ/อยู่บนเตียง " + String(m / 60) + " ชม. " + String(m % 60) + " นาที, "; }
            s += "อุณหภูมิห้อง " + String(tNow, 1) + "°C, ความชื้น " + String(hNow, 0) + "% — ปลดล็อกโทรศัพท์แล้ว";
            postNotify(s);
            sleepStartMs = 0;
          }
        }
        break;
      case ST_UNLOCKED:
        if (!phone) { aSet = false; setAlarm(aMin, false, false); enterState(ST_IDLE); }   // phoneRemoved
        break;
    }

    // ---- servo timeout stop ----
    if (servoDetachAt && now > servoDetachAt) { lockServo.detach(); servoDetachAt = 0; }

    // ---- buzzer ----
    bool want = (state == ST_ALARM) && ((now / 400) % 2 == 0);
    if (want && !buzzOn) { tone(BUZZER_PIN, 2000); buzzOn = true; }
    if (!want && buzzOn) { noTone(BUZZER_PIN); buzzOn = false; }

    // ---- 1 Hz housekeeping ----
    if (now - lastSec >= 1000) {
      lastSec = now;
      int c = 0; for (int i = 0; i < 16; i++) if (pirTimes[i] && now - pirTimes[i] < 300000UL) c++;
      uint32_t left = 0;
      if (emergencyActive) { uint32_t el = now - emergencyStartMs; left = (el >= EMERGENCY_MS) ? 0 : (EMERGENCY_MS - el) / 1000; }
      lockData();
      S.pirRecent = c; S.phoneInserted = phone; S.alarmFiredMs = (state == ST_ALARM || state == ST_UP) ? alarmFiredMs : 0;
      S.emergencyLeftS = left;
      unlockData();

#if DBG_STATE
      bool changed = ((int)state != pSt) || ((int)phone != pPhone) || ((int)aSet != pSet) ||
                     ((int)ai != pAi) || ((int)aiValid != pValid) || ((int)emergencyActive != pEm) || ((int)aMin != pMin);
      bool verTick = (state == ST_UP) && (now - lastVerPrint >= 10000);
      if (changed || verTick) {
        lastVerPrint = now;
        pSt = state; pPhone = phone; pSet = aSet; pAi = ai; pValid = aiValid; pEm = emergencyActive; pMin = aMin;
        Serial.printf("[DBG] st=%s phone=%d alarmSet=%d alarmMin=%d ai=%s valid=%d verified=%lu/%lu emerg=%d\n",
          STATE_NAMES[state], phone, aSet, aMin, CLASS_NAMES[ai], aiValid,
          (unsigned long)verifiedMs, (unsigned long)VERIFY_AWAKE_MS, emergencyActive);
      }
#endif
    }
    if (now - lastDisp >= 250) { lastDisp = now; drawDisplay(state, phone, ai, aiValid, aMin, aSet); }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ---------- Cloud ----------
void appendLogToFile(const String& payload) {
  if (!fsOk) return;
  Serial.println("[Queue] offline -> stored to flash");
  if (LittleFS.exists(Q_FILE)) {
    File f = LittleFS.open(Q_FILE, FILE_READ);
    size_t sz = f ? f.size() : 0; if (f) f.close();
    if (sz > Q_MAX_BYTES) LittleFS.remove(Q_FILE);       // กันแฟลชเต็ม: เริ่มคิวใหม่
  }
  File file = LittleFS.open(Q_FILE, FILE_APPEND);
  if (!file) return;
  file.println(payload); file.close();
}

void flushOfflineData() {
  if (!fsOk || !LittleFS.exists(Q_FILE)) return;
  Serial.println("[Queue] online -> flushing stored data");
  File in = LittleFS.open(Q_FILE, FILE_READ); if (!in) return;
  File out; bool failed = false;
  while (in.available()) {
    String line = in.readStringUntil('\n'); line.trim();
    if (!line.length()) continue;
    if (!failed) {
      if (mqtt.publish(MQTT_TOPIC, line.c_str())) { mqtt.loop(); vTaskDelay(pdMS_TO_TICKS(100)); continue; }
      failed = true; out = LittleFS.open(Q_TMP, FILE_WRITE);
    }
    if (out) out.println(line);
  }
  in.close(); if (out) out.close();
  LittleFS.remove(Q_FILE);
  if (failed) LittleFS.rename(Q_TMP, Q_FILE);
}

String buildPayload(const char* action, const char* note) {
  Shared s;
  lockData(); s = S; unlockData();
  String p; p.reserve(640);
  p = "{\"data\":{";
  p += "\"action\":\"" + String(action) + "\",";
  if (note && note[0]) p += "\"note\":\"" + jsonEscape(note) + "\",";
  p += "\"timestamp\":\"" + isoNow() + "\",";
  p += "\"tempCelsius\":" + String(s.temp, 1) + ",";
  p += "\"humidityPercent\":" + String(s.hum, 1) + ",";
  p += "\"lightIntensity\":" + String(s.lux, 0) + ",";
  p += "\"distanceMm\":" + String(s.distance) + ",";
  p += "\"phoneInserted\":" + String(s.phoneInserted ? "true" : "false") + ",";
  p += "\"node_id\":" + String(s.lastNode) + ",";
  p += "\"bed_force1\":" + String(s.force1) + ",";
  p += "\"bed_force2\":" + String(s.force2) + ",";
  p += "\"bed_move_ratio\":" + String(s.moveRatio, 2) + ",";
  p += "\"bed_still_min\":" + String(s.stillMin, 1) + ",";
  p += "\"ai_class\":\"" + String(s.aiValid ? CLASS_NAMES[s.ai] : "NONE") + "\",";
  p += "\"alarm_state\":\"" + String(STATE_NAMES[s.state]) + "\",";
  char at[8]; snprintf(at, sizeof(at), "%02d:%02d", s.alarmMin / 60, s.alarmMin % 60);
  p += "\"alarm_time\":\"" + String(at) + "\"";
  p += "}}";
  return p;
}

void publishOrStore(const String& payload, bool connected, bool isTelemetry) {
  if (connected && mqtt.publish(MQTT_TOPIC, payload.c_str())) {
    if (LOG_TELEMETRY || !isTelemetry) Serial.println("[Cloud] " + payload);
  } else {
    static uint32_t lastTeleStore = 0;
    if (isTelemetry) {                                     // ออฟไลน์: telemetry เก็บถี่แค่นาทีละครั้ง กันไฟล์เต็มแล้วถูกลบทิ้งทั้งก้อน
      uint32_t now = millis();
      if (lastTeleStore && now - lastTeleStore < OFFLINE_TELE_MS) return;
      lastTeleStore = now;
    }
    appendLogToFile(payload);                              // store-and-forward (E4)
  }
}

void TaskCloud(void *pv) {
  uint32_t lastWifi = 0, lastMqtt = 0, lastTele = 0;
  bool prevConn = false;
  for (;;) {
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (now - lastWifi > 15000) { lastWifi = now; WiFi.begin(NET_SSID, NET_PASS, NET_CH); }   // self-recovery
    } else if (!mqtt.connected()) {
      if (now - lastMqtt > 5000) { lastMqtt = now; mqtt.connect(MQTT_CLIENT_ID, MQTT_TOKEN, MQTT_SECRET); }
    }
    mqtt.loop();
    bool conn = (WiFi.status() == WL_CONNECTED) && mqtt.connected();
    cloudOk = conn;
    if (conn && !prevConn) flushOfflineData();
    prevConn = conn;

    // sync RTC จาก NTP ครั้งเดียวหลังต่อเน็ตได้
    if (hasRTC && !rtcSynced && WiFi.status() == WL_CONNECTED) {
      struct tm t;
      if (getLocalTime(&t, 0) && t.tm_year + 1900 >= 2024) {
        if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          rtc.adjust(DateTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec));
          xSemaphoreGive(i2cMutex); rtcSynced = true; Serial.println("[RTC] synced from NTP");
        }
      }
    }

    CloudEvent ev;
    while (xQueueReceive(eventQueue, &ev, 0) == pdPASS) publishOrStore(buildPayload(ev.action, ev.note), conn, false);

    if (now - lastTele >= TELEMETRY_MS) {
      lastTele = now;
      publishOrStore(buildPayload("telemetry", ""), conn, true);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------- Discord (HTTPS) ----------
bool sendDiscord(const String& text) {
#if WOKWI_SIM && DISCORD_SIM_ONLY
  Serial.println("[Discord-SIM] " + text);                  // Wokwi จำลอง TLS ช้า/หลุดบ่อย จึงแค่พิมพ์ข้อความที่จะส่ง
  return true;
#else
  WiFiClientSecure c;
#ifdef DISCORD_ROOT_CA
  c.setCACert(DISCORD_ROOT_CA);
#else
  c.setInsecure();                                         // TODO: ใส่ DISCORD_ROOT_CA ใน secrets.h (E5)
#endif
  c.setHandshakeTimeout(30);                               // วินาที
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(c, DISCORD_WEBHOOK_URL)) { Serial.println("[Discord] http.begin FAILED (url ผิดรูปแบบ?)"); return false; }
  http.addHeader("Content-Type", "application/json");
  String body = "{\"content\":\"" + jsonEscape(text.c_str()) + "\"}";
  uint32_t t0 = millis();
  int code = http.POST(body);
  Serial.printf("[Discord] code %d (%lu ms)\n", code, (unsigned long)(millis() - t0));
  if (code < 0) { char eb[128]; int e = c.lastError(eb, sizeof(eb)); Serial.printf("[TLS] err=%d %s\n", e, eb); }
  http.end();
  return code >= 200 && code < 300;
#endif
}

void TaskNotify(void *pv) {
  NotifyItem it;
  for (;;) {
    if (xQueueReceive(notifyQueue, &it, portMAX_DELAY) == pdPASS) {
      for (int w = 0; w < 12 && WiFi.status() != WL_CONNECTED; w++) vTaskDelay(pdMS_TO_TICKS(5000));   // รอเน็ตสูงสุด ~1 นาที (เช่น ไฟกลับมาหลังดับ)
      bool sent = false;
      const int tries = WOKWI_SIM ? 1 : 5;                  // Wokwi ลองครั้งเดียว ไม่ให้คิวค้าง
      for (int a = 0; a < tries && !sent; a++) {
        if (WiFi.status() == WL_CONNECTED) sent = sendDiscord(String(it.text));
        if (!sent && a + 1 < tries) vTaskDelay(pdMS_TO_TICKS(5000));
      }
      if (!sent) Serial.println("[Notify] dropped");
    }
  }
}

#if WOKWI_SIM
// Wokwi ไม่มี ESP-NOW: จำลอง Sensor Node ผ่านการพิมพ์ใน Serial Monitor
//   z = หลับ   b = ตื่นอยู่บนเตียง   u = ลุกจากเตียง   m = โมเดลจริง   w = ตั้งปลุกอีก 2 นาที
//   d = ทดสอบ Discord   x = หยุด/เริ่มส่งข้อมูล node (ทดสอบ nodeOffline หลัง 90 วิ)
char simMode = 'b';
void TaskSimBed(void *pv) {
  for (;;) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == 'z' || c == 'b' || c == 'u') {
        simMode = c;
        simForceAI = (c == 'z') ? C_ASLEEP : (c == 'b') ? C_AWAKE_IN_BED : C_UP;
        Serial.printf("[SIM] bed = %c (AI forced)\n", c);
      }
      else if (c == 'm') { simForceAI = -1; Serial.println("[SIM] AI = real model"); }
      else if (c == 'w') {
        struct tm t;
        if (getNow(t)) {
          int m = (t.tm_hour * 60 + t.tm_min + 2) % 1440;
          simAlarmReq = m;                                    // TaskUI เป็นคนเขียนจริง
          Serial.printf("[SIM] alarm set %02d:%02d\n", m / 60, m % 60);
        } else Serial.println("[SIM] time not ready yet (รอ NTP)");
      }
      else if (c == 'd') { postNotify("🔔 ทดสอบ Discord จาก Wokwi"); Serial.println("[SIM] test notify queued"); }
      else if (c == 'x') { simPause = !simPause; Serial.printf("[SIM] node %s\n", simPause ? "PAUSED" : "RESUMED"); }
    }
    if (!simPause) {
      RxItem it; memset(&it, 0, sizeof(it));
      it.pkt.node_id = BED_NODE_ID;
      long jit = (simMode == 'b') ? 6000 : 0;
      for (int i = 0; i < 12; i++) it.pkt.imu[i] = (i % 6 < 3) ? (int16_t)random(-jit, jit + 1) : 0;
      uint16_t f = (simMode == 'u') ? 100 : 3000;
      it.pkt.fsr[0] = f & 0xFF;
      it.pkt.fsr[1] = ((f >> 8) & 0x0F) | ((f & 0x0F) << 4);
      it.pkt.fsr[2] = f >> 4;
      xQueueSend(espnowQueue, &it, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
#endif

// ======================= SETUP =======================
void restoreState() {
  prefs.begin("alarm", false);
  S.alarmMin = prefs.getUShort("am", 6 * 60);
  S.alarmSet = prefs.getBool("as", false);
  uint8_t st = prefs.getUChar("st", ST_IDLE);
  verifiedMs = prefs.getUInt("vm", 0);
  if (st > ST_UNLOCKED) st = ST_IDLE;
  state = (AlarmState)st; S.state = st;
  if (state == ST_LOCKED || state == ST_SLEPT || state == ST_ALARM || state == ST_UP) {
    servoCmd(LOCK_ANGLE);                                    // ไฟดับแล้วกลับมา: ต้องคงล็อกไว้
    if (state == ST_ALARM || state == ST_UP) alarmFiredMs = millis();
    Serial.printf("[BOOT] restored %s, keeping lock\n", STATE_NAMES[state]);
  } else {
    servoCmd(UNLOCK_ANGLE);
  }
  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) {
    postEvent("powerRecovered", "gateway restarted after power loss");
    postNotify("⚡ **Smart Alarm:** Gateway เริ่มทำงานใหม่หลังไฟดับ/ไฟตก — สถานะล่าสุด: " + String(STATE_NAMES[state]));
  }
}

void setup() {
  Serial.begin(115200);
  dataMutex = xSemaphoreCreateMutex();
  i2cMutex = xSemaphoreCreateMutex();
  espnowQueue = xQueueCreate(10, sizeof(RxItem));
  eventQueue = xQueueCreate(16, sizeof(CloudEvent));
  notifyQueue = xQueueCreate(8, sizeof(NotifyItem));
  memset(&S, 0, sizeof(S)); S.ai = C_AWAKE_IN_BED; S.distance = -1;

  Wire.begin();
  dht.begin();
  fsOk = LittleFS.begin(true);

  pinMode(MICROSWITCH_PIN, INPUT_PULLUP);
  pinMode(ENC_CLK, INPUT_PULLUP); pinMode(ENC_DT, INPUT_PULLUP); pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(PIR_PIN, INPUT); pinMode(BUZZER_PIN, OUTPUT);

  ESP32PWM::allocateTimer(0);
  lockServo.setPeriodHertz(50);

  hasBH1750 = lightMeter.begin();
  hasVL53L0X = lox.begin();
  hasRTC = rtc.begin();
  Wire.beginTransmission(0x3C);                              // เช็คก่อนว่ามีจอต่ออยู่จริง
  hasOLED = (Wire.endTransmission() == 0) && display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  Serial.printf("[HW] BH1750=%d VL53L0X=%d RTC=%d OLED=%d FS=%d\n", hasBH1750, hasVL53L0X, hasRTC, hasOLED, fsOk);

  restoreState();

  attachInterrupt(digitalPinToInterrupt(PIR_PIN), pirISR, RISING);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encRotateISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(ENC_SW), encButtonISR, FALLING);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(NET_SSID, NET_PASS, NET_CH);
  configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");

#if WOKWI_SIM
  // plain MQTT ไม่ต้องตั้งค่า TLS
#elif defined(NETPIE_ROOT_CA)
  espClient.setCACert(NETPIE_ROOT_CA);
#else
  espClient.setInsecure();                                   // TODO: ใส่ NETPIE_ROOT_CA เพื่อ verify cert (E5)
#endif
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  if (!mqtt.setBufferSize(1024)) Serial.println("[MQTT] setBufferSize failed");   // default 256 ไม่พอสำหรับ payload
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);

#if !WOKWI_SIM
  // หมายเหตุ: ESP-NOW ต้องใช้ช่อง WiFi เดียวกับ AP/hotspot -> ตั้งช่องฝั่ง Sensor Node ให้ตรง
  if (esp_now_init() != ESP_OK) Serial.println("Error init ESP-NOW");
  esp_now_register_recv_cb(OnDataRecv);
#endif

  xTaskCreatePinnedToCore(TaskSensor, "Sensor", 4096,  NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskESPNOW, "ESPNOW", 4096,  NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(TaskAI,     "AI",     10240, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskUI,     "UI",     8192,  NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskCloud,  "Cloud",  12288, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskNotify, "Notify", 16384, NULL, 0, NULL, 0);   // priority 0: TLS handshake ไม่แย่ง idle task จน watchdog ฟ้อง
#if WOKWI_SIM
  xTaskCreatePinnedToCore(TaskSimBed, "SimBed", 4096,  NULL, 1, NULL, 1);
#endif
}

void loop() { vTaskDelete(NULL); }
