// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cmath>

namespace charadock {

// StackChan submits 20 ms PCM blocks. The desktop handles utterance admission.
class HandsFreeVoice {
public:
  static uint16_t centeredRms(const int16_t *samples, size_t count) {
    if (!count) return 0;
    int64_t sum = 0;
    uint64_t squareSum = 0;
    for (size_t i = 0; i < count; ++i) {
      const int32_t sample = samples[i];
      sum += sample;
      squareSum += static_cast<uint32_t>(sample * sample);
    }
    const double mean = static_cast<double>(sum) / count;
    const double variance = static_cast<double>(squareSum) / count - mean * mean;
    return std::sqrt(std::max(0.0, variance));
  }

  bool observe(uint16_t rms, uint16_t sensitivity, bool recording) {
    if (recording) {
      silence_ = rms < continueThreshold(sensitivity) ? silence_ + 1 : 0;
      return silence_ >= 25;
    }
    if (calibration_) {
      // The I2S reader can deliver zeros and settling transients at start.
      // Ignore its first five blocks, then take the median of the next 15.
      if (calibration_ <= calibrationSamples_.size())
        calibrationSamples_[calibrationSamples_.size() - calibration_] = rms;
      if (!--calibration_) {
        auto middle = calibrationSamples_.begin() + calibrationSamples_.size() / 2;
        std::nth_element(calibrationSamples_.begin(), middle, calibrationSamples_.end());
        floor_ = std::max<uint16_t>(40, *middle);
      }
      return false;
    }
    if (settling_) {
      --settling_;
      return false;
    }
    const auto threshold = startThreshold(sensitivity);
    speech_ = rms >= threshold ? std::min<int>(speech_ + 1, 3) : 0;
    if (!speech_) floor_ = std::max<uint16_t>(40, (floor_ * 31 + rms + 16) / 32);
    return speech_ >= 3;
  }

  void rearm() {
    speech_ = silence_ = 0;
    if (!calibration_) settling_ = 3;
  }

  uint16_t startThreshold(uint16_t sensitivity) const {
    return std::min<uint32_t>(32768, std::max<uint32_t>(sensitivity, floor_ * 2u + 80u));
  }
  uint16_t continueThreshold(uint16_t sensitivity) const {
    return std::max<uint16_t>(sensitivity * 3u / 4u, floor_ + std::max<uint16_t>(35, floor_ / 3u));
  }
  uint16_t floor() const { return floor_; }
  uint8_t speech() const { return speech_; }
  uint8_t silence() const { return silence_; }
  uint8_t calibration() const { return calibration_; }

private:
  std::array<uint16_t, 15> calibrationSamples_{};
  uint16_t floor_ = 40;
  uint8_t calibration_ = 20;
  uint8_t settling_ = 0;
  uint8_t speech_ = 0;
  uint8_t silence_ = 0;
};

} // namespace charadock
