// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

#include "charadock/audio_playback.hpp"
#include "charadock/portrait_cache.hpp"
#include "charadock/presentation.hpp"
#include "charadock/protocol_v2.hpp"

namespace charadock {

enum class FrameApplyResult : uint8_t {
  Applied = 0,
  PortraitCompleted = 1,
  PortraitCacheHit = 2,
  Ignored = 3,
  InvalidPayload = 4,
  PortraitRejected = 5,
  AudioRejected = 6,
};

struct FrameApplyOutcome {
  FrameApplyResult result = FrameApplyResult::Ignored;
  PortraitCacheResult portraitResult = PortraitCacheResult::Ok;
  AudioBufferResult audioResult = AudioBufferResult::Ok;
};

struct PresentationSettings {
  Theme theme{};
  MotionProfile motionProfile = MotionProfile::Energetic;
  uint8_t motionIntensity = 70;
  bool artworkAllowed = true;
};

class FrameDispatcher {
public:
  FrameDispatcher(PresentationController &presentation,
                  PortraitCache &portraitCache,
                  PcmPlaybackBuffer *audioPlayback = nullptr);

  FrameApplyOutcome apply(const protocol::Frame &frame, uint32_t nowMs);

private:
  PresentationController &presentation_;
  PortraitCache &portraitCache_;
  PcmPlaybackBuffer *audioPlayback_;
};

// Binary payload used by AssetMeta. This helper is shared with host-side tests
// and will also be usable by the future PC protocol-v2 sender.
std::vector<uint8_t>
encodePortraitMetadataPayload(const PortraitMetadata &metadata);
bool decodePortraitMetadataPayload(const std::vector<uint8_t> &payload,
                                   PortraitMetadata &metadata);
std::vector<uint8_t>
encodePresentationSettingsPayload(const PresentationSettings &settings);
bool decodePresentationSettingsPayload(const std::vector<uint8_t> &payload,
                                       PresentationSettings &settings);
std::vector<uint8_t>
encodeAudioStreamConfigPayload(const AudioStreamConfig &config);
bool decodeAudioStreamConfigPayload(const std::vector<uint8_t> &payload,
                                    AudioStreamConfig &config);

const char *frameApplyResultName(FrameApplyResult result);

} // namespace charadock
