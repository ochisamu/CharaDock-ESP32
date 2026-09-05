// SPDX-License-Identifier: Apache-2.0
#include "charadock/presentation.hpp"

#include <algorithm>

namespace charadock {

PresentationController::PresentationController(uint32_t randomSeed)
    : randomState_(randomSeed ? randomSeed : 0x4348444bu) {
  scheduleBlink(0);
}

void PresentationController::setState(DeviceState state, uint32_t nowMs) {
  if (snapshot_.state == state)
    return;
  snapshot_.state = state;
  stateChangedAt_ = nowMs;
  if (state == DeviceState::Listening)
    snapshot_.expression = Expression::Listening;
  if (state == DeviceState::Thinking)
    snapshot_.expression = Expression::Thinking;
  if (state == DeviceState::Error)
    snapshot_.mouthLevel = 0;
}

void PresentationController::setExpression(Expression expression) {
  snapshot_.expression = expression;
}

void PresentationController::setDisplayMode(DisplayMode mode) {
  if (!snapshot_.artworkAllowed && mode == DisplayMode::CharacterArt)
    snapshot_.displayMode = DisplayMode::NativeFace;
  else
    snapshot_.displayMode = mode;
  if (snapshot_.displayMode == DisplayMode::NativeFace)
    portraitRequested_ = false;
}

void PresentationController::setMotionProfile(MotionProfile profile) {
  snapshot_.motionProfile = profile;
}

void PresentationController::setTheme(const Theme &theme) {
  snapshot_.theme = theme;
}

void PresentationController::setMouthLevel(uint8_t level) {
  snapshot_.mouthLevel = std::min<uint8_t>(2, level);
}

void PresentationController::setMotionIntensity(uint8_t intensity) {
  snapshot_.motionIntensity = std::min<uint8_t>(100, intensity);
}

void PresentationController::setArtworkAllowed(bool allowed) {
  snapshot_.artworkAllowed = allowed;
  if (!allowed)
    portraitRequested_ = false;
  if (!allowed && snapshot_.displayMode == DisplayMode::CharacterArt) {
    snapshot_.displayMode = DisplayMode::NativeFace;
  }
}

void PresentationController::setPortraitAvailable(bool available) {
  snapshot_.portraitAvailable = available;
  if (!available)
    portraitRequested_ = false;
}

void PresentationController::requestPortrait(uint32_t nowMs,
                                             uint32_t durationMs) {
  if (!snapshot_.artworkAllowed ||
      snapshot_.displayMode == DisplayMode::NativeFace) {
    return;
  }
  portraitRequested_ = true;
  const uint32_t boundedDuration = std::min<uint32_t>(10000, durationMs);
  portraitVisibleUntil_ = nowMs + boundedDuration;
}

void PresentationController::update(uint32_t nowMs) {
  if (portraitRequested_ && snapshot_.displayMode == DisplayMode::Hybrid &&
      static_cast<int32_t>(nowMs - portraitVisibleUntil_) >= 0) {
    portraitRequested_ = false;
  }
  const bool blinkAllowed = snapshot_.state != DeviceState::Error &&
                            snapshot_.state != DeviceState::Listening;
  if (!blinkAllowed) {
    snapshot_.eyesClosed = false;
    if (nowMs >= nextBlinkAt_)
      scheduleBlink(nowMs);
    return;
  }
  if (!snapshot_.eyesClosed && nowMs >= nextBlinkAt_) {
    snapshot_.eyesClosed = true;
    blinkEndsAt_ = nowMs + 120;
  } else if (snapshot_.eyesClosed && nowMs >= blinkEndsAt_) {
    snapshot_.eyesClosed = false;
    scheduleBlink(nowMs);
  }
}

const PresentationSnapshot &PresentationController::snapshot() const {
  return snapshot_;
}

bool PresentationController::shouldRenderPortrait(uint32_t nowMs) const {
  if (!snapshot_.artworkAllowed || !snapshot_.portraitAvailable ||
      snapshot_.displayMode == DisplayMode::NativeFace) {
    return false;
  }
  if (snapshot_.displayMode == DisplayMode::CharacterArt)
    return true;
  return portraitRequested_ &&
         static_cast<int32_t>(portraitVisibleUntil_ - nowMs) > 0;
}

uint32_t PresentationController::stateChangedAt() const {
  return stateChangedAt_;
}

uint32_t PresentationController::nextBlinkAt() const { return nextBlinkAt_; }

uint32_t PresentationController::portraitVisibleUntil() const {
  return portraitVisibleUntil_;
}

uint32_t PresentationController::nextRandom() {
  uint32_t value = randomState_;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  randomState_ = value;
  return value;
}

void PresentationController::scheduleBlink(uint32_t nowMs) {
  nextBlinkAt_ = nowMs + 4000 + (nextRandom() % 3001);
}

const char *deviceStateName(DeviceState state) {
  switch (state) {
  case DeviceState::Idle:
    return "idle";
  case DeviceState::Listening:
    return "listening";
  case DeviceState::Thinking:
    return "thinking";
  case DeviceState::Speaking:
    return "speaking";
  case DeviceState::Error:
    return "error";
  case DeviceState::Connecting:
    return "connecting";
  }
  return "connecting";
}

const char *expressionName(Expression expression) {
  switch (expression) {
  case Expression::Neutral:
    return "neutral";
  case Expression::Listening:
    return "listening";
  case Expression::Thinking:
    return "thinking";
  case Expression::Happy:
    return "happy";
  case Expression::Surprised:
    return "surprised";
  case Expression::Soft:
    return "soft";
  }
  return "neutral";
}

const char *displayModeName(DisplayMode mode) {
  switch (mode) {
  case DisplayMode::Hybrid:
    return "hybrid";
  case DisplayMode::CharacterArt:
    return "character-art";
  case DisplayMode::NativeFace:
    return "native-face";
  }
  return "native-face";
}

} // namespace charadock
