// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace charadock::rlcd {

// Half-duplex 16 kHz PCM16 capture from the RLCD 4.2 ES7210.  The output
// codec and input codec share I2S0, so callers must stop AudioOutput before
// starting capture and stop capture before starting playback.
class AudioInput {
public:
  AudioInput();
  ~AudioInput();

  bool begin();
  bool available() const;
  bool start();
  bool readPcm16(uint8_t *destination, size_t capacity, size_t &length,
                 uint32_t timeoutMs = 10);
  void stop();
  bool active() const;
  uint16_t rms() const;

private:
  struct Impl;
  Impl *impl_ = nullptr;
};

} // namespace charadock::rlcd
