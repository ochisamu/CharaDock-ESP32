// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "charadock/audio_sink.hpp"
#include "charadock/monochrome_asset.hpp"
#include "charadock/protocol_v2.hpp"
#include "charadock/scene_model.hpp"

namespace charadock::rlcd {

enum class FrameApplyResult : uint8_t {
  Applied = 0,
  AssetCompleted = 1,
  AssetCacheHit = 2,
  Ignored = 3,
  InvalidPayload = 4,
  AssetRejected = 5,
  AudioRejected = 6,
};

struct FrameApplyOutcome {
  FrameApplyResult result = FrameApplyResult::Ignored;
  MonochromeAssetResult assetResult = MonochromeAssetResult::Ok;
  uint8_t audioResult = 0;
  bool displayChanged = false;
};

class TimeSyncSink {
public:
  virtual ~TimeSyncSink() = default;
  virtual bool setUnixTime(uint64_t unixSeconds, int16_t utcOffsetMinutes) = 0;
};

class FrameDispatcher {
public:
  FrameDispatcher(SceneModel &scene, MonochromeAssetStore &assets,
                  TimeSyncSink *timeSink = nullptr,
                  AudioSink *audioSink = nullptr,
                  MonochromeAssetStore *blinkAsset = nullptr,
                  MonochromeAssetStore *mouthHalfAsset = nullptr,
                  MonochromeAssetStore *mouthOpenAsset = nullptr);
  FrameApplyOutcome apply(const protocol::Frame &frame);

private:
  SceneModel &scene_;
  MonochromeAssetStore &assets_;
  TimeSyncSink *timeSink_ = nullptr;
  AudioSink *audioSink_ = nullptr;
  MonochromeAssetStore *blinkAsset_ = nullptr;
  MonochromeAssetStore *mouthHalfAsset_ = nullptr;
  MonochromeAssetStore *mouthOpenAsset_ = nullptr;
  MonochromeAssetStore *assetTransfer_ = nullptr;
};

const char *frameApplyResultName(FrameApplyResult result);

} // namespace charadock::rlcd
