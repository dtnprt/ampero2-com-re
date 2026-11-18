#include "Ili9325Spi.h"

namespace {

constexpr uint8_t START_INDEX = 0x70;   // ID=0, RS=0, RW=0
constexpr uint8_t START_DATA = 0x72;    // ID=0, RS=1, RW=0
constexpr uint16_t BGR = 0x1000;        // entry mode bit 12 (fbtft: .bgr = true)

constexpr uint16_t DELAY = 0xFFFF;      // pseudo register: value = milliseconds

// fbtft hy28b_init_sequence (register, value) pairs
const uint16_t kInit[][2] = {
    {0x00E7, 0x0010}, {0x0000, 0x0001}, {0x0001, 0x0100}, {0x0002, 0x0700}, {0x0003, 0x1030},
    {0x0004, 0x0000}, {0x0008, 0x0207}, {0x0009, 0x0000}, {0x000A, 0x0000}, {0x000C, 0x0001},
    {0x000D, 0x0000}, {0x000F, 0x0000}, {0x0010, 0x0000}, {0x0011, 0x0007}, {0x0012, 0x0000},
    {0x0013, 0x0000}, {DELAY, 50},
    {0x0010, 0x1590}, {0x0011, 0x0227}, {DELAY, 50},
    {0x0012, 0x009C}, {DELAY, 50},
    {0x0013, 0x1900}, {0x0029, 0x0023}, {0x002B, 0x000E}, {DELAY, 50},
    {0x0020, 0x0000}, {0x0021, 0x0000}, {DELAY, 50},
    {0x0050, 0x0000}, {0x0051, 0x00EF}, {0x0052, 0x0000}, {0x0053, 0x013F}, {0x0060, 0xA700},
    {0x0061, 0x0001}, {0x006A, 0x0000}, {0x0080, 0x0000}, {0x0081, 0x0000}, {0x0082, 0x0000},
    {0x0083, 0x0000}, {0x0084, 0x0000}, {0x0085, 0x0000}, {0x0090, 0x0010}, {0x0092, 0x0000},
    {0x0093, 0x0003}, {0x0095, 0x0110}, {0x0097, 0x0000}, {0x0098, 0x0000}, {0x0007, 0x0133},
    {0x0020, 0x0000}, {0x0021, 0x0000}, {DELAY, 100},
    // gamma: fbtft HY28B_GAMMA "04 1F 4 7 7 0 7 7 6 0 / 0F 00 1 7 4 0 0 0 6 7" (set_gamma mapping)
    {0x0030, 0x0007}, {0x0031, 0x0707}, {0x0032, 0x0006}, {0x0035, 0x0704}, {0x0036, 0x1F04},
    {0x0037, 0x0004}, {0x0038, 0x0000}, {0x0039, 0x0706}, {0x003C, 0x0701}, {0x003D, 0x000F},
};

}  // namespace

bool Ili9325Spi::begin(const Pins& pins, int rotation, uint32_t spiHz) {
  p_ = pins;
  rot_ = (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270) ? rotation : 90;
  spi_ = SPISettings(spiHz, MSBFIRST, SPI_MODE3);

  pinMode(p_.cs, OUTPUT);
  digitalWrite(p_.cs, HIGH);
  pinMode(p_.reset, OUTPUT);
  pinMode(p_.backlight, OUTPUT);
  digitalWrite(p_.backlight, LOW);

  SPI.setSCK(p_.sck);
  SPI.setTX(p_.mosi);
  SPI.setRX(p_.miso);
  SPI.begin();

  digitalWrite(p_.reset, HIGH);
  delay(5);
  digitalWrite(p_.reset, LOW);
  delay(20);
  digitalWrite(p_.reset, HIGH);
  delay(120);

  for (const auto& c : kInit) {
    if (c[0] == DELAY) delay(c[1]);
    else writeReg(c[0], c[1]);
  }
  // fb_ili9325 set_var(): GRAM update direction per rotation
  uint16_t entry = 0x0030;
  if (rot_ == 90) entry = 0x0018;
  else if (rot_ == 180) entry = 0x0000;
  else if (rot_ == 270) entry = 0x0028;
  writeReg(0x0003, entry | BGR);
  return true;
}

void Ili9325Spi::setBacklight(uint8_t level) {
  if (level == 0) digitalWrite(p_.backlight, LOW);
  else if (level == 255) digitalWrite(p_.backlight, HIGH);
  else analogWrite(p_.backlight, level);
}

void Ili9325Spi::writeIndex(uint16_t reg) {
  SPI.beginTransaction(spi_);
  digitalWrite(p_.cs, LOW);
  SPI.transfer(START_INDEX);
  SPI.transfer16(reg);
  digitalWrite(p_.cs, HIGH);
  SPI.endTransaction();
}

void Ili9325Spi::writeReg(uint16_t reg, uint16_t val) {
  writeIndex(reg);
  SPI.beginTransaction(spi_);
  digitalWrite(p_.cs, LOW);
  SPI.transfer(START_DATA);
  SPI.transfer16(val);
  digitalWrite(p_.cs, HIGH);
  SPI.endTransaction();
}

void Ili9325Spi::beginFrameAt(int row) {
  // GRAM start address of logical (0, row) (fb_ili9325 set_addr_win); the scan direction comes from the
  // entry mode set in begin():  0: x+ then y+   90: y- then x+   180: x- then y-   270: y+ then x-
  uint16_t x = 0, y = (uint16_t)row;
  if (rot_ == 90) { x = (uint16_t)row; y = 319; }
  else if (rot_ == 180) { x = 239; y = (uint16_t)(319 - row); }
  else if (rot_ == 270) { x = (uint16_t)(239 - row); y = 0; }
  writeReg(0x0020, x);
  writeReg(0x0021, y);
  writeIndex(0x0022);   // following data goes to GRAM, address auto-increments across CS cycles
}

void Ili9325Spi::writePixels(uint16_t* px, size_t n) {
  for (size_t i = 0; i < n; i++) px[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));   // big endian on the wire
  SPI.beginTransaction(spi_);
  digitalWrite(p_.cs, LOW);
  SPI.transfer(START_DATA);
  SPI.transfer(px, nullptr, n * 2);
  digitalWrite(p_.cs, HIGH);
  SPI.endTransaction();
}
