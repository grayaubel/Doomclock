// notify.cpp — Discord webhook over HTTPS (M6). Lowest priority task: a slow TLS
// handshake must never delay sensing or the ESP-NOW ACKs.
#include "shared.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"

static bool sendDiscord(const char* text) {
  WiFiClientSecure c;
#ifdef DISCORD_ROOT_CA
  c.setCACert(DISCORD_ROOT_CA);
#else
  c.setInsecure();
#endif
  c.setHandshakeTimeout(20);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(c, DISCORD_WEBHOOK_URL)) { Serial.println("[DISCORD] bad webhook URL"); return false; }
  http.addHeader("Content-Type", "application/json");

  String body = "{\"content\":\"";
  for (const char* p = text; *p; p++) {
    if (*p == '"' || *p == '\\') { body += '\\'; body += *p; }
    else if (*p == '\n') body += "\\n";
    else if ((uint8_t)*p >= 0x20) body += *p;
  }
  body += "\"}";
  int code = http.POST(body);
  http.end();
  Serial.printf("[DISCORD] HTTP %d\n", code);
  return code >= 200 && code < 300;
}

static bool webhookConfigured() {
  return strncmp(DISCORD_WEBHOOK_URL, "https://discord.com/api/webhooks/", 33) == 0 &&
         strstr(DISCORD_WEBHOOK_URL, "/xxx/") == nullptr;
}

void TaskNotify(void*) {
  NotifyMsg n;
  bool warned = false;
  for (;;) {
    if (xQueueReceive(gNotifyQ, &n, portMAX_DELAY) != pdTRUE) continue;
    if (!webhookConfigured()) {
      if (!warned) Serial.println("[DISCORD] DISCORD_WEBHOOK_URL not set in secrets.h — notifications skipped");
      warned = true;
      continue;
    }
    for (int w = 0; w < 12 && WiFi.status() != WL_CONNECTED; w++) vTaskDelay(pdMS_TO_TICKS(5000));   // wait up to 1 min for Wi-Fi
    bool sent = false;
    for (int a = 0; a < 4 && !sent; a++) {
      if (WiFi.status() == WL_CONNECTED) sent = sendDiscord(n.text);
      if (!sent) vTaskDelay(pdMS_TO_TICKS(5000));
    }
    if (!sent) Serial.println("[DISCORD] gave up on one message");
  }
}
