// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "charadock/audio_playback.hpp"
#include "charadock/frame_dispatcher.hpp"
#include "charadock/mouth_envelope.hpp"
#include "charadock/portrait_cache.hpp"
#include "charadock/presentation.hpp"
#include "charadock/protocol_v2.hpp"

using charadock::PortraitCache;
using charadock::PortraitCacheResult;
using charadock::PortraitMetadata;
using charadock::PresentationController;
using charadock::protocol::Decoder;
using charadock::protocol::Frame;
using charadock::protocol::FrameType;

std::vector<uint8_t> pcmBytes(size_t sampleCount, int16_t firstSample = 0);

void frameRoundTripsAcrossFragments() {
  Frame source{FrameType::Capabilities, 42, {'{', '}', '\n'}};
  const auto encoded = charadock::protocol::encodeFrame(source);
  assert(!encoded.empty());
  Decoder decoder;
  std::vector<Frame> frames;
  for (uint8_t byte : encoded) {
    const auto part = decoder.push(&byte, 1);
    frames.insert(frames.end(), part.begin(), part.end());
  }
  assert(frames.size() == 1);
  assert(frames[0].type == FrameType::Capabilities);
  assert(frames[0].sequence == 42);
  assert(frames[0].payload == source.payload);
  assert(decoder.bufferedBytes() == 0);
}

void frameEncodingMatchesTheUsbToolKnownVector() {
  const auto encoded = charadock::protocol::encodeFrame(
      Frame{FrameType::DisplayMode, 0x1234, {1}});
  const std::vector<uint8_t> expected = {0x43, 0x44, 0x02, 0x54, 0x34,
                                         0x12, 0x01, 0x00, 0x45, 0x5f,
                                         0x98, 0x45, 0x01};
  assert(encoded == expected);
}

void invalidFrameRecoversToTheNextMagic() {
  auto invalid =
      charadock::protocol::encodeFrame(Frame{FrameType::State, 1, {3}});
  invalid.back() ^= 0xff;
  const auto valid =
      charadock::protocol::encodeFrame(Frame{FrameType::MouthLevel, 2, {2}});
  invalid.insert(invalid.end(), valid.begin(), valid.end());
  Decoder decoder;
  const auto frames = decoder.push(invalid.data(), invalid.size());
  assert(frames.size() == 1);
  assert(frames[0].type == FrameType::MouthLevel);
  assert(decoder.rejectedFrames() >= 1);
}

void presentationBoundsMouthAndSchedulesNaturalBlink() {
  PresentationController controller(1234);
  controller.setMouthLevel(99);
  assert(controller.snapshot().mouthLevel == 2);
  assert(controller.nextBlinkAt() >= 4000);
  assert(controller.nextBlinkAt() <= 7000);
  controller.update(controller.nextBlinkAt());
  assert(controller.snapshot().eyesClosed);
  controller.update(controller.nextBlinkAt() + 120);
  assert(!controller.snapshot().eyesClosed);
}

void artworkPolicyFallsBackToNativeFace() {
  PresentationController controller;
  controller.setDisplayMode(charadock::DisplayMode::CharacterArt);
  controller.setArtworkAllowed(false);
  assert(controller.snapshot().displayMode ==
         charadock::DisplayMode::NativeFace);
  controller.setDisplayMode(charadock::DisplayMode::CharacterArt);
  assert(controller.snapshot().displayMode ==
         charadock::DisplayMode::NativeFace);
}

PortraitMetadata metadataFor(const std::vector<uint8_t> &pixels,
                             const char *revision,
                             uint32_t checksumOverride = 0) {
  PortraitMetadata metadata;
  metadata.width = PortraitCache::kWidth;
  metadata.height = PortraitCache::kHeight;
  metadata.byteCount = PortraitCache::kByteCount;
  metadata.checksum =
      checksumOverride
          ? checksumOverride
          : (charadock::protocol::crc32(pixels.data(), pixels.size()) ^
             0xffffffffu);
  std::snprintf(metadata.revision.data(), metadata.revision.size(), "%s",
                revision);
  std::snprintf(metadata.frameName.data(), metadata.frameName.size(),
                "portrait");
  return metadata;
}

void portraitCacheCommitsAtomically() {
  std::vector<uint16_t> firstSlot(PortraitCache::kByteCount / 2);
  std::vector<uint16_t> secondSlot(PortraitCache::kByteCount / 2);
  PortraitCache cache;
  assert(cache.attach(firstSlot.data(), secondSlot.data(),
                      PortraitCache::kByteCount));

  std::vector<uint8_t> firstPortrait(PortraitCache::kByteCount, 0x2a);
  const auto firstMetadata = metadataFor(firstPortrait, "revision-one");
  assert(cache.beginTransfer(firstMetadata) == PortraitCacheResult::Ok);
  assert(cache.writeChunk(0, firstPortrait.data(), 4092) ==
         PortraitCacheResult::Ok);
  assert(cache.writeChunk(5000, firstPortrait.data(), 8) ==
         PortraitCacheResult::UnexpectedOffset);
  assert(cache.writeChunk(4092, firstPortrait.data() + 4092,
                          firstPortrait.size() - 4092) ==
         PortraitCacheResult::Ok);
  assert(cache.finishTransfer() == PortraitCacheResult::Ok);
  assert(cache.available());
  assert(reinterpret_cast<const uint8_t *>(cache.pixels())[0] == 0x2a);

  std::vector<uint8_t> brokenPortrait(PortraitCache::kByteCount, 0x7b);
  const auto brokenMetadata =
      metadataFor(brokenPortrait, "revision-two", 0x12345678u);
  assert(cache.beginTransfer(brokenMetadata) == PortraitCacheResult::Ok);
  assert(cache.writeChunk(0, brokenPortrait.data(), brokenPortrait.size()) ==
         PortraitCacheResult::Ok);
  assert(cache.finishTransfer() == PortraitCacheResult::ChecksumMismatch);
  assert(cache.available());
  assert(reinterpret_cast<const uint8_t *>(cache.pixels())[0] == 0x2a);
  assert(std::string(cache.metadata()->revision.data()) == "revision-one");

  const uint8_t wrongRevision[] = "another-revision";
  cache.invalidate(wrongRevision, sizeof(wrongRevision) - 1);
  assert(cache.available());
  const uint8_t matchingRevision[] = "revision-one";
  cache.invalidate(matchingRevision, sizeof(matchingRevision) - 1);
  assert(!cache.available());
}

void frameDispatcherStagesPortraitAndRejectsBadPayloads() {
  std::vector<uint16_t> firstSlot(PortraitCache::kByteCount / 2);
  std::vector<uint16_t> secondSlot(PortraitCache::kByteCount / 2);
  std::vector<uint8_t> portrait(PortraitCache::kByteCount);
  for (size_t index = 0; index < portrait.size(); ++index)
    portrait[index] = static_cast<uint8_t>(index * 31u);

  PortraitCache cache;
  assert(cache.attach(firstSlot.data(), secondSlot.data(),
                      PortraitCache::kByteCount));
  PresentationController presentation;
  charadock::FrameDispatcher dispatcher(presentation, cache);
  const auto metadata = metadataFor(portrait, "amber:stackchan-v1");
  const auto metadataPayload =
      charadock::encodePortraitMetadataPayload(metadata);
  assert(!metadataPayload.empty());
  PortraitMetadata decoded;
  assert(charadock::decodePortraitMetadataPayload(metadataPayload, decoded));
  assert(decoded.checksum == metadata.checksum);
  assert(std::string(decoded.revision.data()) == "amber:stackchan-v1");

  auto outcome =
      dispatcher.apply(Frame{FrameType::AssetMeta, 1, metadataPayload}, 1000);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  uint16_t sequence = 2;
  for (size_t offset = 0; offset < portrait.size(); offset += 4092) {
    const size_t count = std::min<size_t>(4092, portrait.size() - offset);
    std::vector<uint8_t> payload(4 + count);
    payload[0] = static_cast<uint8_t>(offset & 0xff);
    payload[1] = static_cast<uint8_t>((offset >> 8) & 0xff);
    payload[2] = static_cast<uint8_t>((offset >> 16) & 0xff);
    payload[3] = static_cast<uint8_t>((offset >> 24) & 0xff);
    std::copy(portrait.begin() + offset, portrait.begin() + offset + count,
              payload.begin() + 4);
    outcome = dispatcher.apply(
        Frame{FrameType::AssetChunk, sequence++, std::move(payload)}, 1000);
    assert(outcome.result == charadock::FrameApplyResult::Applied);
  }
  outcome = dispatcher.apply(Frame{FrameType::AssetEnd, sequence, {}}, 1100);
  assert(outcome.result == charadock::FrameApplyResult::PortraitCompleted);
  assert(presentation.snapshot().portraitAvailable);
  assert(presentation.shouldRenderPortrait(1100));
  assert(!presentation.shouldRenderPortrait(2600));
  outcome =
      dispatcher.apply(Frame{FrameType::CharacterChanged,
                             89,
                             {'a', 'm', 'b', 'e', 'r', ':', 's', 't', 'a', 'c',
                              'k', 'c', 'h', 'a', 'n', '-', 'v', '1'}},
                       1150);
  assert(outcome.result == charadock::FrameApplyResult::PortraitCacheHit);
  assert(cache.available());
  charadock::PresentationSettings settings;
  settings.theme = {0x102030, 0xa0b0c0, 0xd09020};
  settings.motionProfile = charadock::MotionProfile::Calm;
  settings.motionIntensity = 42;
  const auto settingsPayload =
      charadock::encodePresentationSettingsPayload(settings);
  assert(settingsPayload.size() == 13);
  charadock::PresentationSettings decodedSettings;
  assert(charadock::decodePresentationSettingsPayload(settingsPayload,
                                                      decodedSettings));
  outcome = dispatcher.apply(
      Frame{FrameType::PresentationConfig, 90, settingsPayload}, 1175);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  assert(presentation.snapshot().theme.primary == 0x102030);
  assert(presentation.snapshot().motionProfile ==
         charadock::MotionProfile::Calm);
  assert(presentation.snapshot().motionIntensity == 42);

  outcome = dispatcher.apply(Frame{FrameType::MouthLevel, 91, {3}}, 1200);
  assert(outcome.result == charadock::FrameApplyResult::InvalidPayload);
  outcome = dispatcher.apply(Frame{FrameType::DisplayMode, 92, {1}}, 1200);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  assert(presentation.shouldRenderPortrait(90000));
  outcome = dispatcher.apply(Frame{FrameType::CharacterChanged,
                                   93,
                                   {'n', 'e', 'w', '-', 'r', 'e', 'v'}},
                             1200);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  assert(cache.available());
  assert(presentation.snapshot().portraitAvailable);
  assert(std::string(cache.metadata()->revision.data()) ==
         "amber:stackchan-v1");
  settings.artworkAllowed = false;
  const auto noArtworkPayload =
      charadock::encodePresentationSettingsPayload(settings);
  outcome = dispatcher.apply(
      Frame{FrameType::PresentationConfig, 94, noArtworkPayload}, 1200);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  assert(!presentation.snapshot().artworkAllowed);
  outcome = dispatcher.apply(Frame{FrameType::DisplayMode, 95, {1}}, 1200);
  assert(outcome.result == charadock::FrameApplyResult::InvalidPayload);
  outcome =
      dispatcher.apply(Frame{FrameType::AssetMeta, 96, metadataPayload}, 1200);
  assert(outcome.result == charadock::FrameApplyResult::InvalidPayload);
  auto malformedSettings = settingsPayload;
  malformedSettings[3] = 101;
  outcome = dispatcher.apply(
      Frame{FrameType::PresentationConfig, 97, malformedSettings}, 1200);
  assert(outcome.result == charadock::FrameApplyResult::InvalidPayload);
}

void frameDispatcherAppliesIdempotentAudioFrames() {
  PresentationController presentation;
  PortraitCache portraitCache;
  std::vector<int16_t> audioStorage(4096);
  charadock::PcmPlaybackBuffer audio;
  assert(audio.attach(audioStorage.data(), audioStorage.size()));
  charadock::FrameDispatcher dispatcher(presentation, portraitCache, &audio);

  charadock::AudioStreamConfig config;
  config.prebufferMs = 100;
  config.volume = 84;
  const auto configPayload = charadock::encodeAudioStreamConfigPayload(config);
  assert(configPayload.size() == 11);
  charadock::AudioStreamConfig decoded;
  assert(charadock::decodeAudioStreamConfigPayload(configPayload, decoded));
  assert(decoded.sampleRate == 16000);
  assert(decoded.prebufferMs == 100);
  assert(decoded.volume == 84);

  auto outcome =
      dispatcher.apply(Frame{FrameType::AudioBegin, 300, configPayload}, 1000);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  assert(outcome.audioResult == charadock::AudioBufferResult::Ok);
  outcome =
      dispatcher.apply(Frame{FrameType::AudioBegin, 300, configPayload}, 1001);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  assert(outcome.audioResult == charadock::AudioBufferResult::Duplicate);

  const auto samples = pcmBytes(320, -400);
  outcome = dispatcher.apply(Frame{FrameType::AudioChunk, 301, samples}, 1010);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  assert(audio.bufferedSamples() == 320);
  outcome = dispatcher.apply(Frame{FrameType::AudioChunk, 301, samples}, 1011);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  assert(outcome.audioResult == charadock::AudioBufferResult::Duplicate);
  assert(audio.bufferedSamples() == 320);

  outcome = dispatcher.apply(Frame{FrameType::AudioEnd, 302, {}}, 1020);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  assert(audio.producerEnded());
  outcome = dispatcher.apply(Frame{FrameType::AudioStop, 303, {}}, 1030);
  assert(outcome.result == charadock::FrameApplyResult::Applied);
  assert(audio.phase() == charadock::AudioPlaybackPhase::Stopped);

  auto unsupported = config;
  unsupported.sampleRate = 24000;
  outcome = dispatcher.apply(
      Frame{FrameType::AudioBegin, 304,
            charadock::encodeAudioStreamConfigPayload(unsupported)},
      1040);
  assert(outcome.result == charadock::FrameApplyResult::AudioRejected);
  assert(outcome.audioResult == charadock::AudioBufferResult::InvalidFormat);
  outcome = dispatcher.apply(Frame{FrameType::AudioBegin, 305, {1, 2}}, 1050);
  assert(outcome.result == charadock::FrameApplyResult::InvalidPayload);
}

void hybridPortraitTimingAndFallbackAreDeterministic() {
  PresentationController controller;
  controller.setPortraitAvailable(true);
  controller.requestPortrait(1000);
  assert(controller.shouldRenderPortrait(1000));
  assert(controller.shouldRenderPortrait(2499));
  controller.update(2500);
  assert(!controller.shouldRenderPortrait(2500));
  controller.setDisplayMode(charadock::DisplayMode::CharacterArt);
  assert(controller.shouldRenderPortrait(90000));
  controller.setDisplayMode(charadock::DisplayMode::NativeFace);
  assert(!controller.shouldRenderPortrait(1001));
  controller.setPortraitAvailable(false);
  controller.setDisplayMode(charadock::DisplayMode::CharacterArt);
  assert(!controller.shouldRenderPortrait(1001));
}

void mouthEnvelopeUsesPlayedPcmWithRateLimitAndReleaseHold() {
  charadock::MouthEnvelope envelope;
  std::vector<int16_t> loud(160, 3000);
  std::vector<int16_t> medium(160, 800);
  std::vector<int16_t> quiet(160, 0);

  auto update = envelope.observePlayedSamples(loud.data(), loud.size(), 0);
  assert(update.changed && update.level == 2);
  update = envelope.observePlayedSamples(medium.data(), medium.size(), 30);
  assert(!update.changed && envelope.level() == 2);
  update = envelope.observePlayedSamples(medium.data(), medium.size(), 100);
  assert(!update.changed && envelope.level() == 2);
  update = envelope.observePlayedSamples(medium.data(), medium.size(), 200);
  assert(update.changed && update.level == 1);
  update = envelope.observePlayedSamples(quiet.data(), quiet.size(), 220);
  assert(!update.changed && envelope.level() == 1);
  update = envelope.observePlayedSamples(quiet.data(), quiet.size(), 320);
  assert(update.changed && update.level == 0);
  update = envelope.observePlayedSamples(loud.data(), loud.size(), 350);
  assert(!update.changed);
  update = envelope.observePlayedSamples(loud.data(), loud.size(), 420);
  assert(update.changed && update.level == 2);
  update = envelope.close(430);
  assert(update.changed && update.level == 0);
}

std::vector<uint8_t> pcmBytes(size_t sampleCount, int16_t firstSample) {
  std::vector<uint8_t> bytes(sampleCount * 2);
  for (size_t index = 0; index < sampleCount; ++index) {
    const uint16_t sample =
        static_cast<uint16_t>(firstSample + static_cast<int16_t>(index));
    bytes[index * 2] = static_cast<uint8_t>(sample & 0xff);
    bytes[index * 2 + 1] = static_cast<uint8_t>(sample >> 8);
  }
  return bytes;
}

void audioBufferPrebuffersAndCommitsOnlyAcceptedSamples() {
  std::vector<int16_t> storage(4096);
  charadock::PcmPlaybackBuffer audio;
  assert(audio.attach(storage.data(), storage.size()));
  charadock::AudioStreamConfig config;
  config.prebufferMs = 100;
  assert(audio.begin(config, 100) == charadock::AudioBufferResult::Ok);
  assert(audio.begin(config, 100) == charadock::AudioBufferResult::Duplicate);
  auto changedConfig = config;
  changedConfig.volume = 99;
  assert(audio.begin(changedConfig, 100) ==
         charadock::AudioBufferResult::SequenceConflict);

  const auto first = pcmBytes(800, -1200);
  assert(audio.appendChunk(101, first.data(), first.size()) ==
         charadock::AudioBufferResult::Ok);
  assert(audio.appendChunk(101, first.data(), first.size()) ==
         charadock::AudioBufferResult::Duplicate);
  auto conflicting = first;
  conflicting[0] ^= 0xff;
  assert(audio.appendChunk(101, conflicting.data(), conflicting.size()) ==
         charadock::AudioBufferResult::SequenceConflict);
  int16_t output[320] = {};
  assert(audio.prepareSamples(output, 320) == 0);

  const auto second = pcmBytes(800, 2000);
  assert(audio.appendChunk(102, second.data(), second.size()) ==
         charadock::AudioBufferResult::Ok);
  assert(audio.prepareSamples(output, 320) == 320);
  assert(output[0] == -1200);
  assert(output[319] == -881);
  assert(audio.bufferedSamples() == 1600);
  assert(audio.commitSamples(320));
  assert(audio.bufferedSamples() == 1280);
  assert(!audio.commitSamples(5000));
}

void audioBufferRebuffersOnUnderrunAndDrainsShortTail() {
  std::vector<int16_t> storage(4096);
  charadock::PcmPlaybackBuffer audio;
  assert(audio.attach(storage.data(), storage.size()));
  charadock::AudioStreamConfig config;
  config.prebufferMs = 80;
  assert(audio.begin(config, 200) == charadock::AudioBufferResult::Ok);
  const auto initial = pcmBytes(1280, 10);
  assert(audio.appendChunk(201, initial.data(), initial.size()) ==
         charadock::AudioBufferResult::Ok);
  std::vector<int16_t> output(1280);
  assert(audio.prepareSamples(output.data(), output.size()) == 1280);
  assert(audio.commitSamples(1280));
  assert(audio.prepareSamples(output.data(), output.size()) == 0);
  assert(audio.phase() == charadock::AudioPlaybackPhase::Buffering);
  assert(audio.underrunCount() == 1);

  const auto tail = pcmBytes(160, -20);
  assert(audio.appendChunk(202, tail.data(), tail.size()) ==
         charadock::AudioBufferResult::Ok);
  assert(audio.prepareSamples(output.data(), output.size()) == 0);
  assert(audio.end(203) == charadock::AudioBufferResult::Ok);
  assert(audio.end(203) == charadock::AudioBufferResult::Duplicate);
  assert(audio.prepareSamples(output.data(), output.size()) == 160);
  assert(audio.commitSamples(160));
  assert(audio.prepareSamples(output.data(), output.size()) == 0);
  assert(audio.phase() == charadock::AudioPlaybackPhase::Finished);
  const uint32_t generation = audio.generation();
  audio.stop();
  assert(audio.phase() == charadock::AudioPlaybackPhase::Stopped);
  assert(audio.generation() == generation + 1);
  assert(audio.underrunCount() == 1);
  assert(audio.begin(config, 204) == charadock::AudioBufferResult::Ok);
  assert(audio.underrunCount() == 0);
}

void audioBufferRejectsUnsupportedOrOversizedInputAtomically() {
  std::vector<int16_t> storage(2048);
  charadock::PcmPlaybackBuffer audio;
  assert(audio.attach(storage.data(), storage.size()));
  charadock::AudioStreamConfig config;
  config.sampleRate = 24000;
  assert(audio.begin(config, 1) == charadock::AudioBufferResult::InvalidFormat);
  config.sampleRate = 16000;
  config.prebufferMs = 80;
  assert(audio.begin(config, 2) == charadock::AudioBufferResult::Ok);
  const auto oversized = pcmBytes(2049);
  assert(audio.appendChunk(3, oversized.data(), oversized.size()) ==
         charadock::AudioBufferResult::BufferFull);
  assert(audio.bufferedSamples() == 0);
  const uint8_t odd[] = {0, 1, 2};
  assert(audio.appendChunk(3, odd, sizeof(odd)) ==
         charadock::AudioBufferResult::InvalidChunk);
}

int main() {
  frameRoundTripsAcrossFragments();
  frameEncodingMatchesTheUsbToolKnownVector();
  invalidFrameRecoversToTheNextMagic();
  presentationBoundsMouthAndSchedulesNaturalBlink();
  artworkPolicyFallsBackToNativeFace();
  portraitCacheCommitsAtomically();
  frameDispatcherStagesPortraitAndRejectsBadPayloads();
  frameDispatcherAppliesIdempotentAudioFrames();
  hybridPortraitTimingAndFallbackAreDeterministic();
  mouthEnvelopeUsesPlayedPcmWithRateLimitAndReleaseHold();
  audioBufferPrebuffersAndCommitsOnlyAcceptedSamples();
  audioBufferRebuffersOnUnderrunAndDrainsShortTail();
  audioBufferRejectsUnsupportedOrOversizedInputAtomically();
  std::cout
      << "StackChan protocol/presentation/cache/envelope/audio tests passed\n";
  return 0;
}
