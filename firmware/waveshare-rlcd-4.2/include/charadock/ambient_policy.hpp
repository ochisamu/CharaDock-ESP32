// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "charadock/scene_model.hpp"

namespace charadock::rlcd {
enum class AmbientPolicy { Portrait, IdleTimeout, Waiting };
// Display-only decision: never start/stop capture or disconnect a session here.
inline AmbientPolicy ambientPolicy(DeviceState state, SceneId scene,
                                   bool monitoring, bool recording,
                                   bool playing, bool pendingVoice = false) {
  if ((recording && !pendingVoice) || playing || scene == SceneId::Offline ||
      scene == SceneId::Recovery)
    return AmbientPolicy::Portrait;
  if (state == DeviceState::Thinking)
    return AmbientPolicy::Waiting;
  if (state == DeviceState::Idle ||
      (state == DeviceState::Listening && monitoring))
    return AmbientPolicy::IdleTimeout;
  return AmbientPolicy::Portrait;
}
inline bool ambientIdleExpired(uint32_t now, uint32_t lastActivity) {
  return static_cast<uint32_t>(now - lastActivity) >= 30000u;
}
} // namespace charadock::rlcd
