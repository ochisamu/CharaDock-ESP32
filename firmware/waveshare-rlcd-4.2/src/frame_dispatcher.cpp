// SPDX-License-Identifier: Apache-2.0
#include "charadock/frame_dispatcher.hpp"

#include "charadock/presentation_protocol.hpp"

#include <cstring>
#include <utility>

namespace charadock::rlcd {
namespace {

uint32_t readU32(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         static_cast<uint32_t>(bytes[1]) << 8 |
         static_cast<uint32_t>(bytes[2]) << 16 |
         static_cast<uint32_t>(bytes[3]) << 24;
}

uint64_t readU64(const uint8_t *bytes) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index)
    value |= static_cast<uint64_t>(bytes[index]) << (index * 8);
  return value;
}

int16_t readI16(const uint8_t *bytes) {
  const uint16_t value = static_cast<uint16_t>(bytes[0]) |
                         static_cast<uint16_t>(bytes[1]) << 8;
  return static_cast<int16_t>(value);
}

FrameApplyOutcome sceneOutcome(SceneApplyResult result,
                               bool changed = false) {
  return {result == SceneApplyResult::Ok ? FrameApplyResult::Applied
                                         : FrameApplyResult::InvalidPayload,
          MonochromeAssetResult::Ok, 0, changed};
}

FrameApplyOutcome assetOutcome(MonochromeAssetResult result,
                               bool completed = false) {
  return {result == MonochromeAssetResult::Ok
              ? (completed ? FrameApplyResult::AssetCompleted
                           : FrameApplyResult::Applied)
              : FrameApplyResult::AssetRejected,
          result, 0, completed && result == MonochromeAssetResult::Ok};
}

bool safeRevision(const std::vector<uint8_t> &payload) {
  if (payload.size() > MonochromeAssetMetadata::kMaximumRevisionBytes)
    return false;
  for (const uint8_t value : payload) {
    const bool alphanumeric = (value >= 'a' && value <= 'z') ||
                              (value >= 'A' && value <= 'Z') ||
                              (value >= '0' && value <= '9');
    if (!alphanumeric && value != '-' && value != '_' && value != '.' &&
        value != ':')
      return false;
  }
  return true;
}

} // namespace

FrameDispatcher::FrameDispatcher(SceneModel &scene,
                                 MonochromeAssetStore &assets,
                                 TimeSyncSink *timeSink,
                                 AudioSink *audioSink,
                                 MonochromeAssetStore *blinkAsset,
                                 MonochromeAssetStore *mouthHalfAsset,
                                 MonochromeAssetStore *mouthOpenAsset)
    : scene_(scene), assets_(assets), timeSink_(timeSink),
      audioSink_(audioSink), blinkAsset_(blinkAsset),
      mouthHalfAsset_(mouthHalfAsset), mouthOpenAsset_(mouthOpenAsset) {}

FrameApplyOutcome FrameDispatcher::apply(const protocol::Frame &frame) {
  const auto &payload = frame.payload;
  if (payload.size() > protocol::kMaximumPayloadBytes)
    return {FrameApplyResult::InvalidPayload};

  switch (frame.type) {
  case protocol::FrameType::State:
    if (payload.size() != 1 ||
        payload[0] > static_cast<uint8_t>(DeviceState::Offline))
      return {FrameApplyResult::InvalidPayload};
    return sceneOutcome(
        scene_.updateState(static_cast<DeviceState>(payload[0])), true);

  case protocol::FrameType::DisplayScene: {
    SceneSnapshot snapshot;
    if (!decodeDisplayScenePayload(payload, snapshot))
      return {FrameApplyResult::InvalidPayload};
    return sceneOutcome(scene_.stage(std::move(snapshot)));
  }

  case protocol::FrameType::DisplayText: {
    TextUpdate update;
    if (!decodeDisplayTextPayload(payload, update))
      return {FrameApplyResult::InvalidPayload};
    return sceneOutcome(scene_.stageText(update.revision, update.target,
                                         update.fontSize,
                                         std::move(update.text)));
  }

  case protocol::FrameType::DisplayCommit: {
    uint32_t revision = 0;
    if (!decodeDisplayCommitPayload(payload, revision))
      return {FrameApplyResult::InvalidPayload};
    return sceneOutcome(scene_.commit(revision), true);
  }

  case protocol::FrameType::AssetMeta: {
    MonochromeAssetMetadata metadata;
    if (!decodeMonochromeAssetMetadata(payload, metadata))
      return {FrameApplyResult::InvalidPayload,
              MonochromeAssetResult::InvalidMetadata};
    const char *name = metadata.frameName.data();
    assetTransfer_ = std::strcmp(name, "portrait") == 0
                         ? &assets_
                     : std::strcmp(name, "portrait-blink") == 0
                         ? blinkAsset_
                     : std::strcmp(name, "portrait-mouth-half") == 0
                         ? mouthHalfAsset_
                     : std::strcmp(name, "portrait-mouth-open") == 0
                         ? mouthOpenAsset_
                         : nullptr;
    if (!assetTransfer_)
      return {FrameApplyResult::AssetRejected,
              MonochromeAssetResult::InvalidMetadata};
    const MonochromeAssetResult result = assetTransfer_->beginTransfer(metadata);
    if (result != MonochromeAssetResult::Ok)
      assetTransfer_ = nullptr;
    return assetOutcome(result);
  }

  case protocol::FrameType::AssetChunk:
    if (payload.size() <= sizeof(uint32_t) || !assetTransfer_)
      return {FrameApplyResult::InvalidPayload};
    return assetOutcome(assetTransfer_->writeChunk(
        readU32(payload.data()), payload.data() + sizeof(uint32_t),
        payload.size() - sizeof(uint32_t)));

  case protocol::FrameType::AssetEnd: {
    if (!payload.empty())
      return {FrameApplyResult::InvalidPayload};
    if (!assetTransfer_)
      return {FrameApplyResult::InvalidPayload};
    MonochromeAssetStore *target = assetTransfer_;
    assetTransfer_ = nullptr;
    return assetOutcome(target->finishTransfer(), true);
  }

  case protocol::FrameType::AssetInvalidate:
    if (!safeRevision(payload))
      return {FrameApplyResult::InvalidPayload};
    assets_.invalidate(payload.empty() ? nullptr : payload.data(),
                       payload.size());
    if (payload.empty()) {
      if (blinkAsset_) blinkAsset_->invalidate();
      if (mouthHalfAsset_) mouthHalfAsset_->invalidate();
      if (mouthOpenAsset_) mouthOpenAsset_->invalidate();
    }
    return {FrameApplyResult::Applied, MonochromeAssetResult::Ok, 0, true};

  case protocol::FrameType::CharacterChanged:
    if (payload.empty() || !safeRevision(payload))
      return {FrameApplyResult::InvalidPayload};
    if (assets_.matchesRevision(payload.data(), payload.size()))
      return {FrameApplyResult::AssetCacheHit, MonochromeAssetResult::Ok, 0,
              true};
    // Keep the last verified portrait visible until AssetEnd atomically
    // commits the replacement.  CharacterChanged is a cache probe, not an
    // invalidation request; AssetInvalidate remains the explicit way to clear
    // the active image.  This also makes interrupted USB transfers harmless.
    return {FrameApplyResult::Applied, MonochromeAssetResult::Ok, 0, true};

  case protocol::FrameType::TimeSync:
    if (payload.size() != 11 || payload[0] != 1 || !timeSink_ ||
        readI16(payload.data() + 9) < -720 ||
        readI16(payload.data() + 9) > 840 ||
        !timeSink_->setUnixTime(readU64(payload.data() + 1),
                                readI16(payload.data() + 9)))
      return {FrameApplyResult::InvalidPayload};
    return {FrameApplyResult::Applied};

  case protocol::FrameType::AudioBegin: {
    const AudioApplyResult audioResult =
        payload.size() == 8 && audioSink_
            ? audioSink_->startPlayback(readU32(payload.data()),
                                        readU32(payload.data() + 4))
            : AudioApplyResult::InvalidFormat;
    if (audioResult != AudioApplyResult::Ok)
      return {FrameApplyResult::AudioRejected, MonochromeAssetResult::Ok,
              static_cast<uint8_t>(audioResult)};
    scene_.updateState(DeviceState::Speaking);
    return {FrameApplyResult::Applied, MonochromeAssetResult::Ok, 0, true};
  }

  case protocol::FrameType::AudioChunk: {
    const AudioApplyResult audioResult =
        !payload.empty() && (payload.size() % sizeof(int16_t)) == 0 &&
                audioSink_
            ? audioSink_->writePcm16(payload.data(), payload.size())
            : AudioApplyResult::InvalidFormat;
    return {audioResult == AudioApplyResult::Ok
                ? FrameApplyResult::Applied
                : FrameApplyResult::AudioRejected,
            MonochromeAssetResult::Ok,
            static_cast<uint8_t>(audioResult)};
  }

  case protocol::FrameType::AudioEnd: {
    const AudioApplyResult audioResult =
        payload.empty() && audioSink_ ? audioSink_->finishPlayback()
                                      : AudioApplyResult::InvalidFormat;
    return {audioResult == AudioApplyResult::Ok
                ? FrameApplyResult::Applied
                : FrameApplyResult::AudioRejected,
            MonochromeAssetResult::Ok,
            static_cast<uint8_t>(audioResult)};
  }

  case protocol::FrameType::AudioStop: {
    const AudioApplyResult audioResult =
        payload.empty() && audioSink_ ? audioSink_->stopPlayback()
                                      : AudioApplyResult::InvalidFormat;
    if (audioResult == AudioApplyResult::Ok)
      scene_.updateState(DeviceState::Idle);
    return {audioResult == AudioApplyResult::Ok
                ? FrameApplyResult::Applied
                : FrameApplyResult::AudioRejected,
            MonochromeAssetResult::Ok,
            static_cast<uint8_t>(audioResult),
            audioResult == AudioApplyResult::Ok};
  }

  default:
    return {FrameApplyResult::Ignored};
  }
}

const char *audioApplyResultName(AudioApplyResult result) {
  switch (result) {
  case AudioApplyResult::Ok:
    return "ok";
  case AudioApplyResult::Unavailable:
    return "unavailable";
  case AudioApplyResult::InvalidFormat:
    return "invalid-format";
  case AudioApplyResult::InvalidState:
    return "invalid-state";
  case AudioApplyResult::BufferFull:
    return "buffer-full";
  case AudioApplyResult::CodecFailure:
    return "codec-failure";
  }
  return "unknown";
}

const char *frameApplyResultName(FrameApplyResult result) {
  switch (result) {
  case FrameApplyResult::Applied:
    return "applied";
  case FrameApplyResult::AssetCompleted:
    return "asset-completed";
  case FrameApplyResult::AssetCacheHit:
    return "asset-cache-hit";
  case FrameApplyResult::Ignored:
    return "ignored";
  case FrameApplyResult::InvalidPayload:
    return "invalid-payload";
  case FrameApplyResult::AssetRejected:
    return "asset-rejected";
  case FrameApplyResult::AudioRejected:
    return "audio-rejected";
  }
  return "unknown";
}

} // namespace charadock::rlcd
