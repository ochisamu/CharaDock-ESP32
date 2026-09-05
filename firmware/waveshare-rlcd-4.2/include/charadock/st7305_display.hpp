// SPDX-License-Identifier: Apache-2.0
// Adapted from Waveshare ESP32-S3-RLCD-4.2 U8g2 example, commit
// eb1f63427d735a22b9c30e22fa63ebddae1834d3 (Apache-2.0).
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

namespace charadock::rlcd {

class St7305Display {
public:
  St7305Display(int sck, int mosi, int dc, int cs, int reset);
  ~St7305Display();

  St7305Display(const St7305Display &) = delete;
  St7305Display &operator=(const St7305Display &) = delete;

  bool begin(const u8g2_cb_t *rotation = U8G2_R1);
  bool healthy() const;
  U8G2 &canvas();
  uint32_t flush();

private:
  void resetPanel();
  void initializePanel();
  void command(uint8_t command);
  void commandData(uint8_t command, const uint8_t *data, size_t length);

  static uint8_t displayCallback(u8x8_t *u8x8, uint8_t message,
                                 uint8_t argument, void *pointer);
  static uint8_t byteCallback(u8x8_t *u8x8, uint8_t message,
                              uint8_t argument, void *pointer);

  int sck_;
  int mosi_;
  int dc_;
  int cs_;
  int reset_;
  SPIClass *spi_ = nullptr;
  U8G2 u8g2_;
  uint8_t *buffer_ = nullptr;
  uint8_t *displayedBuffer_ = nullptr;
  bool displayedBufferValid_ = false;
  bool healthy_ = false;

  static St7305Display *instance_;
};

} // namespace charadock::rlcd
