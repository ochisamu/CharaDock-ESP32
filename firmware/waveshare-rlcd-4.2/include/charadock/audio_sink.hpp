// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace charadock::rlcd {

enum class AudioApplyResult : uint8_t {
  Ok = 0,
  Unavailable = 1,
  InvalidFormat = 2,
  InvalidState = 3,
  BufferFull = 4,
  CodecFailure = 5,
};

class AudioSink {
public:
  virtual ~AudioSink() = default;
  virtual AudioApplyResult startPlayback(uint32_t sampleRate,
                                         uint32_t totalSamples) = 0;
  virtual AudioApplyResult writePcm16(const uint8_t *bytes,
                                      size_t length) = 0;
  virtual AudioApplyResult finishPlayback() = 0;
  virtual AudioApplyResult stopPlayback() = 0;
};

const char *audioApplyResultName(AudioApplyResult result);

} // namespace charadock::rlcd
