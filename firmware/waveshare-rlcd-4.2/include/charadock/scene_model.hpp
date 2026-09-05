// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace charadock::rlcd {

enum class DeviceState : uint8_t {
  Idle = 0,
  Listening = 1,
  Thinking = 2,
  Speaking = 3,
  Error = 4,
  Connecting = 5,
  Working = 6,
  Completed = 7,
  ApprovalRequired = 8,
  Offline = 9,
};

enum class SceneId : uint8_t {
  Home = 0,
  Conversation = 1,
  Work = 2,
  Offline = 3,
  Recovery = 4,
};

enum class TextTarget : uint8_t {
  Caption = 0,
  Activity = 1,
  NextAction = 2,
  Footer = 3,
};

enum SceneFlags : uint8_t {
  SceneConnected = 1u << 0,
  SceneLive = 1u << 1,
  SceneBeatrice = 1u << 2,
  SceneApproval = 1u << 3,
};

struct SceneSnapshot {
  SceneId scene = SceneId::Recovery;
  DeviceState state = DeviceState::Connecting;
  uint8_t flags = 0;
  uint32_t revision = 0;
  uint32_t elapsedSeconds = 0;
  uint16_t artifactCount = 0;
  std::string characterName = "CharaDock";
  std::string modeLabel;
  std::string caption;
  std::string activity;
  std::string nextAction;
  std::string footer;
  uint8_t captionFont = 16;
  uint8_t activityFont = 16;
  uint8_t nextActionFont = 12;
  uint8_t footerFont = 12;
};

enum class SceneApplyResult : uint8_t {
  Ok = 0,
  InvalidPayload = 1,
  StaleRevision = 2,
  NoStagedScene = 3,
  RevisionMismatch = 4,
};

class SceneModel {
public:
  const SceneSnapshot &active() const;
  const SceneSnapshot *staged() const;
  bool dirty() const;
  void clearDirty();

  SceneApplyResult stage(SceneSnapshot snapshot);
  SceneApplyResult stageText(uint32_t revision, TextTarget target,
                             uint8_t fontSize, std::string text);
  SceneApplyResult commit(uint32_t revision);
  SceneApplyResult updateState(DeviceState state);
  void setLocalScene(SceneSnapshot snapshot);

private:
  SceneSnapshot active_{};
  SceneSnapshot staged_{};
  bool hasStaged_ = false;
  bool dirty_ = true;
};

const char *deviceStateName(DeviceState state);
const char *sceneName(SceneId scene);
const char *sceneApplyResultName(SceneApplyResult result);

} // namespace charadock::rlcd
