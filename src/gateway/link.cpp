// link.cpp — ESP-NOW receiver: validate, ACK, de-duplicate, count losses, hand off to the cloud.
// The receive callback runs in the Wi-Fi task, so it only copies the frame into a queue;
// TaskLink (priority 3) does the work and sends the ACK within a few ms.
#include "shared.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_arduino_version.h>

struct RxItem { uint8_t mac[6]; int len; uint8_t data[sizeof(BedPacket)]; };
static QueueHandle_t rxQ;

static void handleRecv(const uint8_t* mac, const uint8_t* data, int len) {
  RxItem it;
  memcpy(it.mac, mac, 6);
  it.len = len;
  memcpy(it.data, data, len > (int)sizeof(it.data) ? sizeof(it.data) : len);
  xQueueSend(rxQ, &it, 0);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) { handleRecv(info->src_addr, data, len); }
#else
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) { handleRecv(mac, data, len); }
#endif

static void sendAck(const uint8_t* mac, const BedPacket& p) {
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;                       // current channel = the hotspot's channel
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }
  AckPacket a = {};
  a.magic = DOOM_MAGIC; a.ver = DOOM_PROTO_VER; a.type = DOOM_PKT_ACK;
  a.node_id = p.node_id; a.boot_id = p.boot_id; a.seq = p.seq;
  a.unix_time = timeValid() ? (uint32_t)nowUtc() : 0;
  doom_seal(a);
  esp_now_send(mac, (const uint8_t*)&a, sizeof(a));
}

// Count and (rate-limited) print frames that are not ours: another ESP-NOW device nearby,
// or a node still running an old firmware with a different packet format.
static void noteBad(const RxItem& it, const char* why) {
  static uint32_t lastPrint = 0;
  lockState(); gLink.bad++; unlockState();
  if (millis() - lastPrint < 5000) return;
  lastPrint = millis();
  Serial.printf("[LINK] ignored frame from %02X:%02X:%02X:%02X:%02X:%02X, %d bytes (%s; ours is %u bytes), first bytes %02X %02X %02X\n",
                it.mac[0], it.mac[1], it.mac[2], it.mac[3], it.mac[4], it.mac[5], it.len, why,
                (unsigned)sizeof(BedPacket), it.data[0], it.data[1], it.data[2]);
}

void linkBegin() {
  rxQ = xQueueCreate(12, sizeof(RxItem));
  if (esp_now_init() != ESP_OK) { Serial.println("[LINK] esp_now_init FAILED"); return; }
  esp_now_register_recv_cb(onRecv);
  Serial.printf("[LINK] gateway MAC %s — ESP-NOW listening\n", WiFi.macAddress().c_str());
}

void TaskLink(void*) {
  RxItem it;
  bool announcedOffline = false;
  for (;;) {
    if (xQueueReceive(rxQ, &it, pdMS_TO_TICKS(1000)) == pdTRUE) {
      BedPacket p;
      if (it.len != (int)sizeof(BedPacket)) { noteBad(it, "wrong size"); continue; }
      memcpy(&p, it.data, sizeof(p));
      if (!doom_check(p) || p.type != DOOM_PKT_BED) { noteBad(it, "bad magic/version/CRC"); continue; }

      sendAck(it.mac, p);                   // ACK duplicates too, otherwise the node keeps resending

      bool unique = true, cameBack = false;
      lockState();
      if (gLink.rx == 0 || p.boot_id != gLink.bootId) {          // first packet or node rebooted
        gLink.bootId = p.boot_id;
      } else if (p.seq <= gLink.lastSeq) {
        gLink.dup++; unique = false;
      } else {
        gLink.lost += p.seq - gLink.lastSeq - 1;                   // windows the node gave up on
      }
      if (unique) {
        gLink.lastSeq = p.seq;
        gLink.rx++;
        gBed.last = p;
        gBed.have = true;
        gBed.rxMs = millis();
        gBed.windowEnd = timeValid() ? nowUtc() - p.age_s : 0;
        cameBack = !gLink.online;
        gLink.online = true;
      }
      unlockState();

      if (unique) {
        postBedRecord();
        if (cameBack) {
          postEvent("nodeOnline", "bed node online");
          if (announcedOffline) postNotify("✅ **Doomclock:** Bed node กลับมาเชื่อมต่อแล้ว");
          announcedOffline = false;
        }
      }
    }

    // offline detection
    bool goneOffline = false;
    lockState();
    if (gLink.online && millis() - gBed.rxMs > NODE_OFFLINE_MS) { gLink.online = false; goneOffline = true; }
    unlockState();
    if (goneOffline) {
      postEvent("nodeOffline", "no bed packet for 35 s");
      postNotify("⚠️ **Doomclock:** Bed node ขาดการเชื่อมต่อ (ไม่ได้รับข้อมูลเกิน 35 วินาที)");
      announcedOffline = true;
    }
  }
}
