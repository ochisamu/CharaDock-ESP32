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
#include "charadock/idle_backlight.hpp"
#include "charadock/hands_free_voice.hpp"
#include "charadock/hardware.hpp"

#include "charadock/mouth_envelope.hpp"
#include "charadock/head_gesture.hpp"
#include "charadock/portrait_cache.hpp"
#include "charadock/presentation.hpp"
#include "charadock/quick_settings.hpp"
#include "charadock/face_tracking.hpp"
#include "charadock/protocol_v2.hpp"

static_assert(charadock::HardwareController::voiceLedColor(false, false) == 0);
static_assert(charadock::HardwareController::voiceLedColor(true, false) == 0x0b4474);
static_assert(charadock::HardwareController::voiceLedColor(false, true) == 0x4d2870);

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

void usbStreamRecoversATruncatedAudioFrameOnRetry() {
  const auto wire = charadock::protocol::encodeFrame(Frame{FrameType::AudioChunk, 23, pcmBytes(1024)});
  Decoder decoder;
  assert(decoder.push(wire.data(), wire.size() - 128).empty());
  const auto frames = decoder.push(wire.data(), wire.size());
  assert(frames.size() == 1 && frames[0].type == FrameType::AudioChunk && frames[0].sequence == 23);
  assert(frames[0].payload == pcmBytes(1024));
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

  assert(dispatcher.apply({FrameType::AudioStop, 303, {1}}, 1031).result ==
         charadock::FrameApplyResult::Applied);
  assert(dispatcher.apply({FrameType::AudioStop, 303, {2}}, 1032).result ==
         charadock::FrameApplyResult::InvalidPayload);

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

  const auto refill = pcmBytes(1280, -20);
  assert(audio.appendChunk(202, refill.data(), refill.size()) ==
         charadock::AudioBufferResult::Ok);
  assert(audio.prebufferSamples() == storage.size());
  assert(audio.prepareSamples(output.data(), output.size()) == 0);
  const auto tail = pcmBytes(160, -20);
  assert(audio.appendChunk(203, tail.data(), tail.size()) ==
         charadock::AudioBufferResult::Ok);
  assert(audio.prepareSamples(output.data(), output.size()) == 0);
  assert(audio.end(204) == charadock::AudioBufferResult::Ok);
  assert(audio.end(204) == charadock::AudioBufferResult::Duplicate);
  assert(audio.prepareSamples(output.data(), output.size()) == 1280);
  assert(audio.commitSamples(1280));
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

void completeAudioWaitsForVerifiedEnd() {
  using namespace charadock;
  std::vector<int16_t> storage(4096);
  PcmPlaybackBuffer audio;
  assert(audio.attach(storage.data(), storage.size()));
  AudioStreamConfig config;
  config.prebufferMs = 80;
  config.expectedSamples = 3400;
  const auto payload = encodeAudioStreamConfigPayload(config);
  assert(payload.size() == 15);
  AudioStreamConfig decoded;
  assert(decodeAudioStreamConfigPayload(payload, decoded));
  assert(decoded.expectedSamples == 3400);
  auto missingLength = payload;
  missingLength[11] = missingLength[12] = 0;
  assert(!decodeAudioStreamConfigPayload(missingLength, decoded));

  auto oversized = config;
  oversized.expectedSamples = storage.size() + 1;
  assert(audio.begin(oversized, 1) == AudioBufferResult::InvalidFormat);
  assert(audio.begin(config, 1) == AudioBufferResult::Ok);
  assert(audio.begin(oversized, 1) == AudioBufferResult::InvalidFormat);
  const auto part = pcmBytes(1700, -1500);
  std::vector<int16_t> out(config.expectedSamples);
  assert(audio.appendChunk(2, part.data(), part.size()) == AudioBufferResult::Ok);
  assert(audio.prepareSamples(out.data(), out.size()) == 0);
  assert(audio.end(3) == AudioBufferResult::InvalidChunk);
  assert(audio.appendChunk(4, part.data(), part.size()) == AudioBufferResult::Ok);
  assert(audio.appendChunk(4, part.data(), part.size()) == AudioBufferResult::Duplicate);
  assert(audio.appendChunk(5, part.data(), part.size()) == AudioBufferResult::InvalidChunk);
  assert(audio.prepareSamples(out.data(), out.size()) == 0);
  assert(audio.end(6) == AudioBufferResult::Ok);
  assert(audio.prepareSamples(out.data(), out.size()) == config.expectedSamples);
  assert(out[0] == -1500 && out[1699] == 199 && out[1700] == -1500);
  assert(audio.commitSamples(out.size()));
  assert(audio.prepareSamples(out.data(), out.size()) == 0);
  assert(audio.phase() == AudioPlaybackPhase::Finished);
  assert(audio.underrunCount() == 0);
}

void imaAdpcmFramesDecodeToOrderedPcm() {
  using namespace charadock;
  using protocol::FrameType;
  std::vector<int16_t> storage(4096);
  PcmPlaybackBuffer audio;
  assert(audio.attach(storage.data(), storage.size()));
  PresentationController presentation;
  PortraitCache portrait;
  FrameDispatcher dispatcher(presentation, portrait, &audio);
  AudioStreamConfig config;
  config.imaAdpcm = true;
  config.expectedSamples = 4;
  assert(dispatcher.apply({FrameType::AudioBegin, 1, encodeAudioStreamConfigPayload(config)}, 0).audioResult == AudioBufferResult::Ok);
  const std::vector<uint8_t> encoded = {4, 0, 100, 0, 0, 0xb7, 0x04};
  auto malformed = encoded;
  malformed[4] = 89;
  assert(dispatcher.apply({FrameType::AudioChunk, 2, malformed}, 0).audioResult == AudioBufferResult::InvalidChunk);
  assert(audio.bufferedSamples() == 0);
  assert(dispatcher.apply({FrameType::AudioChunk, 3, encoded}, 0).audioResult == AudioBufferResult::Ok);
  assert(dispatcher.apply({FrameType::AudioChunk, 3, encoded}, 0).audioResult == AudioBufferResult::Duplicate);
  assert(dispatcher.apply({FrameType::AudioEnd, 4, {}}, 0).audioResult == AudioBufferResult::Ok);
  int16_t output[4]{};
  assert(audio.prepareSamples(output, 4) == 4);
  assert(output[0] == 100 && output[1] == 111 && output[2] == 97 && output[3] == 112);

  constexpr size_t largeSamples = FrameDispatcher::kMaxAdpcmPcmBytes / sizeof(int16_t);
  storage.resize(largeSamples);
  assert(audio.attach(storage.data(), storage.size()));
  config.expectedSamples = largeSamples;
  assert(dispatcher.apply({FrameType::AudioBegin, 5, encodeAudioStreamConfigPayload(config)}, 0).audioResult == AudioBufferResult::Ok);
  std::vector<uint8_t> large(5 + largeSamples / 2);
  large[0] = (largeSamples + 1) & 0xff;
  large[1] = (largeSamples + 1) >> 8;
  assert(dispatcher.apply({FrameType::AudioChunk, 6, large}, 0).audioResult == AudioBufferResult::InvalidChunk);
  large[0] = largeSamples & 0xff;
  large[1] = largeSamples >> 8;
  assert(dispatcher.apply({FrameType::AudioChunk, 7, large}, 0).audioResult == AudioBufferResult::Ok);
  assert(dispatcher.apply({FrameType::AudioEnd, 8, {}}, 0).audioResult == AudioBufferResult::Ok);
  assert(audio.bufferedSamples() == largeSamples);
}

void listensBeforeShowingOneAcknowledgement() {
  using namespace charadock;
  PresentationController presentation;
  presentation.setState(DeviceState::Listening, 0);
  std::array<int16_t, 320> voice{};
  voice.fill(900);
  std::array<int16_t, 320> silence{};
  presentation.observeMicrophone(voice.data(), voice.size(), 20);
  for (uint32_t at = 40; at <= 220; at += 20)
    presentation.observeMicrophone(silence.data(), silence.size(), at);
  presentation.update(300);
  assert(!presentation.snapshot().listeningToVoice && presentation.snapshot().faceY == 0);
  for (uint32_t at = 320; at <= 780; at += 20)
    presentation.observeMicrophone(voice.data(), voice.size(), at);
  assert(presentation.snapshot().listeningToVoice);
  for (uint32_t at = 800; at <= 960; at += 20)
    presentation.observeMicrophone(silence.data(), silence.size(), at);
  presentation.update(1095);
  assert(presentation.snapshot().faceY == 6);
  for (uint32_t at = 1100; at <= 1560; at += 20)
    presentation.observeMicrophone(voice.data(), voice.size(), at);
  for (uint32_t at = 1580; at <= 1740; at += 20)
    presentation.observeMicrophone(silence.data(), silence.size(), at);
  presentation.update(1800);
  assert(presentation.snapshot().faceY < 6);
  presentation.setState(DeviceState::Thinking, 1830);
  presentation.observeMicrophone(voice.data(), voice.size(), 1840);
  assert(!presentation.snapshot().listeningToVoice);
}

void speakingPerformanceTracksActualLevelAndCancelsCleanly() {
  using namespace charadock;
  PresentationController presentation;
  for (uint32_t at = 40; at <= 160; at += 40) {
    const auto cues = presentation.observeSpeech(640, at);
    assert(!cues.emphasized && cues.moveHead == (at == 160));
  }
  presentation.update(160);
  const auto quiet = presentation.snapshot().mouthAperture;
  assert(quiet > 0);
  const auto accent = presentation.observeSpeech(2500, 200);
  assert(accent.emphasized && !accent.moveHead);
  presentation.setFacePose(-40, 17);
  presentation.update(200);
  assert(presentation.snapshot().mouthAperture > quiet);
  assert(presentation.snapshot().speakingWithEmphasis && presentation.snapshot().faceX < 0);
  const auto nearby = presentation.observeSpeech(3300, 240);
  assert(!nearby.emphasized && !nearby.moveHead);
  for (uint32_t at = 280; at <= 2440; at += 40)
    assert(!presentation.observeSpeech(850, at).moveHead);
  for (uint32_t at = 2480; at <= 2680; at += 40)
    assert(!presentation.observeSpeech(120, at).moveHead);
  assert(presentation.observeSpeech(850, 2720).moveHead);
  presentation.closeSpeech();
  presentation.setFacePose(0, 0);
  presentation.update(400);
  presentation.update(440);
  presentation.update(480);
  assert(!presentation.snapshot().speakingWithEmphasis && presentation.snapshot().mouthAperture == 0);
  assert(presentation.snapshot().faceX == 0);
}

static void testHeadGestures() {
  using namespace charadock;
  HeadGesturePlayer player;
  HeadPose pose{};
  assert(!player.next(0, true, 100, pose));
  assert(player.request(HeadGesture::Nod, 100));
  assert(!player.next(200, false, 100, pose));
  assert(player.next(300, true, 70, pose));
  assert(pose.yaw == 0 && pose.pitch == 47);
  assert(!player.request(HeadGesture::Ponder, 320));
  assert(!player.next(500, true, 100, pose));
  assert(player.next(950, true, 100, pose) && pose.yaw == 0 && pose.pitch == 0);
  assert(!player.next(1370, true, 100, pose));
  assert(player.request(HeadGesture::Ponder, 1390));
  assert(!player.next(2190, true, 100, pose)); // Expired without speech.
  assert(player.request(HeadGesture::Waiting, 2300));
  assert(player.next(2300, false, 100, pose) && pose.pitch == 44);
  assert(player.cancel());
  assert(!player.next(2600, true, 100, pose));
  assert(player.request(HeadGesture::Delight, 2700));
  assert(!player.next(2750, true, 0, pose));
  assert(player.request(HeadGesture::Accent, 2900));
  assert(player.next(2900, true, 100, pose) && pose.pitch == 75);
  assert(player.request(HeadGesture::Ponder, 2920));
  assert(!player.next(3000, false, 100, pose));
  assert(player.next(3720, false, 100, pose) && pose.pitch == 0);
}

void explicitVoiceMotionCanMoveWhileSilentAndReturnsToCenter() {
  using namespace charadock;
  HeadGesturePlayer player;
  HeadPose pose{};
  assert(player.request(HeadGesture::Sweep, 100));
  assert(player.voiceMotionActive());
  assert(!player.next(99, false, 100, pose));
  assert(player.next(100, false, 70, pose) && pose.yaw == 175);
  assert(!player.next(1000, false, 70, pose));
  assert(player.next(1600, false, 70, pose) && pose.yaw == -175);
  assert(player.next(3100, false, 70, pose) && pose.yaw == 0);
  assert(player.voiceMotionActive());
  assert(player.request(HeadGesture::VoiceNod, 3200));
  assert(player.next(3200, false, 70, pose) && pose.pitch > 0);
  const HeadPose expected[] = {{0, 210, 350}, {0, -210, 335}, {0, -180, 390}, {-165, 65, 350},
                              {-220, 210, 350}, {220, 210, 350}, {-220, -210, 335}, {220, -210, 335}};
  for (uint8_t id = 13; id <= kLastVoiceMotion; ++id) {
    HeadGesturePlayer next;
    assert(next.request(static_cast<HeadGesture>(id), 100));
    assert(next.next(100, false, 100, pose));
    const auto &target = expected[id - 13];
    assert(pose.yaw == target.yaw && pose.pitch == target.pitch && pose.speed == target.speed);
    assert(next.next(1800, false, 100, pose) && pose.yaw == 0 && pose.pitch == 0);
  }
}

void quickSettingsProtectsTheDefaultFaceAndPrivacyControl() {
  using charadock::QuickAction;
  charadock::QuickSettingsTouch touch;
  const auto gesture = [&](int x, int y, int endX, int endY, bool open,
                           bool preview = false, bool asleep = false) {
    assert(touch.update(true, x, y, open, preview, asleep) == QuickAction::None);
    assert(touch.update(true, endX, endY, open, preview, false) == QuickAction::None);
    return touch.update(false, 0, 0, open, preview, false);
  };
  assert(gesture(180, 220, 171, 152, false) == QuickAction::Open);
  assert(gesture(180, 220, 171, 152, false, false, true) == QuickAction::None);
  assert(gesture(180, 170, 171, 102, false) == QuickAction::None);
  assert(gesture(170, 231, 255, 110, false) == QuickAction::None);
  assert(gesture(155, 78, 155, 78, false) == QuickAction::None);
  assert(gesture(155, 78, 155, 78, true) == QuickAction::Quieter);
  assert(gesture(279, 78, 279, 78, true) == QuickAction::Louder);
  assert(gesture(90, 130, 90, 130, true) == QuickAction::Tracking);
  assert(gesture(90, 200, 90, 200, true) == QuickAction::Preview);
  assert(gesture(310, 17, 310, 17, true) == QuickAction::None);
  assert(gesture(310, 17, 310, 17, false, true) == QuickAction::None);
  assert(gesture(90, 140, 90, 140, false, true) == QuickAction::PreviewBack);
  assert(gesture(160, 100, 170, 190, true) == QuickAction::Close);
  assert(gesture(47, 24, 47, 24, true) == QuickAction::Close);
}

void faceTrackingMovesOnlyOnFreshOffCenterDetections() {
  charadock::FaceTracking tracking;
  tracking.update(500, 500, 17, -13, true);
  assert(tracking.yaw() == 0 && tracking.pitch() == 0);
  tracking.update(900, 900, 140, -95, true);
  assert(tracking.yaw() == -13 && tracking.pitch() == 9);
  tracking.update(1100, 900, 140, -95, true);
  assert(tracking.yaw() == -13 && tracking.pitch() == 9);
  for (uint32_t at = 1300; at <= 6500; at += 400)
    tracking.update(at, at, 140, -95, true);
  assert(tracking.yaw() == -120 && tracking.pitch() == 50);
  tracking.update(8299, 0, 0, 0, true);
  assert(tracking.yaw() == -120 && tracking.pitch() == 50);
  tracking.update(8300, 0, 0, 0, true);
  assert(tracking.yaw() == 0 && tracking.pitch() == 0);
  tracking.update(8500, 8500, -120, 0, true);
  assert(tracking.yaw() == 14);
  tracking.update(8600, 8500, -120, 0, false);
  assert(tracking.yaw() == 0 && tracking.pitch() == 0);
}

void idleDisplayDimsSleepsAndWakesWithoutASecondTap() {
  using charadock::IdleBacklight;
  IdleBacklight backlight;
  backlight.noteActivity(100);
  assert(backlight.update(30099, false) == IdleBacklight::kActiveBrightness);
  assert(backlight.update(30100, false) == 104);
  assert(backlight.update(30220, false) == 80);
  assert(backlight.update(30460, false) == IdleBacklight::kDimBrightness);
  assert(backlight.update(300099, false) == IdleBacklight::kDimBrightness);
  assert(backlight.update(300100, false) == 0);
  assert(backlight.update(101, false) == 0); // Still off after timer rollover.
  assert(backlight.update(300101, true) == IdleBacklight::kActiveBrightness);
  assert(backlight.update(720000, true) == IdleBacklight::kActiveBrightness);
  assert(backlight.update(749999, false) == IdleBacklight::kActiveBrightness);

  backlight.noteActivity(0xfffffff0);
  assert(backlight.update(15, false) == IdleBacklight::kActiveBrightness);
  assert(backlight.update(31000, false) == IdleBacklight::kDimBrightness);
  backlight.noteActivity(32000);
  assert(backlight.update(32000, false) == IdleBacklight::kActiveBrightness);
}

void handsFreeRearmKeepsItsNoiseFloor() {
  charadock::HandsFreeVoice vad;
  for (int i = 0; i < 20; ++i) assert(!vad.observe(100, 120, false));
  assert(vad.calibration() == 0 && vad.floor() == 100);
  vad.rearm();
  for (int i = 0; i < 5; ++i) assert(!vad.observe(450, 120, false));
  assert(vad.observe(450, 120, false));
  assert(vad.floor() == 100);
}

void handsFreeIgnoresCalibrationAndShortNoiseThenEndsAtPause() {
  charadock::HandsFreeVoice vad;
  const int16_t silenceWithOffset[] = {2100, 2100, 2100, 2100};
  const int16_t voiceWithOffset[] = {1900, 2300, 1900, 2300};
  assert(vad.centeredRms(silenceWithOffset, 4) == 0);
  assert(vad.centeredRms(voiceWithOffset, 4) == 200);
  for (int index = 0; index < 5; ++index) assert(!vad.observe(0, 120, false));
  assert(!vad.observe(0, 120, false));
  for (int index = 0; index < 14; ++index) assert(!vad.observe(270, 120, false));
  assert(vad.floor() == 270);
  for (int index = 0; index < 9; ++index) assert(!vad.observe(290, 120, false));
  assert(!vad.observe(650, 120, false));
  assert(!vad.observe(650, 120, false));
  assert(!vad.observe(80, 120, false));
  for (int i = 0; i < 2; ++i) assert(!vad.observe(650, 120, false));
  assert(vad.observe(650, 120, false));
  for (int i = 0; i < 24; ++i) assert(!vad.observe(40, 120, true));
  assert(!vad.observe(650, 120, true));
  for (int i = 0; i < 24; ++i) assert(!vad.observe(40, 120, true));
  assert(vad.observe(40, 120, true));
}

void stagesOneCompletePhraseWithoutReplacingCurrentAudio() {
  using namespace charadock;
  using protocol::FrameType;
  PcmPlaybackBuffer active, staged;
  PresentationController presentation;
  PortraitCache portrait;
  FrameDispatcher dispatcher(presentation, portrait, &active, &staged);
  AudioStreamConfig config;
  config.prebufferMs = 80;
  config.sampleRate = 16000;
  config.expectedSamples = 1;
  // At least 80 ms of storage is needed even for a short complete phrase.
  std::vector<int16_t> current(1280), next(1280);
  assert(active.attach(current.data(), current.size()));
  assert(staged.attach(next.data(), next.size()));
  const auto begin = encodeAudioStreamConfigPayload(config);
  assert(dispatcher.apply({FrameType::AudioBegin, 1, begin}, 0).audioResult == AudioBufferResult::Ok);
  assert(dispatcher.apply({FrameType::AudioChunk, 2, {11, 0}}, 0).audioResult == AudioBufferResult::Ok);
  assert(dispatcher.apply({FrameType::AudioEnd, 3, {}}, 0).audioResult == AudioBufferResult::Ok);
  assert(dispatcher.apply({FrameType::AudioStageBegin, 4, begin}, 0).audioResult == AudioBufferResult::Ok);
  assert(dispatcher.apply({FrameType::AudioStageEnd, 5, {}}, 0).audioResult == AudioBufferResult::InvalidChunk);
  assert(dispatcher.apply({FrameType::AudioStageChunk, 6, {22, 0}}, 0).audioResult == AudioBufferResult::Ok);
  assert(dispatcher.apply({FrameType::AudioStageEnd, 7, {}}, 0).audioResult == AudioBufferResult::Ok);
  assert(!active.adoptStaged(staged));
  int16_t sample = 0;
  assert(active.prepareSamples(&sample, 1) == 1 && sample == 11);
  assert(active.commitSamples(1));
  assert(active.prepareSamples(&sample, 1) == 0);
  assert(active.adoptStaged(staged));
  assert(active.beginSequence() == 4);
  assert(active.prepareSamples(&sample, 1) == 1 && sample == 22);
  dispatcher.apply({FrameType::AudioStop, 8, {}}, 0);
  assert(!active.active() && !staged.active());
}

static void faceDetectionBadgeUsesCurrentModeAndFacePresence() {
  using namespace charadock;
  PresentationSnapshot snapshot;
  snapshot.state = DeviceState::Idle;
  snapshot.showFaceDetection(true, false);
  assert(snapshot.faceDetection == FaceDetectionStatus::Searching);
  snapshot.showFaceDetection(true, true);
  assert(snapshot.faceDetection == FaceDetectionStatus::Detected);
  snapshot.showFaceDetection(true, false);
  assert(snapshot.faceDetection == FaceDetectionStatus::Searching);
  snapshot.showFaceDetection(false, true);
  assert(snapshot.faceDetection == FaceDetectionStatus::Hidden);
  snapshot.privacyMuted = true;
  snapshot.showFaceDetection(true, true);
  assert(snapshot.faceDetection == FaceDetectionStatus::Hidden);
  snapshot.privacyMuted = false;
  for (auto state : {DeviceState::Listening, DeviceState::Thinking, DeviceState::Speaking,
                     DeviceState::Error, DeviceState::Connecting}) {
    snapshot.state = state;
    snapshot.showFaceDetection(true, true);
    assert(snapshot.faceDetection == FaceDetectionStatus::Hidden);
  }
}

int main() {
  faceDetectionBadgeUsesCurrentModeAndFacePresence();
  stagesOneCompletePhraseWithoutReplacingCurrentAudio();
  handsFreeRearmKeepsItsNoiseFloor();
  handsFreeIgnoresCalibrationAndShortNoiseThenEndsAtPause();
  explicitVoiceMotionCanMoveWhileSilentAndReturnsToCenter();
  quickSettingsProtectsTheDefaultFaceAndPrivacyControl();
  faceTrackingMovesOnlyOnFreshOffCenterDetections();
  idleDisplayDimsSleepsAndWakesWithoutASecondTap();
  testHeadGestures();
  listensBeforeShowingOneAcknowledgement();
  speakingPerformanceTracksActualLevelAndCancelsCleanly();
  frameRoundTripsAcrossFragments();
  frameEncodingMatchesTheUsbToolKnownVector();
  invalidFrameRecoversToTheNextMagic();
  usbStreamRecoversATruncatedAudioFrameOnRetry();
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
  completeAudioWaitsForVerifiedEnd();
  imaAdpcmFramesDecodeToOrderedPcm();
  std::cout
      << "StackChan protocol/presentation/cache/envelope/audio tests passed\n";
  return 0;
}
