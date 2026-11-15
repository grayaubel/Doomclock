// Copy this file to include/secrets.h and fill in real values.
// include/secrets.h is in .gitignore — never commit it (requirement E5).
#pragma once

#define WIFI_SSID           "your-hotspot"
#define WIFI_PASSWORD       "your-password"

// NETPIE 2020 device credentials (Device -> Key)
#define MQTT_CLIENT_ID      "netpie-client-id"
#define MQTT_TOKEN          "netpie-token"
#define MQTT_SECRET         "netpie-secret"

#define DISCORD_WEBHOOK_URL "https://discord.com/api/webhooks/xxx/yyy"

// Optional but needed for full E5 marks: root CA certificates (PEM) so the
// gateway verifies who it talks to. Without them TLS still encrypts but does
// not authenticate the server (setInsecure). See README for how to get them.
// #define NETPIE_ROOT_CA R"EOF(
// -----BEGIN CERTIFICATE-----
// ...
// -----END CERTIFICATE-----
// )EOF"
// #define DISCORD_ROOT_CA R"EOF(
// ...
// )EOF"
