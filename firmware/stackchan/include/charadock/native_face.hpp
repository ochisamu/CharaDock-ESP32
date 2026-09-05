// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <M5Unified.hpp>

#include "charadock/presentation.hpp"

namespace charadock {

class NativeFaceRenderer {
public:
  explicit NativeFaceRenderer(LGFX_Device &display);
  ~NativeFaceRenderer();

  bool begin();
  void render(const PresentationSnapshot &snapshot, uint32_t nowMs);
  void renderPortrait(const uint16_t *rgb565Pixels,
                      const PresentationSnapshot &snapshot, uint32_t nowMs);
  bool ready() const;

private:
  void drawEyes(const PresentationSnapshot &snapshot, uint16_t foreground,
                uint16_t accent);
  void drawMouth(const PresentationSnapshot &snapshot, uint16_t foreground);
  void drawStatus(const PresentationSnapshot &snapshot, uint32_t nowMs,
                  uint16_t foreground, uint16_t accent);
  void drawThickLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     uint8_t thickness, uint16_t color);
  static uint16_t rgb565(uint32_t rgb);
  static uint16_t blend565(uint16_t from, uint16_t to, uint8_t amount);

  LGFX_Device &display_;
  M5Canvas canvas_;
  bool ready_ = false;
  uint32_t lastFrameAt_ = 0;
};

} // namespace charadock
