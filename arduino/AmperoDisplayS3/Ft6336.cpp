#include "Ft6336.h"

namespace {

constexpr uint8_t kAddr = 0x38;
constexpr uint8_t REG_TD_STATUS = 0x02;   // number of touch points (low nibble), then P1 XH XL YH YL
constexpr uint8_t REG_TH_GROUP = 0x80;    // touch threshold
constexpr uint8_t REG_G_MODE = 0xA4;      // 0 = INT polling mode (level while touched)
constexpr uint8_t REG_CHIP_ID = 0xA3;     // 0x64 = FT6336U
constexpr uint8_t REG_VENDOR_ID = 0xA8;   // 0x11 = FocalTech

}  // namespace

bool Ft6336::begin(TwoWire& wire, const Pins& pins, uint32_t i2cHz) {
  wire_ = &wire;
  p_ = pins;
  if (p_.reset >= 0) {
    pinMode(p_.reset, OUTPUT);
    digitalWrite(p_.reset, LOW);
    delay(10);
    digitalWrite(p_.reset, HIGH);
    delay(300);   // the controller needs ~300 ms after reset
  }
  if (p_.irq >= 0) pinMode(p_.irq, INPUT_PULLUP);
  wire_->begin(p_.sda, p_.scl, i2cHz);
  uint8_t v = 0;
  if (!readRegs(REG_CHIP_ID, &v, 1)) return false;
  chipId_ = v;
  if (readRegs(REG_VENDOR_ID, &v, 1)) vendorId_ = v;
  writeReg(REG_G_MODE, 0);
  writeReg(REG_TH_GROUP, 22);   // a bit more sensitive than the default (~40 on some panels)
  return true;
}

bool Ft6336::readRegs(uint8_t reg, uint8_t* buf, size_t n) {
  wire_->beginTransmission(kAddr);
  wire_->write(reg);
  if (wire_->endTransmission(false) != 0) return false;
  if (wire_->requestFrom(kAddr, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)wire_->read();
  return true;
}

void Ft6336::writeReg(uint8_t reg, uint8_t val) {
  wire_->beginTransmission(kAddr);
  wire_->write(reg);
  wire_->write(val);
  wire_->endTransmission();
}

bool Ft6336::read(int& rawX, int& rawY) {
  if (!wire_) return false;
  uint8_t d[5];
  if (!readRegs(REG_TD_STATUS, d, sizeof d)) return false;
  const uint8_t points = d[0] & 0x0F;
  if (points == 0 || points > 2) return false;
  const uint8_t event = d[1] >> 6;   // 0 press down, 1 lift up, 2 contact, 3 none
  if (event == 1 || event == 3) return false;
  rawX = ((d[1] & 0x0F) << 8) | d[2];
  rawY = ((d[3] & 0x0F) << 8) | d[4];
  return true;
}

void Ft6336::setMapping(int rotation, int nativeW, int nativeH, bool flipX, bool flipY) {
  rot_ = rotation & 3;
  nw_ = nativeW;
  nh_ = nativeH;
  flipX_ = flipX;
  flipY_ = flipY;
}

void Ft6336::map(int rx, int ry, int& x, int& y) const {
  int w, h;   // screen size in this rotation
  switch (rot_) {
    case 0: x = rx; y = ry; w = nw_; h = nh_; break;
    case 1: x = ry; y = nw_ - 1 - rx; w = nh_; h = nw_; break;
    case 2: x = nw_ - 1 - rx; y = nh_ - 1 - ry; w = nw_; h = nh_; break;
    default: x = nh_ - 1 - ry; y = rx; w = nh_; h = nw_; break;
  }
  if (flipX_) x = w - 1 - x;
  if (flipY_) y = h - 1 - y;
  x = x < 0 ? 0 : x >= w ? w - 1 : x;
  y = y < 0 ? 0 : y >= h ? h - 1 : y;
}
