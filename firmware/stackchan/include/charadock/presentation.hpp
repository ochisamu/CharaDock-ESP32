// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace charadock {

enum class DeviceState : uint8_t {
  Idle = 0,
  Listening = 1,
  Thinking = 2,
  Speaking = 3,
  Error = 4,
  Connecting = 5,
};

enum class Expression : uint8_t {
  Neutral = 0,
  Listening = 1,
  Thinking = 2,
  Happy = 3,
  Surprised = 4,
  Soft = 5,
};

enum class DisplayMode : uint8_t {
  Hybrid = 0,
  CharacterArt = 1,
  NativeFace = 2,
};

enum class MotionProfile : uint8_t {
  Energetic = 0,
  Calm = 1,
  Curious = 2,
  Reserved = 3,
  Custom = 4,
};

enum class FaceDetectionStatus : uint8_t { Hidden, Searching, Detected };

struct Theme {
  uint32_t primary = 0x101619;
  uint32_t secondary = 0xfff4e4;
  uint32_t accent = 0xf0a086;
};

struct PresentationSnapshot {
  DeviceState state = DeviceState::Connecting;
  Expression expression = Expression::Neutral;
  DisplayMode displayMode = DisplayMode::Hybrid;
  MotionProfile motionProfile = MotionProfile::Energetic;
  Theme theme{};
  uint8_t mouthLevel = 0;
  uint8_t mouthAperture = 0;
  bool listeningToVoice = false;
  bool speakingWithEmphasis = false;
  int8_t faceX = 0;
  int8_t attentionX = 0;
  int8_t faceY = 0;
  uint8_t motionIntensity = 70;
  bool eyesClosed = false;
  bool artworkAllowed = true;
  bool privacyMuted = false;
  const char *connection = "";
  bool portraitAvailable = false;
  FaceDetectionStatus faceDetection = FaceDetectionStatus::Hidden;

  void showFaceDetection(bool cameraConversationAvailable, bool recentFace) {
    if (!cameraConversationAvailable || privacyMuted || state != DeviceState::Idle) {
      faceDetection = FaceDetectionStatus::Hidden;
      return;
    }
    faceDetection = recentFace ? FaceDetectionStatus::Detected : FaceDetectionStatus::Searching;
  }
};

struct SpeechCues {
  bool emphasized = false;
  bool moveHead = false;
};

class PresentationController {
public:
  explicit PresentationController(uint32_t randomSeed = 0x4348444bu);

  void setState(DeviceState state, uint32_t nowMs);
  void setExpression(Expression expression);
  void setDisplayMode(DisplayMode mode);
  void setMotionProfile(MotionProfile profile);
  void setTheme(const Theme &theme);
  void setMouthLevel(uint8_t level);
  void observeMicrophone(const int16_t *samples, size_t count, uint32_t nowMs);
  SpeechCues observeSpeech(uint16_t rms, uint32_t nowMs);
  void closeSpeech();
  void setFacePose(int yaw, int pitch);
  void setMotionIntensity(uint8_t intensity);
  void setArtworkAllowed(bool allowed);
  void setPortraitAvailable(bool available);
  void requestPortrait(uint32_t nowMs, uint32_t durationMs = 1500);
  void update(uint32_t nowMs);

  const PresentationSnapshot &snapshot() const;
  bool shouldRenderPortrait(uint32_t nowMs) const;
  uint32_t stateChangedAt() const;
  uint32_t nextBlinkAt() const;
  uint32_t portraitVisibleUntil() const;

private:
  uint32_t nextRandom();
  void scheduleBlink(uint32_t nowMs);

  PresentationSnapshot snapshot_{};
  uint16_t micFloor_ = 64;
  uint8_t voicedMicBlocks_ = 0;
  uint8_t quietMicBlocks_ = 0;
  bool acknowledgedPause_ = false;
  uint32_t listeningNodAt_ = 0;
  uint16_t speechBaseline_ = 0;
  uint8_t observedSpeechBlocks_ = 0;
  uint32_t lastVoicedSpeechAt_ = 0;
  uint32_t lastSpeechMotionAt_ = 0;
  bool hasSpeechMotion_ = false;
  uint8_t targetAperture_ = 0;
  uint32_t accentAt_ = 0;
  bool hasAccent_ = false;
  int8_t poseX_ = 0;
  int8_t poseY_ = 0;
  uint32_t lastMotionFrameAt_ = 0;
  uint32_t randomState_;
  uint32_t stateChangedAt_ = 0;
  uint32_t nextBlinkAt_ = 0;
  uint32_t blinkEndsAt_ = 0;
  uint32_t portraitVisibleUntil_ = 0;
  bool portraitRequested_ = false;
};

const char *deviceStateName(DeviceState state);
const char *expressionName(Expression expression);
const char *displayModeName(DisplayMode mode);

} // namespace charadock
