// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "charadock/audio_sink.hpp"

namespace charadock::rlcd {

enum class AudioPlaybackEvent : uint8_t {
  None = 0,
  Completed = 1,
  Failed = 2,
};

// USB PCM playback for the Waveshare RLCD 4.2 ES8311 -> NS4150B path.
// Implementation details live behind Impl so protocol/model host tests do not
// need ESP-IDF or FreeRTOS headers.
class AudioOutput final : public AudioSink {
public:
  AudioOutput();
  ~AudioOutput();

  bool begin();
  bool available() const;
  bool active() const;
  uint8_t mouthLevel() const;
  uint32_t underruns() const;
  AudioPlaybackEvent pollEvent();

  AudioApplyResult startPlayback(uint32_t sampleRate,
                                 uint32_t totalSamples) override;
  AudioApplyResult writePcm16(const uint8_t *bytes,
                              size_t length) override;
  AudioApplyResult finishPlayback() override;
  AudioApplyResult stopPlayback() override;

private:
  struct Impl;
  Impl *impl_ = nullptr;
};

} // namespace charadock::rlcd
