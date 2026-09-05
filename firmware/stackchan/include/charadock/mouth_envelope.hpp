// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace charadock {

struct MouthEnvelopeConfig {
  uint16_t halfOpenRms = 550;
  uint16_t fullOpenRms = 1800;
  uint16_t halfCloseRms = 350;
  uint16_t fullCloseRms = 1300;
  uint16_t minimumUpdateMs = 100;
  uint16_t releaseHoldMs = 100;
};

struct MouthEnvelopeUpdate {
  uint16_t rms = 0;
  uint8_t level = 0;
  bool changed = false;
};

// Feed this controller only after a PCM block has actually been accepted by
// the speaker path. This keeps the face aligned to heard audio rather than to
// data merely received from the network.
class MouthEnvelope {
public:
  explicit MouthEnvelope(const MouthEnvelopeConfig &config = {});

  MouthEnvelopeUpdate observePlayedSamples(const int16_t *samples,
                                           size_t sampleCount, uint32_t nowMs);
  MouthEnvelopeUpdate close(uint32_t nowMs);
  void reset(uint32_t nowMs = 0);

  uint8_t level() const;
  uint16_t lastRms() const;

private:
  uint8_t targetForRms(uint16_t rms) const;
  bool updateIntervalElapsed(uint32_t nowMs) const;

  MouthEnvelopeConfig config_;
  uint8_t level_ = 0;
  uint8_t pendingReleaseLevel_ = 0;
  uint16_t lastRms_ = 0;
  uint32_t lastUpdateAt_ = 0;
  uint32_t releaseStartedAt_ = 0;
  bool hasUpdated_ = false;
  bool releasePending_ = false;
};

} // namespace charadock
