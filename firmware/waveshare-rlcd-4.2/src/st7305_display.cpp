// SPDX-License-Identifier: Apache-2.0
// Adapted from Waveshare ESP32-S3-RLCD-4.2 U8g2 example, commit
// eb1f63427d735a22b9c30e22fa63ebddae1834d3 (Apache-2.0).
#include "charadock/st7305_display.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace charadock::rlcd {
namespace {

constexpr uint32_t kSpiClockHz = 24000000;
constexpr uint8_t kTileWidth = 38;
constexpr uint8_t kTileHeight = 50;
constexpr size_t kBufferRowBytes = kTileWidth * 8;
constexpr size_t kBufferBytes = kBufferRowBytes * kTileHeight;

u8x8_display_info_t displayInfo = {
    /* chip_enable_level = */ 0,
    /* chip_disable_level = */ 1,
    /* post_chip_enable_wait_ns = */ 0,
    /* pre_chip_disable_wait_ns = */ 0,
    /* reset_pulse_width_ms = */ 20,
    /* post_reset_wait_ms = */ 50,
    /* sda_setup_time_ns = */ 0,
    /* sck_pulse_width_ns = */ 0,
    /* sck_clock_hz = */ kSpiClockHz,
    /* spi_mode = */ 0,
    /* i2c_bus_clock_100kHz = */ 4,
    /* data_setup_time_ns = */ 0,
    /* write_pulse_width_ns = */ 0,
    /* tile_width = */ kTileWidth,
    /* tile_height = */ kTileHeight,
    /* default_x_offset = */ 0,
    /* flip_mode_x_offset = */ 0,
    /* pixel_width = */ 300,
    /* pixel_height = */ 400,
};

} // namespace

St7305Display *St7305Display::instance_ = nullptr;

St7305Display::St7305Display(int sck, int mosi, int dc, int cs, int reset)
    : sck_(sck), mosi_(mosi), dc_(dc), cs_(cs), reset_(reset),
      spi_(new SPIClass(HSPI)) {
  instance_ = this;
}

St7305Display::~St7305Display() {
  if (spi_) {
    spi_->end();
    delete spi_;
  }
  std::free(buffer_);
  std::free(displayedBuffer_);
  if (instance_ == this)
    instance_ = nullptr;
}

bool St7305Display::begin(const u8g2_cb_t *rotation) {
  if (!spi_ || instance_ != this)
    return false;
  pinMode(dc_, OUTPUT);
  pinMode(cs_, OUTPUT);
  pinMode(reset_, OUTPUT);
  digitalWrite(cs_, HIGH);
  digitalWrite(dc_, HIGH);
  digitalWrite(reset_, HIGH);

  spi_->begin(sck_, -1, mosi_, -1);
  spi_->beginTransaction(SPISettings(kSpiClockHz, MSBFIRST, SPI_MODE0));

  u8g2_t *raw = u8g2_.getU8g2();
  u8x8_Setup(u8g2_GetU8x8(raw), displayCallback, u8x8_dummy_cb,
              byteCallback, u8x8_dummy_cb);
  buffer_ = static_cast<uint8_t *>(std::malloc(kBufferBytes));
  displayedBuffer_ = static_cast<uint8_t *>(std::malloc(kBufferBytes));
  if (!buffer_ || !displayedBuffer_) {
    std::free(buffer_);
    std::free(displayedBuffer_);
    buffer_ = nullptr;
    displayedBuffer_ = nullptr;
    return false;
  }
  std::memset(buffer_, 0, kBufferBytes);
  std::memset(displayedBuffer_, 0, kBufferBytes);
  u8g2_SetupBuffer(raw, buffer_, kTileHeight,
                   u8g2_ll_hvline_vertical_top_lsb, rotation);
  u8g2_InitDisplay(raw);
  u8g2_SetPowerSave(raw, 0);
  healthy_ = true;
  return true;
}

bool St7305Display::healthy() const { return healthy_; }
U8G2 &St7305Display::canvas() { return u8g2_; }

uint32_t St7305Display::flush() {
  if (!healthy_)
    return 0;
  const uint32_t started = micros();
  if (!displayedBufferValid_) {
    u8g2_.sendBuffer();
    std::memcpy(displayedBuffer_, buffer_, kBufferBytes);
    displayedBufferValid_ = true;
    return micros() - started;
  }

  uint8_t firstTileX = kTileWidth;
  uint8_t lastTileX = 0;
  uint8_t firstTileY = kTileHeight;
  uint8_t lastTileY = 0;
  bool changed = false;
  for (uint8_t tileY = 0; tileY < kTileHeight; ++tileY) {
    const size_t row = static_cast<size_t>(tileY) * kBufferRowBytes;
    for (uint8_t tileX = 0; tileX < kTileWidth; ++tileX) {
      const size_t offset = row + static_cast<size_t>(tileX) * 8u;
      if (std::memcmp(buffer_ + offset, displayedBuffer_ + offset, 8u) == 0)
        continue;
      changed = true;
      firstTileX = std::min(firstTileX, tileX);
      lastTileX = std::max(lastTileX, tileX);
      firstTileY = std::min(firstTileY, tileY);
      lastTileY = std::max(lastTileY, tileY);
    }
  }
  if (changed) {
    // U8g2 updateDisplayArea uses unrotated buffer tile coordinates. The
    // comparison above walks that exact memory layout, so the resulting box
    // remains correct with the landscape U8G2_R1 view used by CharaDock.
    u8g2_.updateDisplayArea(firstTileX, firstTileY,
                            lastTileX - firstTileX + 1,
                            lastTileY - firstTileY + 1);
    std::memcpy(displayedBuffer_, buffer_, kBufferBytes);
  }
  return micros() - started;
}

void St7305Display::command(uint8_t value) {
  digitalWrite(dc_, LOW);
  digitalWrite(cs_, LOW);
  spi_->transfer(value);
  digitalWrite(cs_, HIGH);
}

void St7305Display::commandData(uint8_t commandValue, const uint8_t *data,
                                size_t length) {
  digitalWrite(dc_, LOW);
  digitalWrite(cs_, LOW);
  spi_->transfer(commandValue);
  if (data && length) {
    digitalWrite(dc_, HIGH);
    spi_->transferBytes(const_cast<uint8_t *>(data), nullptr, length);
  }
  digitalWrite(cs_, HIGH);
}

void St7305Display::resetPanel() {
  digitalWrite(reset_, HIGH);
  delay(50);
  digitalWrite(reset_, LOW);
  delay(20);
  digitalWrite(reset_, HIGH);
  delay(50);
}

uint8_t St7305Display::byteCallback(u8x8_t *, uint8_t, uint8_t, void *) {
  return 1;
}

uint8_t St7305Display::displayCallback(u8x8_t *u8x8, uint8_t message,
                                       uint8_t, void *pointer) {
  if (!instance_)
    return 0;
  switch (message) {
  case U8X8_MSG_DISPLAY_SETUP_MEMORY:
    u8x8_d_helper_display_setup_memory(u8x8, &displayInfo);
    return 1;
  case U8X8_MSG_DISPLAY_INIT:
    instance_->initializePanel();
    return 1;
  case U8X8_MSG_DISPLAY_DRAW_TILE: {
    auto *tile = static_cast<u8x8_tile_t *>(pointer);
    const int firstColumn = tile->x_pos * 8;
    const int lastColumn =
        std::min(299, static_cast<int>((tile->x_pos + tile->cnt) * 8 - 1));
    const int addressStart = 0x12 + firstColumn / 12;
    const int addressEnd = 0x12 + lastColumn / 12;
    const int sendStart = (addressStart - 0x12) * 3;
    const int sendCount = (addressEnd - addressStart + 1) * 3;
    const int addressFirstColumn = (addressStart - 0x12) * 12;
    const int addressLastColumn =
        std::min(299, (addressEnd - 0x12) * 12 + 11);
    uint8_t *rowBase = tile->tile_ptr -
                       static_cast<uint16_t>(tile->x_pos) * 8u;

    const uint8_t columnBounds[] = {
        static_cast<uint8_t>(0x3c - addressEnd),
        static_cast<uint8_t>(0x3c - addressStart)};
    instance_->commandData(0x2a, columnBounds, sizeof(columnBounds));
    const uint8_t rowBounds[] = {
        static_cast<uint8_t>(tile->y_pos * 4),
        static_cast<uint8_t>(tile->y_pos * 4 + 3)};
    instance_->commandData(0x2b, rowBounds, sizeof(rowBounds));

    static constexpr uint8_t lookup[4][4] = {
        {0x00, 0x80, 0x40, 0xc0},
        {0x00, 0x20, 0x10, 0x30},
        {0x00, 0x08, 0x04, 0x0c},
        {0x00, 0x02, 0x01, 0x03},
    };
    uint8_t converted[300] = {};
    for (int sourceRow = 0; sourceRow < 4; ++sourceRow) {
      const int shift = sourceRow * 2;
      int output = sourceRow * sendCount +
                   (addressFirstColumn >> 2) - sendStart;
      for (int column = addressFirstColumn; column <= addressLastColumn;
           column += 4, ++output) {
        converted[output] =
            lookup[0][(rowBase[column] >> shift) & 3] |
            lookup[1][(rowBase[column + 1] >> shift) & 3] |
            lookup[2][(rowBase[column + 2] >> shift) & 3] |
            lookup[3][(rowBase[column + 3] >> shift) & 3];
      }
    }
    instance_->commandData(0x2c, converted,
                           static_cast<size_t>(sendCount) * 4u);
    return 1;
  }
  default:
    return 0;
  }
}

void St7305Display::initializePanel() {
  resetPanel();
  const uint8_t d6[] = {0x17, 0x02};
  const uint8_t d1[] = {0x01};
  const uint8_t c0[] = {0x11, 0x04};
  const uint8_t c1[] = {0x69, 0x69, 0x69, 0x69};
  const uint8_t c2[] = {0x19, 0x19, 0x19, 0x19};
  const uint8_t c4[] = {0x4b, 0x4b, 0x4b, 0x4b};
  const uint8_t d8[] = {0x80, 0xe9};
  const uint8_t b2[] = {0x02};
  const uint8_t b3[] = {0xe5, 0xf6, 0x05, 0x46, 0x77,
                        0x77, 0x77, 0x77, 0x76, 0x45};
  const uint8_t b4[] = {0x05, 0x46, 0x77, 0x77,
                        0x77, 0x77, 0x76, 0x45};
  const uint8_t timing[] = {0x32, 0x03, 0x1f};
  const uint8_t b7[] = {0x13};
  const uint8_t b0[] = {0x64};
  const uint8_t c9[] = {0x00};
  const uint8_t m36[] = {0x48};
  const uint8_t m3a[] = {0x11};
  const uint8_t b9[] = {0x20};
  const uint8_t b8[] = {0x29};
  const uint8_t windowA[] = {0x12, 0x2a};
  const uint8_t windowB[] = {0x00, 0xc7};
  const uint8_t m35[] = {0x00};
  const uint8_t d0[] = {0xff};

  commandData(0xd6, d6, sizeof(d6));
  commandData(0xd1, d1, sizeof(d1));
  commandData(0xc0, c0, sizeof(c0));
  commandData(0xc1, c1, sizeof(c1));
  commandData(0xc2, c2, sizeof(c2));
  commandData(0xc4, c4, sizeof(c4));
  commandData(0xc5, c2, sizeof(c2));
  commandData(0xd8, d8, sizeof(d8));
  commandData(0xb2, b2, sizeof(b2));
  commandData(0xb3, b3, sizeof(b3));
  commandData(0xb4, b4, sizeof(b4));
  commandData(0x62, timing, sizeof(timing));
  commandData(0xb7, b7, sizeof(b7));
  commandData(0xb0, b0, sizeof(b0));
  command(0x11);
  delay(120);
  commandData(0xc9, c9, sizeof(c9));
  commandData(0x36, m36, sizeof(m36));
  commandData(0x3a, m3a, sizeof(m3a));
  commandData(0xb9, b9, sizeof(b9));
  commandData(0xb8, b8, sizeof(b8));
  command(0x21);
  commandData(0x2a, windowA, sizeof(windowA));
  commandData(0x2b, windowB, sizeof(windowB));
  commandData(0x35, m35, sizeof(m35));
  commandData(0xd0, d0, sizeof(d0));
  command(0x38);
  command(0x29);
}

} // namespace charadock::rlcd
