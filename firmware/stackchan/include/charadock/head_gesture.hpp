// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace charadock {

enum class HeadGesture : uint8_t {
  Waiting = 1, Nod = 2, Ponder = 3, Delight = 4, Reassure = 5, Accent, WaitingLook,
  LookRight, LookLeft, LookCenter, Sweep, VoiceNod,
  LookUp, LookDown, Bow, Tilt, LookUpRight, LookUpLeft, LookDownRight, LookDownLeft,
};

constexpr uint8_t kFirstVoiceMotion = static_cast<uint8_t>(HeadGesture::LookRight);
constexpr uint8_t kLastVoiceMotion = static_cast<uint8_t>(HeadGesture::LookDownLeft);

struct HeadPose {
  int yaw;
  int pitch;
  int speed;
};

// Angles are in tenths of a degree. Emit each target once and let the servo
// interpolate. Speech gestures wait for voiced PCM at the speaker queue head.
class HeadGesturePlayer {
public:
  bool request(HeadGesture gesture, uint32_t now) {
    const bool semantic = gesture >= HeadGesture::Nod && gesture <= HeadGesture::Reassure;
    const bool replaceable = gesture_ == HeadGesture::Waiting ||
        gesture_ == HeadGesture::WaitingLook || gesture_ == HeadGesture::Accent;
    const bool direct = gesture >= HeadGesture::LookRight;
    if (phase_ != Phase::Idle && !direct && (!semantic || !replaceable)) return false;
    swept_ = false;
    if (phase_ == Phase::Holding) needsHome_ = true;
    gesture_ = gesture;
    started_ = now;
    phase_ = Phase::Pending;
    return true;
  }

  bool next(uint32_t now, bool voiced, uint8_t intensity, HeadPose &pose) {
    // A pose can be scheduled after the render loop has sampled its clock.
    // A stale tick must not look like an elapsed timeout.
    const int32_t age = static_cast<int32_t>(now - started_);
    if (age < 0) return false;
    if (phase_ == Phase::Pending) {
      if (age >= 800 || !intensity) {
        phase_ = Phase::Idle;
        if (!needsHome_) return false;
        needsHome_ = false;
        pose = {0, 0, 335};
        return true;
      }
      if (!voiced && gesture_ < HeadGesture::LookRight &&
          gesture_ != HeadGesture::Waiting && gesture_ != HeadGesture::WaitingLook) return false;
      // The BSP uses a spring stiffness curve over 0–1000. Each small pose
      // needs enough speed to develop before the return starts.
      switch (gesture_) {
      case HeadGesture::Waiting: pose = {0, 44, 410}; hold_ = 320; break;
      case HeadGesture::WaitingLook: pose = {-55, 18, 370}; hold_ = 420; break;
      case HeadGesture::Nod: pose = {0, 68, 420}; hold_ = 650; break;
      case HeadGesture::Ponder: pose = {-40, 14, 360}; hold_ = 380; break;
      case HeadGesture::Delight: pose = {35, 16, 420}; hold_ = 360; break;
      case HeadGesture::Reassure: pose = {0, 23, 350}; hold_ = 330; break;
      case HeadGesture::Accent: pose = {0, 75, 390}; hold_ = 700; break;
      case HeadGesture::LookRight: pose = {-250, 0, 350}; hold_ = 1600; break;
      case HeadGesture::LookLeft:
      case HeadGesture::Sweep: pose = {250, 0, 350}; hold_ = 1500; break;
      case HeadGesture::LookCenter: pose = {0, 0, 335}; hold_ = 400; break;
      case HeadGesture::VoiceNod: pose = {0, 42, 390}; hold_ = 450; break;
      case HeadGesture::LookUp: pose = {0, 210, 350}; hold_ = 1400; break;
      case HeadGesture::LookDown: pose = {0, -210, 335}; hold_ = 1400; break;
      case HeadGesture::Bow: pose = {0, -180, 390}; hold_ = 650; break;
      case HeadGesture::Tilt: pose = {-165, 65, 350}; hold_ = 1200; break;
      case HeadGesture::LookUpRight: pose = {-220, 210, 350}; hold_ = 1400; break;
      case HeadGesture::LookUpLeft: pose = {220, 210, 350}; hold_ = 1400; break;
      case HeadGesture::LookDownRight: pose = {-220, -210, 335}; hold_ = 1400; break;
      case HeadGesture::LookDownLeft: pose = {220, -210, 335}; hold_ = 1400; break;
      }
      pose.yaw = pose.yaw * intensity / 100;
      pose.pitch = pose.pitch * intensity / 100;
      needsHome_ = false;
      phase_ = Phase::Holding;
      started_ = now;
      return true;
    }
    if (phase_ == Phase::Holding && static_cast<uint32_t>(age) >= hold_) {
      if (gesture_ == HeadGesture::Sweep && !swept_) {
        swept_ = true;
        started_ = now;
        pose = {-250 * intensity / 100, 0, 350};
        return true;
      }
      phase_ = Phase::Returning;
      started_ = now;
      pose = {0, 0, 335};
      return true;
    }
    if (phase_ == Phase::Returning && age >= 420)
      phase_ = Phase::Idle;
    return false;
  }

  bool busy() const { return phase_ != Phase::Idle; }
  bool voiceMotionActive() const { return busy() && gesture_ >= HeadGesture::LookRight; }

  bool cancel() {
    const bool moving = needsHome_ || phase_ == Phase::Holding || phase_ == Phase::Returning;
    needsHome_ = false;
    phase_ = Phase::Idle;
    return moving;
  }

private:
  enum class Phase { Idle, Pending, Holding, Returning };
  Phase phase_ = Phase::Idle;
  HeadGesture gesture_ = HeadGesture::Waiting;
  uint32_t started_ = 0;
  uint32_t hold_ = 0;
  bool needsHome_ = false;
  bool swept_ = false;
};

} // namespace charadock
