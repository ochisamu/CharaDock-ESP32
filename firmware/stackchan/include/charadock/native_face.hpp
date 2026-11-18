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
  void render(const PresentationSnapshot &snapshot, uint32_t nowMs, uint8_t intervalMs = 33);
  void renderPortrait(const uint16_t *rgb565Pixels,
                      const PresentationSnapshot &snapshot, uint32_t nowMs);
  void renderQuickSettings(uint32_t nowMs, uint8_t volume, bool privacyMuted, const char *connection,
                           const char *trackingStatus,
                           bool trackingAvailable, const char *cameraStatus, bool cameraAvailable);
  bool ready() const;
  bool setCaption(const char *name, const char *caption);
  bool writeSnapshot(Stream &output);

private:
  void drawEyes(const PresentationSnapshot &snapshot, uint16_t foreground,
                uint16_t background);
  void drawMouth(const PresentationSnapshot &snapshot, uint16_t foreground,
                 uint16_t background);
  void drawStatus(const PresentationSnapshot &snapshot, uint32_t nowMs,
                  uint16_t foreground, uint16_t accent);
  static uint16_t rgb565(uint32_t rgb);
  static uint16_t blend565(uint16_t from, uint16_t to, uint8_t amount);

  LGFX_Device &display_;
  M5Canvas canvas_;
  bool ready_ = false;
  uint32_t lastFrameAt_ = 0;
  String name_, caption_;
};

} // namespace charadock
