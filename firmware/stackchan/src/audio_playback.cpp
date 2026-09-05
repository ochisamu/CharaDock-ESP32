// SPDX-License-Identifier: Apache-2.0
#include "charadock/audio_playback.hpp"

#include <algorithm>
#include <cstring>

namespace charadock {

bool PcmPlaybackBuffer::attach(int16_t *storage, size_t sampleCapacity) {
  stop();
  underrunCount_ = 0;
  storage_ = nullptr;
  capacity_ = 0;
  if (!storage || sampleCapacity == 0)
    return false;
  storage_ = storage;
  capacity_ = sampleCapacity;
  return true;
}

AudioBufferResult PcmPlaybackBuffer::begin(const AudioStreamConfig &config,
                                           uint16_t sequence) {
  if (!storageReady())
    return AudioBufferResult::StorageUnavailable;
  const uint64_t requestedPrebuffer =
      static_cast<uint64_t>(config.sampleRate) * config.prebufferMs / 1000;
  if (config.sampleRate != kRequiredSampleRate || config.channels != 1 ||
      config.bitsPerSample != 16 || !config.signedSamples ||
      !config.littleEndian || config.prebufferMs < kMinimumPrebufferMs ||
      config.prebufferMs > kMaximumPrebufferMs ||
      requestedPrebuffer > capacity_) {
    return AudioBufferResult::InvalidFormat;
  }
  if (hasBeginSequence_ && beginSequence_ == sequence) {
    return configsEqual(config_, config) ? AudioBufferResult::Duplicate
                                         : AudioBufferResult::SequenceConflict;
  }
  clearStreamData();
  underrunCount_ = 0;
  config_ = config;
  beginSequence_ = sequence;
  hasBeginSequence_ = true;
  phase_ = AudioPlaybackPhase::Buffering;
  ++generation_;
  return AudioBufferResult::Ok;
}

AudioBufferResult PcmPlaybackBuffer::appendChunk(uint16_t sequence,
                                                 const uint8_t *pcmBytes,
                                                 size_t byteCount) {
  if (!pcmBytes || byteCount == 0 || (byteCount & 1u) || byteCount > 0xffffu)
    return AudioBufferResult::InvalidChunk;
  const uint32_t chunkFingerprint = fingerprint(pcmBytes, byteCount);
  const AudioBufferResult duplicate =
      duplicateResult(sequence, chunkFingerprint, byteCount);
  if (duplicate != AudioBufferResult::Ok)
    return duplicate;
  if (!active())
    return AudioBufferResult::SessionNotActive;
  if (producerEnded_)
    return AudioBufferResult::AlreadyEnded;
  const size_t sampleCount = byteCount / sizeof(int16_t);
  if (sampleCount > freeSamples())
    return AudioBufferResult::BufferFull;
  for (size_t index = 0; index < sampleCount; ++index) {
    const size_t byteIndex = index * 2;
    const uint16_t sample = static_cast<uint16_t>(pcmBytes[byteIndex]) |
                            static_cast<uint16_t>(pcmBytes[byteIndex + 1]) << 8;
    storage_[writeIndex_] = static_cast<int16_t>(sample);
    writeIndex_ = (writeIndex_ + 1) % capacity_;
  }
  buffered_ += sampleCount;
  rememberChunk(sequence, chunkFingerprint, byteCount);
  return AudioBufferResult::Ok;
}

AudioBufferResult PcmPlaybackBuffer::end(uint16_t sequence) {
  if (hasEndSequence_ && endSequence_ == sequence)
    return AudioBufferResult::Duplicate;
  if (!active())
    return AudioBufferResult::SessionNotActive;
  if (producerEnded_)
    return AudioBufferResult::AlreadyEnded;
  producerEnded_ = true;
  hasEndSequence_ = true;
  endSequence_ = sequence;
  if (phase_ == AudioPlaybackPhase::Playing)
    phase_ = AudioPlaybackPhase::Draining;
  if (phase_ == AudioPlaybackPhase::Buffering && buffered_ == 0)
    phase_ = AudioPlaybackPhase::Finished;
  return AudioBufferResult::Ok;
}

void PcmPlaybackBuffer::stop() {
  clearStreamData();
  phase_ = AudioPlaybackPhase::Stopped;
  hasBeginSequence_ = false;
  ++generation_;
}

size_t PcmPlaybackBuffer::prepareSamples(int16_t *destination,
                                         size_t maximumSamples) {
  if (!destination || maximumSamples == 0)
    return 0;
  if (phase_ == AudioPlaybackPhase::Buffering) {
    if (buffered_ == 0 && producerEnded_) {
      phase_ = AudioPlaybackPhase::Finished;
      return 0;
    }
    if (!producerEnded_ && buffered_ < prebufferSamples())
      return 0;
    phase_ = producerEnded_ ? AudioPlaybackPhase::Draining
                            : AudioPlaybackPhase::Playing;
  }
  if (phase_ != AudioPlaybackPhase::Playing &&
      phase_ != AudioPlaybackPhase::Draining) {
    return 0;
  }
  if (buffered_ == 0) {
    if (producerEnded_) {
      phase_ = AudioPlaybackPhase::Finished;
    } else {
      phase_ = AudioPlaybackPhase::Buffering;
      ++underrunCount_;
    }
    return 0;
  }
  const size_t count = std::min(maximumSamples, buffered_);
  for (size_t index = 0; index < count; ++index)
    destination[index] = storage_[(readIndex_ + index) % capacity_];
  return count;
}

bool PcmPlaybackBuffer::commitSamples(size_t sampleCount) {
  if (sampleCount == 0 || sampleCount > buffered_)
    return false;
  readIndex_ = (readIndex_ + sampleCount) % capacity_;
  buffered_ -= sampleCount;
  return true;
}

bool PcmPlaybackBuffer::storageReady() const {
  return storage_ && capacity_ > 0;
}

bool PcmPlaybackBuffer::active() const {
  return phase_ == AudioPlaybackPhase::Buffering ||
         phase_ == AudioPlaybackPhase::Playing ||
         phase_ == AudioPlaybackPhase::Draining;
}

bool PcmPlaybackBuffer::producerEnded() const { return producerEnded_; }

AudioPlaybackPhase PcmPlaybackBuffer::phase() const { return phase_; }

const AudioStreamConfig &PcmPlaybackBuffer::config() const { return config_; }

size_t PcmPlaybackBuffer::bufferedSamples() const { return buffered_; }

size_t PcmPlaybackBuffer::freeSamples() const { return capacity_ - buffered_; }

size_t PcmPlaybackBuffer::capacitySamples() const { return capacity_; }

size_t PcmPlaybackBuffer::prebufferSamples() const {
  return static_cast<size_t>(config_.sampleRate) * config_.prebufferMs / 1000;
}

uint32_t PcmPlaybackBuffer::generation() const { return generation_; }

uint32_t PcmPlaybackBuffer::underrunCount() const { return underrunCount_; }

bool PcmPlaybackBuffer::configsEqual(const AudioStreamConfig &left,
                                     const AudioStreamConfig &right) {
  return left.sampleRate == right.sampleRate &&
         left.prebufferMs == right.prebufferMs &&
         left.channels == right.channels &&
         left.bitsPerSample == right.bitsPerSample &&
         left.volume == right.volume &&
         left.signedSamples == right.signedSamples &&
         left.littleEndian == right.littleEndian;
}

uint32_t PcmPlaybackBuffer::fingerprint(const uint8_t *bytes, size_t length) {
  uint32_t value = 2166136261u;
  for (size_t index = 0; index < length; ++index) {
    value ^= bytes[index];
    value *= 16777619u;
  }
  return value;
}

AudioBufferResult PcmPlaybackBuffer::duplicateResult(uint16_t sequence,
                                                     uint32_t fingerprintValue,
                                                     size_t byteCount) const {
  for (const auto &recent : recentChunks_) {
    if (!recent.valid || recent.sequence != sequence)
      continue;
    return recent.fingerprint == fingerprintValue &&
                   recent.byteCount == byteCount
               ? AudioBufferResult::Duplicate
               : AudioBufferResult::SequenceConflict;
  }
  return AudioBufferResult::Ok;
}

void PcmPlaybackBuffer::rememberChunk(uint16_t sequence,
                                      uint32_t fingerprintValue,
                                      size_t byteCount) {
  auto &recent = recentChunks_[recentChunkIndex_];
  recent.sequence = sequence;
  recent.fingerprint = fingerprintValue;
  recent.byteCount = static_cast<uint16_t>(byteCount);
  recent.valid = true;
  recentChunkIndex_ = (recentChunkIndex_ + 1) % recentChunks_.size();
}

void PcmPlaybackBuffer::clearStreamData() {
  readIndex_ = 0;
  writeIndex_ = 0;
  buffered_ = 0;
  producerEnded_ = false;
  hasEndSequence_ = false;
  recentChunks_.fill(RecentChunk{});
  recentChunkIndex_ = 0;
}

const char *audioBufferResultName(AudioBufferResult result) {
  switch (result) {
  case AudioBufferResult::Ok:
    return "ok";
  case AudioBufferResult::StorageUnavailable:
    return "storage-unavailable";
  case AudioBufferResult::InvalidFormat:
    return "invalid-format";
  case AudioBufferResult::SessionNotActive:
    return "session-not-active";
  case AudioBufferResult::InvalidChunk:
    return "invalid-chunk";
  case AudioBufferResult::BufferFull:
    return "buffer-full";
  case AudioBufferResult::SequenceConflict:
    return "sequence-conflict";
  case AudioBufferResult::Duplicate:
    return "duplicate";
  case AudioBufferResult::AlreadyEnded:
    return "already-ended";
  }
  return "unknown";
}

const char *audioPlaybackPhaseName(AudioPlaybackPhase phase) {
  switch (phase) {
  case AudioPlaybackPhase::Stopped:
    return "stopped";
  case AudioPlaybackPhase::Buffering:
    return "buffering";
  case AudioPlaybackPhase::Playing:
    return "playing";
  case AudioPlaybackPhase::Draining:
    return "draining";
  case AudioPlaybackPhase::Finished:
    return "finished";
  }
  return "unknown";
}

} // namespace charadock
