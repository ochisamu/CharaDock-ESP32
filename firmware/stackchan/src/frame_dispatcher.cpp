// SPDX-License-Identifier: Apache-2.0
#include "charadock/frame_dispatcher.hpp"

#include <cstring>

namespace charadock {
namespace {

constexpr size_t kAssetMetadataHeaderBytes = 15;
constexpr size_t kPresentationSettingsBytes = 13;
constexpr uint8_t kPresentationSettingsVersion = 1;
constexpr size_t kAudioStreamConfigBytes = 11;
constexpr uint8_t kAudioStreamConfigVersion = 1;

uint16_t readU16(const uint8_t *bytes) {
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8;
}

uint32_t readU32(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         static_cast<uint32_t>(bytes[1]) << 8 |
         static_cast<uint32_t>(bytes[2]) << 16 |
         static_cast<uint32_t>(bytes[3]) << 24;
}

void writeU16(uint8_t *bytes, uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t *bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  bytes[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  bytes[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

uint32_t readRgb(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) << 16 |
         static_cast<uint32_t>(bytes[1]) << 8 | static_cast<uint32_t>(bytes[2]);
}

void writeRgb(uint8_t *bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>((value >> 16) & 0xff);
  bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  bytes[2] = static_cast<uint8_t>(value & 0xff);
}

size_t boundedLength(const char *value, size_t capacity) {
  size_t length = 0;
  while (length < capacity && value[length] != '\0')
    ++length;
  return length;
}

bool isSafeIdentifier(const uint8_t *bytes, size_t length, bool frameName) {
  if (!bytes)
    return length == 0;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value = bytes[index];
    const bool alphanumeric = (value >= 'a' && value <= 'z') ||
                              (value >= 'A' && value <= 'Z') ||
                              (value >= '0' && value <= '9');
    const bool punctuation = value == '-' || value == '_' || value == '.' ||
                             (!frameName && value == ':');
    if (!alphanumeric && !punctuation)
      return false;
  }
  return true;
}

FrameApplyOutcome portraitOutcome(PortraitCacheResult result,
                                  bool completed = false) {
  return {result == PortraitCacheResult::Ok
              ? (completed ? FrameApplyResult::PortraitCompleted
                           : FrameApplyResult::Applied)
              : FrameApplyResult::PortraitRejected,
          result};
}

FrameApplyOutcome audioOutcome(AudioBufferResult result) {
  const bool accepted =
      result == AudioBufferResult::Ok || result == AudioBufferResult::Duplicate;
  return {accepted ? FrameApplyResult::Applied
                   : FrameApplyResult::AudioRejected,
          PortraitCacheResult::Ok, result};
}

} // namespace

FrameDispatcher::FrameDispatcher(PresentationController &presentation,
                                 PortraitCache &portraitCache,
                                 PcmPlaybackBuffer *audioPlayback)
    : presentation_(presentation), portraitCache_(portraitCache),
      audioPlayback_(audioPlayback) {}

FrameApplyOutcome FrameDispatcher::apply(const protocol::Frame &frame,
                                         uint32_t nowMs) {
  const auto &payload = frame.payload;
  if (payload.size() > protocol::kMaximumPayloadBytes)
    return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
  switch (frame.type) {
  case protocol::FrameType::State:
    if (payload.size() != 1 ||
        payload[0] > static_cast<uint8_t>(DeviceState::Connecting))
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    presentation_.setState(static_cast<DeviceState>(payload[0]), nowMs);
    return {FrameApplyResult::Applied, PortraitCacheResult::Ok};
  case protocol::FrameType::AudioBegin: {
    AudioStreamConfig config;
    if (!decodeAudioStreamConfigPayload(payload, config))
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    return audioOutcome(audioPlayback_
                            ? audioPlayback_->begin(config, frame.sequence)
                            : AudioBufferResult::StorageUnavailable);
  }
  case protocol::FrameType::AudioChunk:
    return audioOutcome(
        audioPlayback_ ? audioPlayback_->appendChunk(
                             frame.sequence, payload.data(), payload.size())
                       : AudioBufferResult::StorageUnavailable);
  case protocol::FrameType::AudioEnd:
    if (!payload.empty())
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    return audioOutcome(audioPlayback_ ? audioPlayback_->end(frame.sequence)
                                       : AudioBufferResult::StorageUnavailable);
  case protocol::FrameType::AudioStop:
    if (!payload.empty())
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    if (!audioPlayback_)
      return audioOutcome(AudioBufferResult::StorageUnavailable);
    audioPlayback_->stop();
    return audioOutcome(AudioBufferResult::Ok);
  case protocol::FrameType::Expression:
    if (payload.size() != 1 ||
        payload[0] > static_cast<uint8_t>(Expression::Soft))
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    presentation_.setExpression(static_cast<Expression>(payload[0]));
    return {FrameApplyResult::Applied, PortraitCacheResult::Ok};
  case protocol::FrameType::PresentationConfig: {
    PresentationSettings settings;
    if (!decodePresentationSettingsPayload(payload, settings))
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    presentation_.setTheme(settings.theme);
    presentation_.setMotionProfile(settings.motionProfile);
    presentation_.setMotionIntensity(settings.motionIntensity);
    presentation_.setArtworkAllowed(settings.artworkAllowed);
    if (!settings.artworkAllowed)
      portraitCache_.invalidate();
    presentation_.setPortraitAvailable(settings.artworkAllowed &&
                                       portraitCache_.available());
    if (settings.artworkAllowed && portraitCache_.available())
      presentation_.requestPortrait(nowMs);
    return {FrameApplyResult::Applied, PortraitCacheResult::Ok};
  }
  case protocol::FrameType::MouthLevel:
    if (payload.size() != 1 || payload[0] > 2)
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    presentation_.setMouthLevel(payload[0]);
    return {FrameApplyResult::Applied, PortraitCacheResult::Ok};
  case protocol::FrameType::DisplayMode:
    if (payload.size() != 1 ||
        payload[0] > static_cast<uint8_t>(DisplayMode::NativeFace))
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    if (payload[0] == static_cast<uint8_t>(DisplayMode::CharacterArt) &&
        !presentation_.snapshot().artworkAllowed)
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    presentation_.setDisplayMode(static_cast<DisplayMode>(payload[0]));
    if (payload[0] != static_cast<uint8_t>(DisplayMode::NativeFace))
      presentation_.requestPortrait(nowMs);
    return {FrameApplyResult::Applied, PortraitCacheResult::Ok};
  case protocol::FrameType::CharacterChanged:
    if (payload.size() > PortraitMetadata::kMaximumRevisionBytes ||
        !isSafeIdentifier(payload.data(), payload.size(), false))
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    if (!payload.empty() &&
        portraitCache_.matchesRevision(payload.data(), payload.size())) {
      presentation_.setPortraitAvailable(true);
      presentation_.requestPortrait(nowMs);
      return {FrameApplyResult::PortraitCacheHit, PortraitCacheResult::Ok};
    }
    // CharacterChanged probes the verified cache.  Keep the current portrait
    // visible while the inactive PSRAM slot receives its replacement; an
    // interrupted or corrupt transfer must not blank the display.
    presentation_.setPortraitAvailable(portraitCache_.available());
    presentation_.requestPortrait(nowMs);
    return {FrameApplyResult::Applied, PortraitCacheResult::Ok};
  case protocol::FrameType::AssetMeta: {
    if (!presentation_.snapshot().artworkAllowed)
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    PortraitMetadata metadata;
    if (!decodePortraitMetadataPayload(payload, metadata))
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    return portraitOutcome(portraitCache_.beginTransfer(metadata));
  }
  case protocol::FrameType::AssetChunk:
    if (payload.size() < sizeof(uint32_t))
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    return portraitOutcome(portraitCache_.writeChunk(
        readU32(payload.data()), payload.data() + sizeof(uint32_t),
        payload.size() - sizeof(uint32_t)));
  case protocol::FrameType::AssetEnd: {
    if (!payload.empty())
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    const PortraitCacheResult result = portraitCache_.finishTransfer();
    if (result == PortraitCacheResult::Ok) {
      presentation_.setPortraitAvailable(true);
      presentation_.requestPortrait(nowMs);
    }
    return portraitOutcome(result, true);
  }
  case protocol::FrameType::AssetInvalidate:
    if (payload.size() > PortraitMetadata::kMaximumRevisionBytes ||
        !isSafeIdentifier(payload.data(), payload.size(), false))
      return {FrameApplyResult::InvalidPayload, PortraitCacheResult::Ok};
    portraitCache_.invalidate(payload.empty() ? nullptr : payload.data(),
                              payload.size());
    presentation_.setPortraitAvailable(portraitCache_.available());
    return {FrameApplyResult::Applied, PortraitCacheResult::Ok};
  default:
    return {FrameApplyResult::Ignored, PortraitCacheResult::Ok};
  }
}

std::vector<uint8_t>
encodePortraitMetadataPayload(const PortraitMetadata &metadata) {
  const size_t revisionLength = boundedLength(
      metadata.revision.data(), PortraitMetadata::kMaximumRevisionBytes + 1);
  const size_t frameNameLength = boundedLength(
      metadata.frameName.data(), PortraitMetadata::kMaximumFrameNameBytes + 1);
  if (revisionLength == 0 ||
      revisionLength > PortraitMetadata::kMaximumRevisionBytes ||
      frameNameLength > PortraitMetadata::kMaximumFrameNameBytes ||
      !isSafeIdentifier(
          reinterpret_cast<const uint8_t *>(metadata.revision.data()),
          revisionLength, false) ||
      !isSafeIdentifier(
          reinterpret_cast<const uint8_t *>(metadata.frameName.data()),
          frameNameLength, true)) {
    return {};
  }
  std::vector<uint8_t> payload(kAssetMetadataHeaderBytes + revisionLength +
                               frameNameLength);
  payload[0] = static_cast<uint8_t>(metadata.format);
  writeU16(payload.data() + 1, metadata.width);
  writeU16(payload.data() + 3, metadata.height);
  writeU32(payload.data() + 5, metadata.byteCount);
  writeU32(payload.data() + 9, metadata.checksum);
  payload[13] = static_cast<uint8_t>(revisionLength);
  payload[14] = static_cast<uint8_t>(frameNameLength);
  std::memcpy(payload.data() + kAssetMetadataHeaderBytes,
              metadata.revision.data(), revisionLength);
  std::memcpy(payload.data() + kAssetMetadataHeaderBytes + revisionLength,
              metadata.frameName.data(), frameNameLength);
  return payload;
}

bool decodePortraitMetadataPayload(const std::vector<uint8_t> &payload,
                                   PortraitMetadata &metadata) {
  if (payload.size() < kAssetMetadataHeaderBytes)
    return false;
  const size_t revisionLength = payload[13];
  const size_t frameNameLength = payload[14];
  if (revisionLength == 0 ||
      revisionLength > PortraitMetadata::kMaximumRevisionBytes ||
      frameNameLength > PortraitMetadata::kMaximumFrameNameBytes ||
      payload.size() !=
          kAssetMetadataHeaderBytes + revisionLength + frameNameLength) {
    return false;
  }
  const uint8_t *revision = payload.data() + kAssetMetadataHeaderBytes;
  const uint8_t *frameName = revision + revisionLength;
  if (!isSafeIdentifier(revision, revisionLength, false) ||
      !isSafeIdentifier(frameName, frameNameLength, true)) {
    return false;
  }
  metadata = PortraitMetadata{};
  metadata.format = static_cast<PortraitFormat>(payload[0]);
  metadata.width = readU16(payload.data() + 1);
  metadata.height = readU16(payload.data() + 3);
  metadata.byteCount = readU32(payload.data() + 5);
  metadata.checksum = readU32(payload.data() + 9);
  std::memcpy(metadata.revision.data(), revision, revisionLength);
  std::memcpy(metadata.frameName.data(), frameName, frameNameLength);
  return true;
}

std::vector<uint8_t>
encodePresentationSettingsPayload(const PresentationSettings &settings) {
  if (settings.motionProfile > MotionProfile::Custom ||
      settings.motionIntensity > 100)
    return {};
  std::vector<uint8_t> payload(kPresentationSettingsBytes, 0);
  payload[0] = kPresentationSettingsVersion;
  payload[1] = settings.artworkAllowed ? 0x01 : 0x00;
  payload[2] = static_cast<uint8_t>(settings.motionProfile);
  payload[3] = settings.motionIntensity;
  writeRgb(payload.data() + 4, settings.theme.primary);
  writeRgb(payload.data() + 7, settings.theme.secondary);
  writeRgb(payload.data() + 10, settings.theme.accent);
  return payload;
}

bool decodePresentationSettingsPayload(const std::vector<uint8_t> &payload,
                                       PresentationSettings &settings) {
  if (payload.size() != kPresentationSettingsBytes ||
      payload[0] != kPresentationSettingsVersion || (payload[1] & ~0x01u) ||
      payload[2] > static_cast<uint8_t>(MotionProfile::Custom) ||
      payload[3] > 100) {
    return false;
  }
  settings = PresentationSettings{};
  settings.artworkAllowed = (payload[1] & 0x01u) != 0;
  settings.motionProfile = static_cast<MotionProfile>(payload[2]);
  settings.motionIntensity = payload[3];
  settings.theme.primary = readRgb(payload.data() + 4);
  settings.theme.secondary = readRgb(payload.data() + 7);
  settings.theme.accent = readRgb(payload.data() + 10);
  return true;
}

std::vector<uint8_t>
encodeAudioStreamConfigPayload(const AudioStreamConfig &config) {
  std::vector<uint8_t> payload(kAudioStreamConfigBytes, 0);
  payload[0] = kAudioStreamConfigVersion;
  writeU32(payload.data() + 1, config.sampleRate);
  payload[5] = config.channels;
  payload[6] = config.bitsPerSample;
  payload[7] = config.volume;
  writeU16(payload.data() + 8, config.prebufferMs);
  payload[10] = (config.signedSamples ? 0x01 : 0x00) |
                (config.littleEndian ? 0x02 : 0x00);
  return payload;
}

bool decodeAudioStreamConfigPayload(const std::vector<uint8_t> &payload,
                                    AudioStreamConfig &config) {
  if (payload.size() != kAudioStreamConfigBytes ||
      payload[0] != kAudioStreamConfigVersion || (payload[10] & ~0x03u)) {
    return false;
  }
  config = AudioStreamConfig{};
  config.sampleRate = readU32(payload.data() + 1);
  config.channels = payload[5];
  config.bitsPerSample = payload[6];
  config.volume = payload[7];
  config.prebufferMs = readU16(payload.data() + 8);
  config.signedSamples = (payload[10] & 0x01u) != 0;
  config.littleEndian = (payload[10] & 0x02u) != 0;
  return true;
}

const char *frameApplyResultName(FrameApplyResult result) {
  switch (result) {
  case FrameApplyResult::Applied:
    return "applied";
  case FrameApplyResult::PortraitCompleted:
    return "portrait-completed";
  case FrameApplyResult::PortraitCacheHit:
    return "portrait-cache-hit";
  case FrameApplyResult::Ignored:
    return "ignored";
  case FrameApplyResult::InvalidPayload:
    return "invalid-payload";
  case FrameApplyResult::PortraitRejected:
    return "portrait-rejected";
  case FrameApplyResult::AudioRejected:
    return "audio-rejected";
  }
  return "unknown";
}

} // namespace charadock
