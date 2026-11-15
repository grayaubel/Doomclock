// config.h — gateway pins and tunables (ESP32 DevKit V1)
#pragma once

// ---------------- pins in use now ----------------
#define PIN_SDA            21     // I2C: BH1750 (0x23), DS3231 (0x68)
#define PIN_SCL            22
#define PIN_DHT            27     // DHT11 data
#define PIN_HALL_AO        34     // KY-024 analog out (ADC1 — ADC2 can't be used with Wi-Fi). Power KY-024 from 3V3!
#define PIN_HALL_DO        35     // KY-024 digital out (comparator, threshold set by its pot)

// ---------------- reserved for the next stage (UI / lock) ----------------
#define PIN_ENC_CLK        32     // KY-040
#define PIN_ENC_DT         33
#define PIN_ENC_SW         25
#define PIN_BUTTON         26     // KY-004 (AI label button)
#define PIN_BUZZER         13     // KY-006 passive buzzer
#define PIN_SERVO          16     // MG90S signal (power the servo from 5 V, not from the ESP32)
// ILI9488 3.5" SPI + XPT2046 touch (VSPI):
//   SCK 18, MOSI 23, MISO 19 (touch T_DO only — leave the TFT's SDO/MISO pin unconnected),
//   TFT_CS 5, TFT_DC 2, TFT_RST 4, TOUCH_CS 15, TOUCH_IRQ 36

// ---------------- behaviour ----------------
#define NODE_OFFLINE_MS    35000UL   // 3.5 missed windows -> node offline event
#define BED_FRESH_MS       25000UL
#define TELEMETRY_MS       10000UL   // gateway-only record when no bed record went out recently

#define HALL_BASELINE_DEF  1950      // ADC counts with the tray UP (no magnet). Calibrate with serial 'c'
#define HALL_ON_DELTA      300       // |raw - baseline| above this  -> tray down
#define HALL_OFF_DELTA     200       // below this                   -> tray up (hysteresis)
#define HALL_DEBOUNCE      3         // consecutive 100 ms samples

#define MQTT_SERVER        "mqtt.netpie.io"
#define MQTT_PORT          1884      // NETPIE: 1883 = plain MQTT, 1884 = MQTTS (TLS)
#define MQTT_TOPIC         "@shadow/data/update"

#define Q_FILE             "/q.jsonl"   // store-and-forward queue (LittleFS)
#define Q_POS_FILE         "/q.pos"
#define Q_MAX_BYTES        (600UL * 1024UL)   // ~2.5 h of 10 s records (LittleFS partition is 896 KB with huge_app.csv)
