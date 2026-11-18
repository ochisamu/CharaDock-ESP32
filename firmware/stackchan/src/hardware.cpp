// SPDX-License-Identifier: Apache-2.0
#include "charadock/hardware.hpp"
#include "charadock/idle_backlight.hpp"

#include <Arduino.h>
#include <StackChan-BSP.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace charadock {

void HardwareController::begin() {
  M5StackChan.begin();
  setBacklight(IdleBacklight::kActiveBrightness);
  // The BSP's idle check queries the servo bus even after torque has been
  // released. Skip those queries completely when the servo rail is off.
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(false);
  M5StackChan.Motion.setTorqueEnabled(false);
  M5StackChan.setServoPowerEnabled(false);
  M5StackChan.showRgbColor(0, 0, 0);
  M5.Speaker.end();
  M5.Mic.end();
  auto micConfig = M5.Mic.config();
  // The CoreS3 default gain is too quiet for speech at the shell's mic ports.
  micConfig.magnification = 8;
  M5.Mic.config(micConfig);
#if CHARADOCK_ENABLE_SERVO_ON_BOOT
  setMotionEnabled(true);
#endif
}

void HardwareController::update() { M5StackChan.update(); }

void HardwareController::setBacklight(uint8_t brightness) {
  if (backlight() != brightness)
    M5StackChan.Display().setBrightness(brightness);
}

uint8_t HardwareController::backlight() const {
  return M5StackChan.Display().getBrightness();
}

bool HardwareController::setMotionEnabled(bool enabled) {
  if (motionEnabled_ == enabled)
    return motionEnabled_;
  motionEnabled_ = enabled;
  if (enabled) {
    M5StackChan.setServoPowerEnabled(true);
    delay(80);
    M5StackChan.Motion.setTorqueEnabled(true);
    M5StackChan.Motion.move(0, homePitch_ * 10, 430);
    M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  } else {
    M5StackChan.Motion.setAutoTorqueReleaseEnabled(false);
    gestureMotionActive_ = false;
    trackingYaw_ = trackingPitch_ = 0;
    M5StackChan.Motion.stop();
    M5StackChan.Motion.setTorqueEnabled(false);
    M5StackChan.setServoPowerEnabled(false);
  }
  return motionEnabled_;
}

bool HardwareController::motionEnabled() const { return motionEnabled_; }
int HardwareController::currentPitch() { return M5StackChan.Motion.getCurrentPitchAngle(); }
int HardwareController::currentYaw() { return M5StackChan.Motion.getCurrentYawAngle(); }

void HardwareController::setHomePitch(uint8_t degrees, bool reposition) {
  homePitch_ = degrees;
  if (motionEnabled_ && reposition && !gestureMotionActive_) {
    M5StackChan.Motion.setTorqueEnabled(true);
    M5StackChan.Motion.movePitch(std::max(50, homePitch_ * 10 + trackingPitch_), 335);
  }
}

void HardwareController::runConservativeMotionTest() {
  if (!motionEnabled_)
    setMotionEnabled(true);
  M5StackChan.Motion.move(-80, homePitch_ * 10 + 50, 160);
  delay(650);
  M5StackChan.Motion.move(80, homePitch_ * 10 + 50, 160);
  delay(650);
  M5StackChan.Motion.move(0, homePitch_ * 10, 160);
}

uint8_t HardwareController::headTouchStrength() const {
  const auto &zones = M5StackChan.TouchSensor.getIntensities();
  return *std::max_element(zones.begin(), zones.end());
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
  static int16_t samples[kSamples];
  M5.Speaker.end();
  if (!M5.Mic.isEnabled())
    M5.Mic.begin();
  if (!M5.Mic.record(samples, kSamples, 16000))
    return 0;
  const uint32_t started = millis();
  while (M5.Mic.isRecording() && millis() - started < 250) delay(1);
  if (M5.Mic.isRecording()) { M5.Mic.end(); return 0; }
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

void HardwareController::setSpeakerLevel(uint8_t percent) {
  speakerLevel_ = std::min<uint8_t>(percent, 100);
  if (speakerStreamActive_) updateSpeakerVolume();
}

void HardwareController::updateSpeakerVolume() {
  // M5Unified squares the mixer volume. Convert the desired linear amplitude.
  M5.Speaker.setVolume(static_cast<uint8_t>(std::lround(
      std::sqrt(speakerVolume_ * 255.0 * speakerLevel_ / 100.0))));
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
    M5.Mic.end();
    if (!M5.Speaker.begin())
      return false;
    const auto speakerConfig = M5.Speaker.config();
    const uint32_t dmaSamples = speakerConfig.dma_buf_len * speakerConfig.dma_buf_count;
    speakerDmaDrainMs_ = (dmaSamples * 1000 + speakerConfig.sample_rate - 1) /
        speakerConfig.sample_rate + 5;
    speakerSampleRate_ = sampleRate;
    speakerVolume_ = volume;
    updateSpeakerVolume();
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
  speakerBlockMetadata_[speakerBlockIndex_] = {
      block.data(), sampleCount, ++speakerBlockSequence_};
  speakerBlockIndex_ = (speakerBlockIndex_ + 1) % speakerBlocks_.size();
  speakerLastMixerActivityAt_ = millis();
  return true;
}

bool HardwareController::speakerPlaying() {
  if (!speakerStreamActive_) return false;
  const uint32_t now = millis();
  if (M5.Speaker.isPlaying(0)) {
    speakerLastMixerActivityAt_ = now;
    return true;
  }
  // The mixer releases PCM slots before I2S has clocked out their samples.
  // Keep the amplifier on until the final DMA buffers have played.
  return now - speakerLastMixerActivityAt_ < speakerDmaDrainMs_;
}

HardwareController::SpeakerBlock HardwareController::currentSpeakerBlock() const {
  const size_t queued = speakerStreamActive_ ? M5.Speaker.isPlaying(0) : 0;
  if (!queued || queued >= speakerBlocks_.size()) return {};
  return speakerBlockMetadata_[(speakerBlockIndex_ + speakerBlocks_.size() - queued) %
                               speakerBlocks_.size()];
}

void HardwareController::stopSpeakerStream() {
  if (speakerStreamActive_) M5.Speaker.end();
  speakerStreamActive_ = false;
  speakerBlockIndex_ = 0;
  speakerSampleRate_ = 0;
  speakerVolume_ = 0;
}

bool HardwareController::finishSpeakerStream() {
  if (speakerPlaying())
    return false;
  stopSpeakerStream();
  return true;
}

void HardwareController::updateVoiceLed(bool capturing, bool playing) {
  const uint32_t color = voiceLedColor(capturing, playing);
  if (voiceLedRgb_ == color) return;
  M5StackChan.showRgbColor(static_cast<uint8_t>(color >> 16),
                         static_cast<uint8_t>(color >> 8),
                         static_cast<uint8_t>(color));
  voiceLedRgb_ = color;
}

void HardwareController::setTrackingPose(int yaw, int pitch, bool reposition) {
  if (trackingYaw_ == yaw && trackingPitch_ == pitch) return;
  trackingYaw_ = yaw;
  trackingPitch_ = pitch;
  if (motionEnabled_ && reposition)
    applyGesturePose({0, 0, 300});
}

void HardwareController::applyGesturePose(const HeadPose &pose) {
  if (!motionEnabled_) return;
  gestureMotionActive_ = pose.yaw != 0 || pose.pitch != 0;
  M5StackChan.Motion.setTorqueEnabled(true);
  M5StackChan.Motion.move(trackingYaw_ + pose.yaw,
                          std::clamp(homePitch_ * 10 + trackingPitch_ + pose.pitch, 50, 290),
                          pose.speed);
}

void HardwareController::stopGestureMotion() {
  if (gestureMotionActive_ && motionEnabled_) {
    gestureMotionActive_ = false;
    M5StackChan.Motion.stop();
    M5StackChan.Motion.setTorqueEnabled(false);
  }
}

} // namespace charadock
