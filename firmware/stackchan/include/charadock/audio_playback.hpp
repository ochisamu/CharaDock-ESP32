// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace charadock {

enum class AudioBufferResult : uint8_t {
  Ok = 0,
  StorageUnavailable = 1,
  InvalidFormat = 2,
  SessionNotActive = 3,
  InvalidChunk = 4,
  BufferFull = 5,
  SequenceConflict = 6,
  Duplicate = 7,
  AlreadyEnded = 8,
};

enum class AudioPlaybackPhase : uint8_t {
  Stopped = 0,
  Buffering = 1,
  Playing = 2,
  Draining = 3,
  Finished = 4,
};

struct AudioStreamConfig {
  uint32_t sampleRate = 16000;
  uint16_t prebufferMs = 200;
  uint8_t channels = 1;
  uint8_t bitsPerSample = 16;
  uint8_t volume = 72;
  bool signedSamples = true;
  bool littleEndian = true;
};

class PcmPlaybackBuffer {
public:
  static constexpr uint32_t kRequiredSampleRate = 16000;
  static constexpr uint16_t kMinimumPrebufferMs = 80;
  static constexpr uint16_t kMaximumPrebufferMs = 500;

  bool attach(int16_t *storage, size_t sampleCapacity);
  AudioBufferResult begin(const AudioStreamConfig &config, uint16_t sequence);
  AudioBufferResult appendChunk(uint16_t sequence, const uint8_t *pcmBytes,
                                size_t byteCount);
  AudioBufferResult end(uint16_t sequence);
  void stop();

  // prepareSamples copies without consuming. Call commitSamples only after the
  // hardware speaker accepts the block, so a busy output queue loses no audio.
  size_t prepareSamples(int16_t *destination, size_t maximumSamples);
  bool commitSamples(size_t sampleCount);

  bool storageReady() const;
  bool active() const;
  bool producerEnded() const;
  AudioPlaybackPhase phase() const;
  const AudioStreamConfig &config() const;
  size_t bufferedSamples() const;
  size_t freeSamples() const;
  size_t capacitySamples() const;
  size_t prebufferSamples() const;
  uint32_t generation() const;
  uint32_t underrunCount() const;

private:
  struct RecentChunk {
    uint16_t sequence = 0;
    uint32_t fingerprint = 0;
    uint16_t byteCount = 0;
    bool valid = false;
  };

  static bool configsEqual(const AudioStreamConfig &left,
                           const AudioStreamConfig &right);
  static uint32_t fingerprint(const uint8_t *bytes, size_t length);
  AudioBufferResult duplicateResult(uint16_t sequence, uint32_t fingerprint,
                                    size_t byteCount) const;
  void rememberChunk(uint16_t sequence, uint32_t fingerprint, size_t byteCount);
  void clearStreamData();

  int16_t *storage_ = nullptr;
  size_t capacity_ = 0;
  size_t readIndex_ = 0;
  size_t writeIndex_ = 0;
  size_t buffered_ = 0;
  AudioStreamConfig config_{};
  AudioPlaybackPhase phase_ = AudioPlaybackPhase::Stopped;
  uint32_t generation_ = 0;
  uint32_t underrunCount_ = 0;
  uint16_t beginSequence_ = 0;
  uint16_t endSequence_ = 0;
  bool hasBeginSequence_ = false;
  bool hasEndSequence_ = false;
  bool producerEnded_ = false;
  std::array<RecentChunk, 8> recentChunks_{};
  size_t recentChunkIndex_ = 0;
};

const char *audioBufferResultName(AudioBufferResult result);
const char *audioPlaybackPhaseName(AudioPlaybackPhase phase);

} // namespace charadock
