// SPDX-License-Identifier: Apache-2.0
#include "charadock/mouth_envelope.hpp"

#include <algorithm>
#include <cmath>

namespace charadock {

MouthEnvelope::MouthEnvelope(const MouthEnvelopeConfig &config)
    : config_(config) {
  config_.minimumUpdateMs = std::max<uint16_t>(100, config_.minimumUpdateMs);
  config_.fullOpenRms =
      std::max<uint16_t>(config_.fullOpenRms, config_.halfOpenRms + 1);
  config_.halfCloseRms =
      std::min<uint16_t>(config_.halfCloseRms, config_.halfOpenRms);
  config_.fullCloseRms = std::max<uint16_t>(
      config_.halfCloseRms,
      std::min<uint16_t>(config_.fullCloseRms, config_.fullOpenRms));
}

MouthEnvelopeUpdate MouthEnvelope::observePlayedSamples(const int16_t *samples,
                                                        size_t sampleCount,
                                                        uint32_t nowMs) {
  uint64_t energy = 0;
  if (samples) {
    for (size_t index = 0; index < sampleCount; ++index) {
      const int32_t value = samples[index];
      energy += static_cast<uint64_t>(value * value);
    }
  }
  lastRms_ =
      sampleCount && samples
          ? static_cast<uint16_t>(std::min<double>(
                65535.0, std::sqrt(static_cast<double>(energy) / sampleCount)))
          : 0;
  const uint8_t target = targetForRms(lastRms_);
  MouthEnvelopeUpdate update{lastRms_, level_, false};
  if (target > level_) {
    releasePending_ = false;
    if (updateIntervalElapsed(nowMs)) {
      level_ = target;
      lastUpdateAt_ = nowMs;
      hasUpdated_ = true;
      update.level = level_;
      update.changed = true;
    }
    return update;
  }
  if (target == level_) {
    releasePending_ = false;
    return update;
  }
  if (!releasePending_ || pendingReleaseLevel_ != target) {
    releasePending_ = true;
    pendingReleaseLevel_ = target;
    releaseStartedAt_ = nowMs;
    return update;
  }
  if (nowMs - releaseStartedAt_ >= config_.releaseHoldMs &&
      updateIntervalElapsed(nowMs)) {
    level_ = pendingReleaseLevel_;
    lastUpdateAt_ = nowMs;
    hasUpdated_ = true;
    releasePending_ = false;
    update.level = level_;
    update.changed = true;
  }
  return update;
}

MouthEnvelopeUpdate MouthEnvelope::close(uint32_t nowMs) {
  MouthEnvelopeUpdate update{0, level_, false};
  lastRms_ = 0;
  releasePending_ = false;
  if (level_ != 0) {
    level_ = 0;
    lastUpdateAt_ = nowMs;
    hasUpdated_ = true;
    update.level = 0;
    update.changed = true;
  }
  return update;
}

void MouthEnvelope::reset(uint32_t nowMs) {
  level_ = 0;
  pendingReleaseLevel_ = 0;
  lastRms_ = 0;
  lastUpdateAt_ = nowMs;
  releaseStartedAt_ = nowMs;
  hasUpdated_ = false;
  releasePending_ = false;
}

uint8_t MouthEnvelope::level() const { return level_; }

uint16_t MouthEnvelope::lastRms() const { return lastRms_; }

uint8_t MouthEnvelope::targetForRms(uint16_t rms) const {
  if (level_ == 2 && rms >= config_.fullCloseRms)
    return 2;
  if (rms >= config_.fullOpenRms)
    return 2;
  if (level_ >= 1 && rms >= config_.halfCloseRms)
    return 1;
  return rms >= config_.halfOpenRms ? 1 : 0;
}

bool MouthEnvelope::updateIntervalElapsed(uint32_t nowMs) const {
  return !hasUpdated_ || nowMs - lastUpdateAt_ >= config_.minimumUpdateMs;
}

} // namespace charadock
