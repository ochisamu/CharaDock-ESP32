// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "charadock/scene_model.hpp"

namespace charadock::rlcd {

// Tracks host liveness using wrap-safe Arduino millis() arithmetic.  The
// timeout is consumed once so the display is not needlessly redrawn on every
// loop after a host disappears.
class HostConnectionWatchdog {
public:
  explicit HostConnectionWatchdog(uint32_t timeoutMs);

  void noteActivity(uint32_t nowMs);
  bool consumeTimeout(uint32_t nowMs);
  bool hostSeen() const;
  bool online() const;
  uint32_t lastActivityAtMs() const;

private:
  uint32_t timeoutMs_ = 0;
  uint32_t lastActivityAtMs_ = 0;
  bool hostSeen_ = false;
  bool offlineReported_ = false;
};

// Preserves the last character and portrait while removing transient host
// state from the scene shown after the watchdog expires.
SceneSnapshot offlineSnapshot(const SceneSnapshot &current);

} // namespace charadock::rlcd
