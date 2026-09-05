// SPDX-License-Identifier: Apache-2.0
#include "charadock/host_connection.hpp"

namespace charadock::rlcd {

HostConnectionWatchdog::HostConnectionWatchdog(uint32_t timeoutMs)
    : timeoutMs_(timeoutMs) {}

void HostConnectionWatchdog::noteActivity(uint32_t nowMs) {
  lastActivityAtMs_ = nowMs;
  hostSeen_ = true;
  offlineReported_ = false;
}

bool HostConnectionWatchdog::consumeTimeout(uint32_t nowMs) {
  if (!hostSeen_ || offlineReported_ || timeoutMs_ == 0 ||
      static_cast<uint32_t>(nowMs - lastActivityAtMs_) < timeoutMs_)
    return false;
  offlineReported_ = true;
  return true;
}

bool HostConnectionWatchdog::hostSeen() const { return hostSeen_; }

bool HostConnectionWatchdog::online() const {
  return hostSeen_ && !offlineReported_;
}

uint32_t HostConnectionWatchdog::lastActivityAtMs() const {
  return lastActivityAtMs_;
}

SceneSnapshot offlineSnapshot(const SceneSnapshot &current) {
  SceneSnapshot snapshot = current;
  snapshot.scene = SceneId::Offline;
  snapshot.state = DeviceState::Offline;
  snapshot.flags &= static_cast<uint8_t>(~SceneConnected);
  snapshot.modeLabel = "OFFLINE";
  snapshot.caption.clear();
  snapshot.activity.clear();
  snapshot.nextAction.clear();
  snapshot.footer = "CharaDockはオフラインです";
  return snapshot;
}

} // namespace charadock::rlcd
