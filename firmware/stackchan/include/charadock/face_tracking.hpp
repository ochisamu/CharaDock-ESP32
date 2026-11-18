// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace charadock {

class FaceTracking {
public:
  void update(uint32_t now, uint32_t seenAt, int x, int y, bool enabled) {
    if (!enabled || (!seenAt && lastSeenAt_ && now - lastSeenAt_ >= 1800)) {
      yaw_ = pitch_ = 0;
      lastSeenAt_ = 0;
      return;
    }
    if (!seenAt || seenAt == lastSeenAt_) return;
    const bool reacquired = !lastSeenAt_ || seenAt - lastSeenAt_ >= 1800;
    lastSeenAt_ = seenAt;
    filteredX_ = reacquired ? x : (filteredX_ + x) / 2;
    filteredY_ = reacquired ? y : (filteredY_ + y) / 2;
    // The StackChan camera is mirrored. In the BSP positive pitch is upward.
    yaw_ = std::clamp(yaw_ - step(filteredX_, 26), -120, 120);
    pitch_ = std::clamp(pitch_ - step(filteredY_, 23), -50, 50);
  }

  int yaw() const { return yaw_; }
  int pitch() const { return pitch_; }

private:
  static int step(int error, int deadZone) {
    return std::abs(error) <= deadZone ? 0 : std::clamp(error / 6, -14, 14);
  }

  uint32_t lastSeenAt_ = 0;
  int yaw_ = 0;
  int pitch_ = 0;
  int filteredX_ = 0;
  int filteredY_ = 0;
};

} // namespace charadock
