// cloud.cpp — Wi-Fi + NETPIE MQTT over TLS, with store-and-forward on LittleFS (E4, E5).
// Records that can't be published go to /q.jsonl (one JSON per line). When the
// connection comes back the file is replayed in small chunks, so live data keeps
// flowing during a long replay, and the replay position survives a reboot.
#include "shared.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include "secrets.h"

static WiFiClientSecure tls;
static PubSubClient mqtt(tls);
static bool fsOk = false;
static size_t flushPos = 0;
volatile bool gCloudOk = false;
volatile uint32_t gOfflineStored = 0, gOfflineDropped = 0;

static void storeOffline(const char* json) {
  if (!fsOk) { gOfflineDropped++; return; }
  File f = LittleFS.open(Q_FILE, FILE_APPEND);
  if (!f) { gOfflineDropped++; return; }
  if (f.size() > Q_MAX_BYTES) { f.close(); gOfflineDropped++; return; }   // full: keep the oldest data, drop new
  f.println(json);
  f.close();
  gOfflineStored++;
}

static void savePos() {
  File f = LittleFS.open(Q_POS_FILE, FILE_WRITE);
  if (f) { f.print((unsigned long)flushPos); f.close(); }
}

// Replays up to `maxLines` stored records. Returns true when the queue file is empty.
static bool flushChunk(int maxLines) {
  if (!fsOk || !LittleFS.exists(Q_FILE)) return true;
  File in = LittleFS.open(Q_FILE, FILE_READ);
  if (!in) return true;
  if (flushPos > in.size()) flushPos = 0;
  in.seek(flushPos);
  int sent = 0;
  while (sent < maxLines && in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() && !mqtt.publish(MQTT_TOPIC, line.c_str())) { in.close(); savePos(); return false; }
    flushPos = in.position();
    sent++;
    mqtt.loop();
  }
  bool done = !in.available();
  in.close();
  if (done) {
    LittleFS.remove(Q_FILE);
    if (LittleFS.exists(Q_POS_FILE)) LittleFS.remove(Q_POS_FILE);
    flushPos = 0;
    Serial.println("[CLOUD] offline queue fully replayed");
  } else {
    savePos();
  }
  return done;
}

static void publishOrStore(const char* json) {
  if (gCloudOk && mqtt.publish(MQTT_TOPIC, json)) return;
  storeOffline(json);
}

void cloudBegin() {
  fsOk = LittleFS.begin(true);
  if (fsOk && LittleFS.exists(Q_POS_FILE)) {
    File f = LittleFS.open(Q_POS_FILE, FILE_READ);
    if (f) { flushPos = f.readString().toInt(); f.close(); }
  }
  Serial.printf("[CLOUD] LittleFS %s, pending queue %s\n", fsOk ? "ok" : "FAILED",
                fsOk && LittleFS.exists(Q_FILE) ? "yes" : "no");

#ifdef NETPIE_ROOT_CA
  tls.setCACert(NETPIE_ROOT_CA);
#else
  tls.setInsecure();
  Serial.println("[CLOUD] WARNING: NETPIE_ROOT_CA not set — TLS encrypts but does not verify the server (E5)");
#endif
  tls.setHandshakeTimeout(15);
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setBufferSize(1400);              // records are 700-1000 bytes; the default 256 is too small
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
}

void TaskCloud(void*) {
  uint32_t lastWifiTry = 0, lastMqttTry = 0, lastTele = 0;
  bool prevOk = false, replaying = false;
  for (;;) {
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (now - lastWifiTry > 15000) {                    // self-recovery after router/hotspot loss
        lastWifiTry = now;
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        Serial.println("[CLOUD] Wi-Fi reconnecting ...");
      }
    } else if (!mqtt.connected() && now - lastMqttTry > 5000) {
      lastMqttTry = now;
      if (mqtt.connect(MQTT_CLIENT_ID, MQTT_TOKEN, MQTT_SECRET)) Serial.println("[CLOUD] NETPIE connected");
      else Serial.printf("[CLOUD] NETPIE connect failed, state %d\n", mqtt.state());
    }
    mqtt.loop();
    gCloudOk = WiFi.status() == WL_CONNECTED && mqtt.connected();
    if (gCloudOk && !prevOk) replaying = true;
    prevOk = gCloudOk;

    clockService();

    CloudMsg m;
    while (xQueueReceive(gCloudQ, &m, 0) == pdTRUE) publishOrStore(m.json);

    if (gCloudOk && replaying) replaying = !flushChunk(10);

    // gateway-only telemetry while no bed records are flowing (node off / out of range)
    if (now - lastTele >= TELEMETRY_MS && now - gLastBedPostMs >= TELEMETRY_MS + 2000) {
      lastTele = now;
      buildRecord(m.json, sizeof(m.json), "telemetry", "", false);
      publishOrStore(m.json);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
