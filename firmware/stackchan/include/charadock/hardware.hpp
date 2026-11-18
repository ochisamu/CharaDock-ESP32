// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "charadock/head_gesture.hpp"

namespace charadock {

class HardwareController {
public:
  static constexpr size_t kSpeakerBlockSamples = 1920;

  void begin();
  void update();
  void setBacklight(uint8_t brightness);
  uint8_t backlight() const;
  static constexpr uint32_t voiceLedColor(bool capturing, bool playing) {
    if (capturing) return 0x0b4474;
    if (playing) return 0x4d2870;
    return 0;
  }
  void updateVoiceLed(bool capturing, bool playing);
  uint32_t voiceLedRgb() const { return voiceLedRgb_; }
  void setTrackingPose(int yaw, int pitch, bool reposition = true);
  void applyGesturePose(const HeadPose &pose);
  void stopGestureMotion();

  bool setMotionEnabled(bool enabled);
  bool motionEnabled() const;
  void setHomePitch(uint8_t degrees, bool reposition);
  uint8_t homePitch() const { return homePitch_; }
  int currentPitch();
  int currentYaw();
  void runConservativeMotionTest();
  bool headTouched() const;
  uint8_t headTouchStrength() const;
  float batteryVoltage() const;
  float batteryCurrent() const;
  void runSpeakerTest();
  uint32_t sampleMicrophoneRms();
  bool speakerQueueHasRoom() const;
  bool queueSpeakerPcm(const int16_t *samples, size_t sampleCount,
                       uint32_t sampleRate, uint8_t volume);
  bool speakerPlaying();
  bool speakerStreamOpen() const { return speakerStreamActive_; }
  void setSpeakerLevel(uint8_t percent);
  uint8_t speakerLevel() const { return speakerLevel_; }
  struct SpeakerBlock {
    const int16_t *samples = nullptr;
    size_t count = 0;
    uint32_t sequence = 0;
  };
  SpeakerBlock currentSpeakerBlock() const;
  void stopSpeakerStream();
  bool finishSpeakerStream();

private:
  bool motionEnabled_ = false;
  uint8_t homePitch_ = 8;
  int trackingYaw_ = 0;
  int trackingPitch_ = 0;
  uint32_t voiceLedRgb_ = 0;
  bool gestureMotionActive_ = false;
  std::array<std::array<int16_t, kSpeakerBlockSamples>, 3> speakerBlocks_{};
  std::array<SpeakerBlock, 3> speakerBlockMetadata_{};
  uint32_t speakerBlockSequence_ = 0;
  size_t speakerBlockIndex_ = 0;
  uint32_t speakerSampleRate_ = 0;
  uint8_t speakerVolume_ = 0;
  uint8_t speakerLevel_ = 100;
  void updateSpeakerVolume();
  bool speakerStreamActive_ = false;
  uint32_t speakerDmaDrainMs_ = 0;
  uint32_t speakerLastMixerActivityAt_ = 0;
};

} // namespace charadock
