// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace charadock {

class IdleBacklight {
public:
  static constexpr uint8_t kActiveBrightness = 128;
  static constexpr uint8_t kDimBrightness = 32;
  static constexpr uint32_t kDimAfterMs = 30000;
  static constexpr uint32_t kOffAfterMs = 300000;

  void noteActivity(uint32_t now) {
    lastActiveAt_ = now;
    off_ = false;
  }

  uint8_t update(uint32_t now, bool active) {
    if (active) noteActivity(now);
    const uint32_t idleMs = now - lastActiveAt_;
    off_ = off_ || idleMs >= kOffAfterMs;
    if (off_) return 0;
    if (idleMs < kDimAfterMs) return kActiveBrightness;
    // Four small steps over roughly half a second. Keep I2C traffic off
    // the audio path as soon as the display has reached its idle level.
    constexpr uint32_t stepMs = 120;
    constexpr uint32_t steps = 4;
    const uint32_t step = 1 + (idleMs - kDimAfterMs) / stepMs;
    if (step >= steps) return kDimBrightness;
    return kActiveBrightness - step * (kActiveBrightness - kDimBrightness) / steps;
  }

private:
  uint32_t lastActiveAt_ = 0;
  bool off_ = false;
};

} // namespace charadock
