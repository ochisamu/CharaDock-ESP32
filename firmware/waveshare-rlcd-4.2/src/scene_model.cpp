// SPDX-License-Identifier: Apache-2.0
#include "charadock/scene_model.hpp"

#include <utility>

namespace charadock::rlcd {

const SceneSnapshot &SceneModel::active() const { return active_; }
const SceneSnapshot *SceneModel::staged() const {
  return hasStaged_ ? &staged_ : nullptr;
}
bool SceneModel::dirty() const { return dirty_; }
void SceneModel::clearDirty() { dirty_ = false; }

SceneApplyResult SceneModel::stage(SceneSnapshot snapshot) {
  if (snapshot.revision == 0)
    return SceneApplyResult::InvalidPayload;
  if (snapshot.revision < active_.revision)
    return SceneApplyResult::StaleRevision;
  staged_ = std::move(snapshot);
  hasStaged_ = true;
  return SceneApplyResult::Ok;
}

SceneApplyResult SceneModel::stageText(uint32_t revision, TextTarget target,
                                       uint8_t fontSize, std::string text) {
  if (!hasStaged_)
    return SceneApplyResult::NoStagedScene;
  if (revision != staged_.revision)
    return SceneApplyResult::RevisionMismatch;
  if (fontSize != 12 && fontSize != 16)
    return SceneApplyResult::InvalidPayload;
  switch (target) {
  case TextTarget::Caption:
    staged_.caption = std::move(text);
    staged_.captionFont = fontSize;
    break;
  case TextTarget::Activity:
    staged_.activity = std::move(text);
    staged_.activityFont = fontSize;
    break;
  case TextTarget::NextAction:
    staged_.nextAction = std::move(text);
    staged_.nextActionFont = fontSize;
    break;
  case TextTarget::Footer:
    staged_.footer = std::move(text);
    staged_.footerFont = fontSize;
    break;
  default:
    return SceneApplyResult::InvalidPayload;
  }
  return SceneApplyResult::Ok;
}

SceneApplyResult SceneModel::commit(uint32_t revision) {
  if (!hasStaged_)
    return revision == active_.revision ? SceneApplyResult::Ok
                                        : SceneApplyResult::NoStagedScene;
  if (revision != staged_.revision)
    return SceneApplyResult::RevisionMismatch;
  active_ = std::move(staged_);
  staged_ = SceneSnapshot{};
  hasStaged_ = false;
  dirty_ = true;
  return SceneApplyResult::Ok;
}

SceneApplyResult SceneModel::updateState(DeviceState state) {
  if (static_cast<uint8_t>(state) >
      static_cast<uint8_t>(DeviceState::Offline))
    return SceneApplyResult::InvalidPayload;
  active_.state = state;
  if (hasStaged_)
    staged_.state = state;
  dirty_ = true;
  return SceneApplyResult::Ok;
}

void SceneModel::setLocalScene(SceneSnapshot snapshot) {
  active_ = std::move(snapshot);
  staged_ = SceneSnapshot{};
  hasStaged_ = false;
  dirty_ = true;
}

const char *deviceStateName(DeviceState state) {
  switch (state) {
  case DeviceState::Idle:
    return "IDLE";
  case DeviceState::Listening:
    return "LISTENING";
  case DeviceState::Thinking:
    return "THINKING";
  case DeviceState::Speaking:
    return "SPEAKING";
  case DeviceState::Error:
    return "ERROR";
  case DeviceState::Connecting:
    return "CONNECTING";
  case DeviceState::Working:
    return "WORK";
  case DeviceState::Completed:
    return "COMPLETED";
  case DeviceState::ApprovalRequired:
    return "APPROVAL";
  case DeviceState::Offline:
    return "OFFLINE";
  }
  return "UNKNOWN";
}

const char *sceneName(SceneId scene) {
  switch (scene) {
  case SceneId::Home:
    return "home";
  case SceneId::Conversation:
    return "conversation";
  case SceneId::Work:
    return "work";
  case SceneId::Offline:
    return "offline";
  case SceneId::Recovery:
    return "recovery";
  }
  return "unknown";
}

const char *sceneApplyResultName(SceneApplyResult result) {
  switch (result) {
  case SceneApplyResult::Ok:
    return "ok";
  case SceneApplyResult::InvalidPayload:
    return "invalid-payload";
  case SceneApplyResult::StaleRevision:
    return "stale-revision";
  case SceneApplyResult::NoStagedScene:
    return "no-staged-scene";
  case SceneApplyResult::RevisionMismatch:
    return "revision-mismatch";
  }
  return "unknown";
}

} // namespace charadock::rlcd
