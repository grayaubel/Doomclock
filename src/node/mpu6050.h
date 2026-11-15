// mpu6050.h — minimal register-level MPU6050 driver (no third-party library).
// Written directly against the MPU-6000/6050 Register Map rev 4.2 so the group
// can claim and explain it for depth item E8 (own driver at bus level).
#pragma once
#include <Arduino.h>
#include <Wire.h>

class Mpu6050 {
 public:
  // Register addresses (Register Map rev 4.2)
  static constexpr uint8_t REG_SMPLRT_DIV   = 0x19;
  static constexpr uint8_t REG_CONFIG       = 0x1A;
  static constexpr uint8_t REG_GYRO_CONFIG  = 0x1B;
  static constexpr uint8_t REG_ACCEL_CONFIG = 0x1C;
  static constexpr uint8_t REG_ACCEL_XOUT_H = 0x3B;   // 14 bytes: accel xyz, temp, gyro xyz
  static constexpr uint8_t REG_PWR_MGMT_1   = 0x6B;
  static constexpr uint8_t REG_WHO_AM_I     = 0x75;

  static constexpr float ACC_LSB_PER_G    = 16384.0f;  // AFS_SEL = 0  (+-2 g)
  static constexpr float GYRO_LSB_PER_DPS = 131.0f;    // FS_SEL  = 0  (+-250 deg/s)

  struct Raw { int16_t ax, ay, az, temp, gx, gy, gz; };

  // Returns false if nothing answers at addr. whoAmI() tells genuine (0x68) from clones.
  bool begin(TwoWire& wire = Wire, uint8_t addr = 0x68);
  bool read(Raw& out);
  uint8_t whoAmI() const { return who_; }

 private:
  bool writeReg(uint8_t reg, uint8_t val);
  bool readRegs(uint8_t reg, uint8_t* buf, size_t n);
  TwoWire* wire_ = nullptr;
  uint8_t addr_ = 0x68;
  uint8_t who_ = 0;
};
