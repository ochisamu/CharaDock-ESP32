// SPDX-License-Identifier: Apache-2.0
#include "charadock/hardware.hpp"

#include <Arduino.h>
#include <StackChan-BSP.h>

#include <cmath>
#include <cstring>

namespace charadock {
namespace {

uint8_t channel(uint32_t rgb, uint8_t shift, uint8_t scale = 1) {
  return static_cast<uint8_t>((((rgb >> shift) & 0xff) * scale) / 4);
}

} // namespace

void HardwareController::begin() {
  M5StackChan.begin();
  M5StackChan.Display().setBrightness(128);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  M5StackChan.Motion.setTorqueEnabled(false);
  M5StackChan.setServoPowerEnabled(false);
  M5StackChan.showRgbColor(0, 0, 0);
  M5.Speaker.end();
  M5.Mic.begin();
#if CHARADOCK_ENABLE_SERVO_ON_BOOT
  setMotionEnabled(true);
#endif
}

void HardwareController::update() { M5StackChan.update(); }

void HardwareController::applyPresentation(const PresentationSnapshot &snapshot,
                                           uint32_t nowMs) {
  updateLed(snapshot);
  updateMotion(snapshot, nowMs);
}

bool HardwareController::setMotionEnabled(bool enabled) {
  if (motionEnabled_ == enabled)
    return motionEnabled_;
  motionEnabled_ = enabled;
  if (enabled) {
    M5StackChan.setServoPowerEnabled(true);
    delay(80);
    M5StackChan.Motion.setTorqueEnabled(true);
    M5StackChan.Motion.goHome(180);
  } else {
    M5StackChan.Motion.stop();
    M5StackChan.Motion.setTorqueEnabled(false);
    M5StackChan.setServoPowerEnabled(false);
  }
  return motionEnabled_;
}

bool HardwareController::motionEnabled() const { return motionEnabled_; }

void HardwareController::runConservativeMotionTest() {
  if (!motionEnabled_)
    setMotionEnabled(true);
  M5StackChan.Motion.move(-80, 50, 160);
  delay(650);
  M5StackChan.Motion.move(80, 50, 160);
  delay(650);
  M5StackChan.Motion.goHome(160);
}

bool HardwareController::headTouched() const {
  return M5StackChan.TouchSensor.isPressed();
}

float HardwareController::batteryVoltage() const {
  return M5StackChan.getBatteryVoltage();
}

float HardwareController::batteryCurrent() const {
  return M5StackChan.getBatteryCurrent();
}

void HardwareController::runSpeakerTest() {
  stopSpeakerStream();
  while (M5.Mic.isRecording())
    delay(1);
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(72);
  M5.Speaker.tone(523, 120);
  delay(150);
  M5.Speaker.tone(659, 120);
  delay(150);
  M5.Speaker.tone(784, 160);
  delay(190);
  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.begin();
}

uint32_t HardwareController::sampleMicrophoneRms() {
  constexpr size_t kSamples = 1024;
  int16_t samples[kSamples];
  M5.Speaker.end();
  if (!M5.Mic.isEnabled())
    M5.Mic.begin();
  if (!M5.Mic.record(samples, kSamples, 16000))
    return 0;
  uint64_t energy = 0;
  for (int16_t sample : samples) {
    const int32_t value = sample;
    energy += static_cast<uint64_t>(value * value);
  }
  return static_cast<uint32_t>(
      std::sqrt(static_cast<double>(energy) / kSamples));
}

bool HardwareController::speakerQueueHasRoom() const {
  return !speakerStreamActive_ || M5.Speaker.isPlaying(0) < 2;
}

bool HardwareController::queueSpeakerPcm(const int16_t *samples,
                                         size_t sampleCount,
                                         uint32_t sampleRate, uint8_t volume) {
  if (!samples || sampleCount == 0 || sampleCount > kSpeakerBlockSamples ||
      sampleRate != 16000)
    return false;
  if (speakerStreamActive_ &&
      (speakerSampleRate_ != sampleRate || speakerVolume_ != volume)) {
    stopSpeakerStream();
  }
  if (!speakerStreamActive_) {
    while (M5.Mic.isRecording())
      delay(1);
    M5.Mic.end();
    if (!M5.Speaker.begin()) {
      M5.Mic.begin();
      return false;
    }
    M5.Speaker.setVolume(volume);
    speakerSampleRate_ = sampleRate;
    speakerVolume_ = volume;
    speakerBlockIndex_ = 0;
    speakerStreamActive_ = true;
  }
  if (!speakerQueueHasRoom())
    return false;
  auto &block = speakerBlocks_[speakerBlockIndex_];
  std::memcpy(block.data(), samples, sampleCount * sizeof(int16_t));
  if (!M5.Speaker.playRaw(block.data(), sampleCount, sampleRate, false, 1, 0,
                          false)) {
    return false;
  }
  speakerBlockIndex_ = (speakerBlockIndex_ + 1) % speakerBlocks_.size();
  return true;
}

bool HardwareController::speakerPlaying() const {
  return speakerStreamActive_ && M5.Speaker.isPlaying(0);
}

void HardwareController::stopSpeakerStream() {
  if (speakerStreamActive_) {
    M5.Speaker.stop(0);
    M5.Speaker.end();
  }
  speakerStreamActive_ = false;
  speakerBlockIndex_ = 0;
  speakerSampleRate_ = 0;
  speakerVolume_ = 0;
  if (!M5.Mic.isEnabled())
    M5.Mic.begin();
}

bool HardwareController::finishSpeakerStream() {
  if (speakerPlaying())
    return false;
  stopSpeakerStream();
  return true;
}

void HardwareController::updateLed(const PresentationSnapshot &snapshot) {
  if (ledInitialized_ && lastLedState_ == snapshot.state &&
      lastLedAccent_ == snapshot.theme.accent)
    return;
  ledInitialized_ = true;
  lastLedState_ = snapshot.state;
  lastLedAccent_ = snapshot.theme.accent;
  uint32_t color = snapshot.theme.accent;
  uint8_t scale = 1;
  switch (snapshot.state) {
  case DeviceState::Idle:
    color = snapshot.theme.accent;
    scale = 1;
    break;
  case DeviceState::Listening:
    color = 0x1688e8;
    scale = 2;
    break;
  case DeviceState::Thinking:
    color = 0xe7a929;
    scale = 2;
    break;
  case DeviceState::Speaking:
    color = 0x9b51e0;
    scale = 2;
    break;
  case DeviceState::Error:
    color = 0xd92d20;
    scale = 2;
    break;
  case DeviceState::Connecting:
    color = snapshot.theme.accent;
    scale = 1;
    break;
  }
  M5StackChan.showRgbColor(channel(color, 16, scale), channel(color, 8, scale),
                           channel(color, 0, scale));
}

void HardwareController::updateMotion(const PresentationSnapshot &snapshot,
                                      uint32_t nowMs) {
  if (!motionEnabled_)
    return;
  const int intensity = snapshot.motionIntensity;
  if (lastMotionState_ != snapshot.state) {
    lastMotionState_ = snapshot.state;
    M5StackChan.Motion.setTorqueEnabled(true);
    if (snapshot.state == DeviceState::Thinking) {
      M5StackChan.Motion.move((80 * intensity) / 100, (60 * intensity) / 100,
                              160);
    } else {
      M5StackChan.Motion.goHome(180);
    }
  }
  if (snapshot.state == DeviceState::Speaking &&
      nowMs - lastSpeakingMotionAt_ >= 900) {
    lastSpeakingMotionAt_ = nowMs;
    speakingNod_ = !speakingNod_;
    const int pitch = speakingNod_ ? (35 * intensity) / 100 : 0;
    M5StackChan.Motion.setTorqueEnabled(true);
    M5StackChan.Motion.movePitch(pitch, 130);
  }
}

} // namespace charadock
