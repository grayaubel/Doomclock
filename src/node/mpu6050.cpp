#include "mpu6050.h"

bool Mpu6050::writeReg(uint8_t reg, uint8_t val) {
  wire_->beginTransmission(addr_);
  wire_->write(reg);
  wire_->write(val);
  return wire_->endTransmission() == 0;
}

bool Mpu6050::readRegs(uint8_t reg, uint8_t* buf, size_t n) {
  wire_->beginTransmission(addr_);
  wire_->write(reg);
  if (wire_->endTransmission(false) != 0) return false;          // repeated start
  if (wire_->requestFrom((int)addr_, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = wire_->read();
  return true;
}

bool Mpu6050::begin(TwoWire& wire, uint8_t addr) {
  wire_ = &wire;
  addr_ = addr;
  if (!writeReg(REG_PWR_MGMT_1, 0x80)) return false;             // DEVICE_RESET
  delay(100);
  if (!writeReg(REG_PWR_MGMT_1, 0x01)) return false;             // wake, clock = PLL with X gyro (more stable than 8 MHz RC)
  delay(10);
  if (!readRegs(REG_WHO_AM_I, &who_, 1)) return false;
  // DLPF_CFG = 4 -> ~20 Hz bandwidth, gyro output rate 1 kHz. We poll at 25 Hz,
  // so the filter removes most aliasing above Nyquist (12.5 Hz).
  writeReg(REG_CONFIG, 0x04);
  writeReg(REG_SMPLRT_DIV, 39);                                  // 1 kHz / (1+39) = 25 Hz internal sample rate
  writeReg(REG_GYRO_CONFIG, 0x00);                               // +-250 deg/s
  writeReg(REG_ACCEL_CONFIG, 0x00);                              // +-2 g
  return true;
}

bool Mpu6050::read(Raw& o) {
  uint8_t b[14];
  if (!readRegs(REG_ACCEL_XOUT_H, b, sizeof(b))) return false;
  auto be16 = [&](int i) { return (int16_t)((b[i] << 8) | b[i + 1]); };  // registers are big-endian
  o.ax = be16(0);  o.ay = be16(2);  o.az = be16(4);
  o.temp = be16(6);
  o.gx = be16(8);  o.gy = be16(10); o.gz = be16(12);
  return true;
}
