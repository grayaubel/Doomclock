// config.h — gateway pins and tunables (ESP32 DevKit V1)
#pragma once

// ---------------- pins ----------------
#define PIN_SDA            21     // I2C: BH1750 (0x23), DS3231 (0x68)
#define PIN_SCL            22
#define PIN_DHT            27     // DHT11 data
#define PIN_HALL_AO        34     // KY-024 analog out (ADC1 — ADC2 can't be used with Wi-Fi). Power KY-024 from 3V3!
#define PIN_HALL_DO        35     // KY-024 digital out (comparator, threshold set by its pot)
#define PIN_ENC_CLK        32     // KY-040 (power from 3V3: its pull-ups go to +)
#define PIN_ENC_DT         33
#define PIN_ENC_SW         25
#define PIN_BUTTON         26     // KY-004: label "I'm up" (data mode), eject outside bedtime, hold 2 min = early unlock
#define PIN_BUZZER         13     // KY-006 passive buzzer
#define PIN_SERVO          16     // MG90S signal (servo power from the 5 V rail, 470 uF across it)
// ILI9488 3.5" SPI + XPT2046 touch: pins are set for TFT_eSPI in platformio.ini (build_flags).
//   SCK 18, MOSI 23, MISO 19 (touch T_DO only — leave the TFT's SDO/MISO unconnected),
//   TFT_CS 5, TFT_DC 2, TFT_RST 4, TOUCH_CS 15. LED (backlight) -> 3V3.

// ---------------- link / cloud ----------------
#define NODE_OFFLINE_MS    35000UL   // 3.5 missed windows -> node offline event
#define BED_FRESH_MS       25000UL
#define TELEMETRY_MS       10000UL   // gateway-only record when no bed record went out recently

#define MQTT_SERVER        "mqtt.netpie.io"
#define MQTT_PORT          1884      // NETPIE: 1883 = plain MQTT, 1884 = MQTTS (TLS)
#define MQTT_TOPIC         "@shadow/data/update"

#define Q_FILE             "/q.jsonl"   // store-and-forward queue (LittleFS)
#define Q_POS_FILE         "/q.pos"
#define Q_MAX_BYTES        (600UL * 1024UL)   // ~2.5 h of 10 s records (LittleFS partition is 896 KB with huge_app.csv)

// ---------------- tray (KY-024) ----------------
#define HALL_BASELINE_DEF  1950      // ADC counts with the tray UP (no magnet). Calibrate with serial 'c'
#define HALL_ON_DELTA      300       // |raw - baseline| above this  -> tray down
#define HALL_OFF_DELTA     200       // below this                   -> tray up (hysteresis)
#define HALL_DEBOUNCE      3         // consecutive 100 ms samples

// ---------------- servo (MG90S pulls the latch tongue) ----------------
#define SERVO_REST_US      1100      // latch free to engage (tune with serial 'v')
#define SERVO_PULL_US      1900      // latch pulled back -> tray pops up
#define SERVO_PULL_MS      700       // how long to hold the pull
#define SERVO_RETURN_MS    400       // back to rest, then PWM stops (no holding torque = safe, rule 4.3)
#define RELEASE_CONFIRM_MS 8000      // tray must read "up" within this, else retry
#define RELEASE_RETRIES    3

// ---------------- KY-004 / KY-040 ----------------
#define BUTTON_ACTIVE_LOW  1         // KY-004 pulls S to GND when pressed; set 0 if yours goes HIGH
#define ENC_STEPS_PER_DETENT 4       // set 2 if one click moves two steps
#define ENC_DIR            1         // -1 to reverse the knob direction

// ---------------- behaviour (agreed 14 Nov 2026) ----------------
#define NAG_MARGIN_MIN     30        // nag starts at wake - (sleep goal + 30 min) if the tray is empty
#define UP_REQUIRED_MIN    30        // cumulative "up" minutes before the box opens
#define BACK_TO_BED_S      120       // asleep / lying in bed this long after getting up -> ring again
#define ASLEEP_CONFIRM_WIN 3         // consecutive ASLEEP windows before LOCKED -> SLEEPING
#define EARLY_UNLOCK_HOLD_MS 120000UL// hold KY-004 for 2 min = "just want my phone" unlock
#define RING_ON_S          600       // ring 10 min ...
#define RING_OFF_S         300       // ... pause 5 min (someone else sleeps in the room), repeat
#define NODE_STALE_WARN_S  120       // no bed data this long during a locked night -> Discord warning
#define KNOB_STEP_MIN      5         // knob on the home screen moves the next wake by 5 min

// ---------------- data-collection mode (labelling for AI training) ----------------
#define PROBE_INTERVAL_S   240       // after "I'm awake", ask again every 4 min
#define PROBE_WINDOW_S     30        // 30 s to tap, otherwise "asleep"
