#include "Xpt2046.h"

#include <EEPROM.h>

namespace {

// control byte: S=1, channel, 12 bit, differential, PD=00 (pen IRQ stays enabled)
constexpr uint8_t CMD_X = 0xD0;
constexpr uint8_t CMD_Y = 0x90;
constexpr uint8_t CMD_Z1 = 0xB0;
constexpr uint8_t CMD_Z2 = 0xC0;
constexpr int kMinPressure = 300;
constexpr int kSamples = 7;

const SPISettings kSpi(2000000, MSBFIRST, SPI_MODE0);

int median(int* v, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
      int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
    }
  return v[n / 2];
}

}  // namespace

void Xpt2046::begin(SPIClassRP2040& spi, const Pins& pins) {
  spi_ = &spi;
  p_ = pins;
  pinMode(p_.cs, OUTPUT);
  digitalWrite(p_.cs, HIGH);
  pinMode(p_.irq, INPUT_PULLUP);
  spi.setSCK(p_.sck);
  spi.setTX(p_.mosi);
  spi.setRX(p_.miso);
  spi.begin();
  // one dummy conversion with PD=00 so the pen IRQ is armed
  spi.beginTransaction(kSpi);
  digitalWrite(p_.cs, LOW);
  measure(CMD_Y);
  digitalWrite(p_.cs, HIGH);
  spi.endTransaction();
}

uint16_t Xpt2046::measure(uint8_t cmd) {
  spi_->transfer(cmd);
  return spi_->transfer16(0) >> 3;   // 12-bit result, MSB first after one busy clock
}

bool Xpt2046::readRaw(int& rx, int& ry) {
  int xs[kSamples], ys[kSamples];
  spi_->beginTransaction(kSpi);
  digitalWrite(p_.cs, LOW);
  const int z1 = measure(CMD_Z1);
  const int z2 = measure(CMD_Z2);
  for (int i = 0; i < kSamples; i++) {
    xs[i] = measure(CMD_X);
    ys[i] = measure(CMD_Y);
  }
  digitalWrite(p_.cs, HIGH);
  spi_->endTransaction();

  const int pressure = z1 + 4095 - z2;
  if (z1 == 0 || pressure < kMinPressure) return false;
  rx = median(xs, kSamples);
  ry = median(ys, kSamples);
  return rx > 0 && rx < 4095 && ry > 0 && ry < 4095;
}

void Xpt2046::map(int rx, int ry, int& sx, int& sy) const {
  sx = (int)(cal_.a * rx + cal_.b * ry + cal_.c + 0.5f);
  sy = (int)(cal_.d * rx + cal_.e * ry + cal_.f + 0.5f);
}

bool Xpt2046::solve(const int sx[3], const int sy[3], const int rx[3], const int ry[3], TouchCalibration& out) {
  // [rx ry 1] * [a b c]^T = sx  for three points (Cramer's rule)
  const double det = (double)rx[0] * (ry[1] - ry[2]) - (double)ry[0] * (rx[1] - rx[2]) +
                     ((double)rx[1] * ry[2] - (double)rx[2] * ry[1]);
  if (det > -1000.0 && det < 1000.0) return false;   // points too close / collinear

  auto coeff = [&](const int* s, float& a, float& b, float& c) {
    const double da = (double)s[0] * (ry[1] - ry[2]) - (double)ry[0] * (s[1] - s[2]) +
                      ((double)s[1] * ry[2] - (double)s[2] * ry[1]);
    const double db = (double)rx[0] * (s[1] - s[2]) - (double)s[0] * (rx[1] - rx[2]) +
                      ((double)rx[1] * s[2] - (double)rx[2] * s[1]);
    const double dc = (double)rx[0] * ((double)ry[1] * s[2] - (double)ry[2] * s[1]) -
                      (double)ry[0] * ((double)rx[1] * s[2] - (double)rx[2] * s[1]) +
                      (double)s[0] * ((double)rx[1] * ry[2] - (double)rx[2] * ry[1]);
    a = (float)(da / det);
    b = (float)(db / det);
    c = (float)(dc / det);
  };
  coeff(sx, out.a, out.b, out.c);
  coeff(sy, out.d, out.e, out.f);
  out.magic = TouchCalibration::kMagic;
  return true;
}

bool Xpt2046::loadCalibration(TouchCalibration& c) {
  EEPROM.begin(kEepromSize);
  EEPROM.get(0, c);
  return c.valid();
}

void Xpt2046::saveCalibration(const TouchCalibration& c) {
  EEPROM.begin(kEepromSize);
  EEPROM.put(0, c);
  EEPROM.commit();
}
