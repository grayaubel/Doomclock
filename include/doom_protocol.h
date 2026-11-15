// doom_protocol.h — ESP-NOW wire format shared by the bed node and the gateway.
// Both sides include THIS file, so the packet layout can never drift apart again.
// If you change a struct, bump DOOM_PROTO_VER: the gateway drops packets whose
// version does not match, instead of silently mis-reading them.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define DOOM_MAGIC        0xD7
#define DOOM_PROTO_VER    1
#define DOOM_WINDOW_MS    10000UL   // one bed packet summarises 10 s of sensing
#define DOOM_IMU_HZ       25        // MPU6050 sample rate on the node
#define DOOM_TOF_HZ       5         // VL53L0X sample rate on the node

enum DoomPktType : uint8_t {
  DOOM_PKT_BED = 1,   // node -> gateway: one 10 s feature window
  DOOM_PKT_ACK = 2,   // gateway -> node: application-level acknowledgement
};

// flags in BedPacket.flags
#define BED_F_IMU_OK   0x01
#define BED_F_TOF_OK   0x02
#define BED_F_MIC_ON   0x04
#define BED_F_SD_OK    0x08

// One 10-second window of bed sensing. All values are integers (no floats on the wire).
typedef struct __attribute__((packed)) {
  uint8_t  magic;            // DOOM_MAGIC
  uint8_t  ver;              // DOOM_PROTO_VER
  uint8_t  type;             // DOOM_PKT_BED
  uint8_t  node_id;
  uint16_t boot_id;          // random per node boot: lets the gateway tell a reboot from a duplicate
  uint32_t seq;              // window counter since node boot (gaps = lost packets)
  uint16_t age_s;            // seconds between window end and THIS transmission (backlog delay)
  uint8_t  attempts;         // transmission attempt number for this packet (1 = first try)
  uint8_t  flags;            // BED_F_*

  // --- IMU (MPU6050 on the mattress) ---
  uint16_t imu_n;            // samples in window (~250)
  uint16_t move_count;       // samples classified as "moving"
  uint16_t acc_dev_mean_mg;  // mean | |a| - 1 g |, milli-g
  uint16_t acc_dev_max_mg;
  uint16_t jerk_mean_mg;     // mean |a_t - a_(t-1)|, milli-g  (best single movement signal)
  uint16_t jerk_max_mg;
  uint16_t gyro_mean_cdps;   // mean |gyro| after bias removal, centi-deg/s
  uint16_t gyro_max_cdps;
  int8_t   tilt_x_deg;       // mean tilt of the sensor (mattress sag / sensor knocked)
  int8_t   tilt_y_deg;

  // --- ToF (VL53L0X looking at the bed) ---
  uint8_t  tof_n;            // readings attempted
  uint8_t  tof_valid;        // readings in range
  uint16_t tof_mean_mm;
  uint16_t tof_min_mm;
  uint16_t tof_max_mm;
  uint16_t tof_std_mm;

  // --- Mic (KY-038, optional) ---
  uint16_t mic_p2p_mean;     // mean peak-to-peak ADC counts per 40 ms burst (0 if disabled)
  uint16_t mic_p2p_max;

  // --- link health ---
  uint16_t backlog;          // packets still waiting on the node (including this one)
  uint16_t dropped_total;    // packets the node had to discard (backlog overflow)

  uint16_t crc;              // CRC-16/CCITT over every byte before this field
} BedPacket;

typedef struct __attribute__((packed)) {
  uint8_t  magic;
  uint8_t  ver;
  uint8_t  type;             // DOOM_PKT_ACK
  uint8_t  node_id;
  uint16_t boot_id;
  uint32_t seq;              // seq being acknowledged
  uint32_t unix_time;        // gateway clock (UTC), 0 if unknown -> node timestamps its SD log
  uint16_t crc;
} AckPacket;

static_assert(sizeof(BedPacket) <= 250, "ESP-NOW payload limit is 250 bytes");

static inline uint16_t doom_crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

template <typename T>
static inline void doom_seal(T& p) { p.crc = doom_crc16((const uint8_t*)&p, offsetof(T, crc)); }

template <typename T>
static inline bool doom_check(const T& p) {
  return p.magic == DOOM_MAGIC && p.ver == DOOM_PROTO_VER &&
         p.crc == doom_crc16((const uint8_t*)&p, offsetof(T, crc));
}
