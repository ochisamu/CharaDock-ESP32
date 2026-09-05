// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "charadock/presentation.hpp"

namespace charadock {

class HardwareController {
public:
  static constexpr size_t kSpeakerBlockSamples = 320;

  void begin();
  void update();
  void applyPresentation(const PresentationSnapshot &snapshot, uint32_t nowMs);

  bool setMotionEnabled(bool enabled);
  bool motionEnabled() const;
  void runConservativeMotionTest();
  bool headTouched() const;
  float batteryVoltage() const;
  float batteryCurrent() const;
  void runSpeakerTest();
  uint32_t sampleMicrophoneRms();
  bool speakerQueueHasRoom() const;
  bool queueSpeakerPcm(const int16_t *samples, size_t sampleCount,
                       uint32_t sampleRate, uint8_t volume);
  bool speakerPlaying() const;
  void stopSpeakerStream();
  bool finishSpeakerStream();

private:
  void updateLed(const PresentationSnapshot &snapshot);
  void updateMotion(const PresentationSnapshot &snapshot, uint32_t nowMs);

  bool motionEnabled_ = false;
  bool ledInitialized_ = false;
  DeviceState lastLedState_ = DeviceState::Connecting;
  uint32_t lastLedAccent_ = 0;
  DeviceState lastMotionState_ = DeviceState::Connecting;
  uint32_t lastSpeakingMotionAt_ = 0;
  bool speakingNod_ = false;
  std::array<std::array<int16_t, kSpeakerBlockSamples>, 3> speakerBlocks_{};
  size_t speakerBlockIndex_ = 0;
  uint32_t speakerSampleRate_ = 0;
  uint8_t speakerVolume_ = 0;
  bool speakerStreamActive_ = false;
};

} // namespace charadock
