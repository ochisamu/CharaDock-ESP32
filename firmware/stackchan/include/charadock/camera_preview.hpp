// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace charadock {
class CameraPreview {
public:
  bool start(bool preview = true);
  void stop();
  void pauseForConversation();
  void setDetectionPaused(bool paused);
  bool active() const;
  bool detecting() const;
  bool recentFace(uint32_t now) const;
  bool facePosition(uint32_t now, uint32_t &seenAt, int &x, int &y) const;
  int8_t gazeOffset(uint32_t now) const;
  uint32_t inferenceCount() const;
  uint32_t frameCount() const;
  void draw();
};
} // namespace charadock
