// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include "charadock/ambient_policy.hpp"
using namespace charadock::rlcd;
int main() {
  assert(ambientPolicy(DeviceState::Listening, SceneId::Home, true, true, false, true) == AmbientPolicy::IdleTimeout);
  assert(ambientPolicy(DeviceState::Listening, SceneId::Home, true, true, false, false) == AmbientPolicy::Portrait);
  assert(ambientPolicy(DeviceState::Idle, SceneId::Home, false, false, false) == AmbientPolicy::IdleTimeout);
  assert(ambientPolicy(DeviceState::Listening, SceneId::Home, true, false, false) == AmbientPolicy::IdleTimeout);
  assert(ambientPolicy(DeviceState::Listening, SceneId::Home, true, true, false) == AmbientPolicy::Portrait);
  assert(ambientPolicy(DeviceState::Listening, SceneId::Home, false, false, false) == AmbientPolicy::Portrait);
  assert(ambientPolicy(DeviceState::Thinking, SceneId::Conversation, false, false, false) == AmbientPolicy::Waiting);
  assert(ambientPolicy(DeviceState::Thinking, SceneId::Conversation, false, false, true) == AmbientPolicy::Portrait);
  for (auto state : {DeviceState::Speaking, DeviceState::Working, DeviceState::Error, DeviceState::ApprovalRequired})
    assert(ambientPolicy(state, SceneId::Home, true, false, false) == AmbientPolicy::Portrait);
  assert(ambientPolicy(DeviceState::Idle, SceneId::Recovery, false, false, false) == AmbientPolicy::Portrait);
  assert(!ambientIdleExpired(30099, 100));
  assert(ambientIdleExpired(30100, 100));
  assert(!ambientIdleExpired(99, UINT32_MAX - 100));
  assert(ambientIdleExpired(30000, UINT32_MAX - 100));
}
