// SPDX-License-Identifier: Apache-2.0
#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include <esp_heap_caps.h>
#include <esp_system.h>

#include "charadock/audio_input.hpp"
#include "charadock/ambient_policy.hpp"
#include "charadock/audio_output.hpp"
#include "charadock/board.hpp"
#include "charadock/board_sensors.hpp"
#include "charadock/frame_dispatcher.hpp"
#include "charadock/host_connection.hpp"
#include "charadock/input.hpp"
#include "charadock/monochrome_asset.hpp"
#include "charadock/protocol_v2.hpp"
#include "charadock/scene_model.hpp"
#include "charadock/scene_renderer.hpp"
#include "charadock/shinonome_font.hpp"
#include "charadock/st7305_display.hpp"
#include "charadock/usb_transport.hpp"
#include "charadock/wifi_connection.hpp"

using namespace charadock::rlcd;

namespace {

enum class Link : uint8_t { Usb, Wifi };
enum class CaptureMode : uint8_t { PushToTalk = 0, HandsFree = 1, Disabled = 2 };

constexpr size_t kMicrophoneChunkBytes = 1024;
// 1024 bytes of PCM16 mono is 32 ms at 16 kHz.  A shorter timeout can consume
// partial DMA data without ever returning one complete VAD chunk.
constexpr uint32_t kMicrophoneReadTimeoutMs = 40;
// Keep most of the long-press decision window so speech that begins with the
// physical press is not clipped when PTT becomes active 420 ms later.
constexpr size_t kVadPrerollBytes = 12288;
constexpr uint8_t kVadSpeechStartChunks = 2;
constexpr uint8_t kVadSilenceEndChunks = 25;
constexpr uint8_t kVadCalibrationChunks = 5;
constexpr uint16_t kVadDefaultThreshold = 120;
constexpr uint16_t kVadMinimumThreshold = 80;
constexpr uint16_t kVadMaximumThreshold = 800;
constexpr uint16_t kVadMinimumContinueRms = 60;
constexpr uint16_t kVadMaximumAdaptiveRms = 4000;
constexpr uint32_t kVadStatusIntervalMs = 1000;
constexpr uint32_t kHandsFreeResumeDelayMs = 350;
constexpr uint32_t kHandsFreeMaximumUtteranceMs = 30000;
constexpr uint32_t kPortraitAnimationIntervalMs = 250;
constexpr uint32_t kBlinkDurationMs = 160;
constexpr uint32_t kDoubleBlinkGapMs = 120;
// Reflective LCDs retain rapidly shifted full frames. Keep idle motion to one
// short, integer-pixel pose every several seconds, then return to the exact
// neutral position. Horizontal motion is always present; vertical motion is
// optional, which creates diagonal variation without continuous animation.
constexpr uint32_t kIdleMotionMinimumDelayMs = 7000;
constexpr uint32_t kIdleMotionDelayJitterMs = 7000;
constexpr uint32_t kIdleMotionMinimumHoldMs = 650;
constexpr uint32_t kIdleMotionHoldJitterMs = 550;

St7305Display display(pins::kDisplayClock, pins::kDisplayMosi,
                      pins::kDisplayDc, pins::kDisplayCs,
                      pins::kDisplayReset);
SceneRenderer renderer;
BoardSensors sensors;
AudioOutput audio;
AudioInput microphone;
SceneModel scene;
MonochromeAssetStore portrait;
MonochromeAssetStore portraitBlink;
MonochromeAssetStore portraitMouthHalf;
MonochromeAssetStore portraitMouthOpen;
FrameDispatcher dispatcher(scene, portrait, &sensors, &audio,
                           &portraitBlink, &portraitMouthHalf,
                           &portraitMouthOpen);
UsbTransport usbTransport(Serial, "usb");
WifiConnection wifi;
DebouncedButton keyButton(ButtonId::Key, 420);
DebouncedButton bootButton(ButtonId::Boot, 3000);
HostConnectionWatchdog usbWatchdog(24000);
HostConnectionWatchdog wifiWatchdog(24000);

uint8_t *portraitSlots[8] = {};
char deviceId[40] = {};
uint16_t outgoingSequence = 1;
uint32_t nextSensorReportAtMs = 0;
bool renderRequested = true;
bool usbHostConnected = false;
bool lastWifiNetworkConnected = false;
Link activeLink = Link::Usb;

CaptureMode captureMode = CaptureMode::PushToTalk;
Link captureLink = Link::Usb;
bool captureMonitoring = false;
bool recording = false;
bool pendingVoiceCandidate = false;
bool forcedHandsFreeRecording = false;
uint8_t microphoneBuffer[kMicrophoneChunkBytes] = {};
uint8_t vadPreroll[kVadPrerollBytes] = {};
size_t vadPrerollLength = 0;
uint8_t vadSpeechChunks = 0;
uint8_t vadSilenceChunks = 0;
uint8_t vadCalibrationRemaining = 0;
uint32_t vadCalibrationMinimum = UINT32_MAX;
uint32_t vadNoiseFloor = 60;
uint16_t vadMinimumStart = kVadDefaultThreshold;
uint32_t handsFreeResumeAtMs = 0;
uint32_t utteranceStartedAtMs = 0;
uint32_t lastVadStatusAtMs = 0;
uint8_t portraitFrame = 0;
uint32_t nextPortraitFrameAtMs = 0;
uint32_t blinkUntilMs = 0;
uint32_t nextBlinkAtMs = 0;
uint32_t secondBlinkAtMs = 0;
uint32_t animationUnderrunBaseline = 0;
bool portraitAnimationSuppressed = false;
bool ambientMode = false;
bool ambientAtKeyPress = false;
uint32_t lastMeaningfulActivityAtMs = 0;
int8_t portraitOffsetX = 0;
int8_t portraitOffsetY = 0;
uint32_t nextIdleMotionAtMs = 0;
uint32_t idleMotionReturnAtMs = 0;

uint16_t nextSequence() {
  const uint16_t sequence = outgoingSequence++;
  if (outgoingSequence == 0)
    outgoingSequence = 1;
  return sequence;
}

UsbTransport &transportFor(Link link) {
  return link == Link::Wifi ? wifi.transport() : usbTransport;
}

bool linkConnected(Link link) {
  return link == Link::Wifi
             ? wifi.hostAuthenticated() && wifiWatchdog.online()
             : usbHostConnected && usbWatchdog.online() && bool(Serial);
}

bool anyHostConnected() {
  return linkConnected(Link::Wifi) || linkConnected(Link::Usb);
}

void stopCapture(bool sendEnd);
void scheduleHandsFreeResume();
void requestRender();

void selectBestLink() {
  const Link next = linkConnected(Link::Usb) ? Link::Usb
      : linkConnected(Link::Wifi) ? Link::Wifi : activeLink;
  if (next == activeLink) return;
  // Never move an in-flight utterance or playback to a different transport.
  const Link previous = activeLink;
  stopCapture(false);
  audio.stopPlayback();
  if (linkConnected(previous))
    transportFor(previous).send(charadock::protocol::FrameType::Interrupt,
                                nextSequence(), nullptr, 0);
  activeLink = next;
  scene.updateState(DeviceState::Idle);
  scheduleHandsFreeResume();
  requestRender();
}

bool sendTo(Link link, charadock::protocol::FrameType type,
            const uint8_t *payload = nullptr, size_t length = 0) {
  return transportFor(link).send(type, nextSequence(), payload, length);
}

bool sendActive(charadock::protocol::FrameType type,
                const uint8_t *payload = nullptr, size_t length = 0) {
  selectBestLink();
  if (!linkConnected(activeLink))
    return false;
  return sendTo(activeLink, type, payload, length);
}

void requestRender() { renderRequested = true; }

void noteMeaningfulActivity(uint32_t now = millis()) {
  lastMeaningfulActivityAtMs = now;
  if (ambientMode) {
    ambientMode = false;
    requestRender();
  }
}

SceneSnapshot recoveryScene(const char *message = "PCを探しています…") {
  SceneSnapshot snapshot;
  snapshot.scene = SceneId::Recovery;
  snapshot.state = DeviceState::Connecting;
  snapshot.characterName = "CharaDock";
  snapshot.modeLabel = "SETUP";
  snapshot.activity = message;
  snapshot.footer = "USB / Wi-Fi / Device Protocol v2";
  return snapshot;
}

void renderIfNeeded() {
  if (!renderRequested || !display.healthy() || !renderer.fontsReady())
    return;
  const MonochromeAssetStore *activePortrait = &portrait;
  if (portraitFrame == 1 && portraitBlink.available())
    activePortrait = &portraitBlink;
  else if (portraitFrame == 2 && portraitMouthHalf.available())
    activePortrait = &portraitMouthHalf;
  else if (portraitFrame == 3 && portraitMouthOpen.available())
    activePortrait = &portraitMouthOpen;
  renderer.compose(display.canvas(), scene.active(), sensors.snapshot(),
                   *activePortrait, keyButton.pressed(), ambientMode,
                   portraitOffsetX, portraitOffsetY);
  const uint32_t flushMicros = display.flush();
  scene.clearDirty();
  renderRequested = false;
  Serial.printf("# display compose=%luus flush=%luus scene=%s rev=%lu\r\n",
                static_cast<unsigned long>(renderer.lastComposeMicros()),
                static_cast<unsigned long>(flushMicros),
                sceneName(scene.active().scene),
                static_cast<unsigned long>(scene.active().revision));
}

void scheduleNextIdleMotion(uint32_t now) {
  nextIdleMotionAtMs = now + kIdleMotionMinimumDelayMs +
                       (esp_random() % kIdleMotionDelayJitterMs);
}

void updateIdlePortraitMotion(uint32_t now) {
  const auto &active = scene.active();
  const bool eligible = !ambientMode && !captureMonitoring && !recording &&
      !audio.active() && portrait.available() &&
      active.scene == SceneId::Home && active.state == DeviceState::Idle;

  if (!eligible) {
    nextIdleMotionAtMs = 0;
    idleMotionReturnAtMs = 0;
    if (portraitOffsetX || portraitOffsetY) {
      portraitOffsetX = 0;
      portraitOffsetY = 0;
      requestRender();
    }
    return;
  }

  if (!nextIdleMotionAtMs)
    scheduleNextIdleMotion(now);

  if (idleMotionReturnAtMs &&
      static_cast<int32_t>(now - idleMotionReturnAtMs) >= 0) {
    portraitOffsetX = 0;
    portraitOffsetY = 0;
    idleMotionReturnAtMs = 0;
    scheduleNextIdleMotion(now);
    requestRender();
    return;
  }

  if (!idleMotionReturnAtMs && portraitFrame == 0 &&
      static_cast<int32_t>(now - nextIdleMotionAtMs) >= 0) {
    // X is deliberately never zero so the sequence is not vertical-only.
    // Two pixels is visible on a 4.2-inch panel without changing composition.
    const uint32_t random = esp_random();
    const int8_t horizontalMagnitude = (random & 1u) ? 1 : 2;
    portraitOffsetX = (random & 2u) ? horizontalMagnitude
                                    : -horizontalMagnitude;
    portraitOffsetY = static_cast<int8_t>((random >> 2) % 3) - 1;
    idleMotionReturnAtMs = now + kIdleMotionMinimumHoldMs +
                           ((random >> 8) % kIdleMotionHoldJitterMs);
    requestRender();
  }
}

void updatePortraitAnimation(uint32_t now) {
  const bool playbackActive = audio.active();
  if (!nextBlinkAtMs)
    nextBlinkAtMs = now + 3000 + (esp_random() % 3500);

  if (playbackActive) {
    const uint32_t underruns = audio.underruns();
    if (underruns > animationUnderrunBaseline)
      portraitAnimationSuppressed = true;
  } else {
    animationUnderrunBaseline = audio.underruns();
    portraitAnimationSuppressed = false;
  }

  if (!captureMonitoring && !playbackActive && !portraitAnimationSuppressed &&
      secondBlinkAtMs &&
      static_cast<int32_t>(now - secondBlinkAtMs) >= 0 &&
      portraitBlink.available()) {
    blinkUntilMs = now + kBlinkDurationMs;
    secondBlinkAtMs = 0;
  } else if (!captureMonitoring && !playbackActive &&
      !portraitAnimationSuppressed &&
      static_cast<int32_t>(now - nextBlinkAtMs) >= 0 &&
      portraitBlink.available()) {
    blinkUntilMs = now + kBlinkDurationMs;
    if ((esp_random() % 5) == 0)
      secondBlinkAtMs = blinkUntilMs + kDoubleBlinkGapMs;
    nextBlinkAtMs = now + 3200 + (esp_random() % 4200);
  }

  uint8_t desired = 0;
  if (!captureMonitoring && !playbackActive && !portraitAnimationSuppressed &&
      blinkUntilMs && static_cast<int32_t>(blinkUntilMs - now) > 0) {
    desired = 1;
  } else if (playbackActive && !portraitAnimationSuppressed &&
             static_cast<int32_t>(now - nextPortraitFrameAtMs) >= 0) {
    const uint8_t level = audio.mouthLevel();
    desired = level >= 2 && portraitMouthOpen.available()
                  ? 3
              : level >= 1 && portraitMouthHalf.available()
                  ? 2
                  : 0;
    nextPortraitFrameAtMs = now + kPortraitAnimationIntervalMs;
  } else if (playbackActive) {
    desired = portraitFrame;
  }

  if (desired != portraitFrame) {
    portraitFrame = desired;
    requestRender();
  }
}

void resetVoiceDetection() {
  vadPrerollLength = 0;
  vadSpeechChunks = 0;
  vadSilenceChunks = 0;
  vadCalibrationRemaining = kVadCalibrationChunks;
  vadCalibrationMinimum = UINT32_MAX;
  utteranceStartedAtMs = 0;
}

void scheduleHandsFreeResume() {
  if (captureMode == CaptureMode::HandsFree && anyHostConnected())
    handsFreeResumeAtMs = millis() + kHandsFreeResumeDelayMs;
}

void stopCapture(bool sendEnd) {
  pendingVoiceCandidate = false;
  const Link destination = captureLink;
  const bool wasRecording = recording;
  recording = false;
  captureMonitoring = false;
  forcedHandsFreeRecording = false;
  microphone.stop();
  if (sendEnd && wasRecording && linkConnected(destination))
    sendTo(destination, charadock::protocol::FrameType::PttEnd);
  resetVoiceDetection();
}

bool startCapture(bool beginRecording, bool forced = false) {
  if (captureMode == CaptureMode::Disabled || !anyHostConnected())
    return false;
  if (audio.active())
    audio.stopPlayback();
  selectBestLink();
  if (!microphone.start()) {
    scene.updateState(DeviceState::Error);
    requestRender();
    return false;
  }
  captureLink = activeLink;
  captureMonitoring = true;
  recording = beginRecording;
  forcedHandsFreeRecording = forced;
  resetVoiceDetection();
  if (recording) {
    utteranceStartedAtMs = millis();
    sendTo(captureLink, charadock::protocol::FrameType::PttStart);
  }
  scene.updateState(DeviceState::Listening);
  requestRender();
  return true;
}

void appendVadPreroll(const uint8_t *bytes, size_t length) {
  if (!bytes || !length)
    return;
  if (length >= sizeof(vadPreroll)) {
    std::memcpy(vadPreroll, bytes + length - sizeof(vadPreroll),
                sizeof(vadPreroll));
    vadPrerollLength = sizeof(vadPreroll);
    return;
  }
  if (vadPrerollLength + length > sizeof(vadPreroll)) {
    const size_t discard = vadPrerollLength + length - sizeof(vadPreroll);
    std::memmove(vadPreroll, vadPreroll + discard,
                 vadPrerollLength - discard);
    vadPrerollLength -= discard;
  }
  std::memcpy(vadPreroll + vadPrerollLength, bytes, length);
  vadPrerollLength += length;
}

uint32_t microphoneRms(const uint8_t *bytes, size_t length) {
  const size_t samples = length / sizeof(int16_t);
  if (!bytes || !samples)
    return 0;
  int64_t sum = 0;
  uint64_t squares = 0;
  for (size_t index = 0; index < samples; ++index) {
    const size_t offset = index * 2;
    const int16_t value = static_cast<int16_t>(
        bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
    sum += value;
    squares += static_cast<int64_t>(value) * value;
  }
  const int64_t mean = sum / static_cast<int64_t>(samples);
  const uint64_t meanSquare = squares / samples;
  const uint64_t dcSquare = static_cast<uint64_t>(mean * mean);
  return static_cast<uint32_t>(std::sqrt(
      static_cast<double>(meanSquare > dcSquare ? meanSquare - dcSquare : 0)));
}

uint32_t voiceStartThreshold() {
  const uint32_t margin = std::max<uint32_t>(60, vadNoiseFloor * 3 / 4);
  return std::min<uint32_t>(
      kVadMaximumAdaptiveRms,
      std::max<uint32_t>(vadMinimumStart, vadNoiseFloor + margin));
}

uint32_t voiceContinueThreshold() {
  const uint32_t margin = std::max<uint32_t>(35, vadNoiseFloor / 3);
  const uint32_t configured =
      std::max<uint32_t>(kVadMinimumContinueRms, vadMinimumStart * 3 / 4);
  return std::min<uint32_t>(
      kVadMaximumAdaptiveRms,
      std::max<uint32_t>(configured, vadNoiseFloor + margin));
}

void writeU16(uint8_t *output, size_t offset, uint32_t value) {
  const uint16_t bounded =
      static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, value));
  output[offset] = static_cast<uint8_t>(bounded & 0xff);
  output[offset + 1] = static_cast<uint8_t>(bounded >> 8);
}

void sendCaptureStatus(uint32_t rms) {
  const uint32_t now = millis();
  if (!captureMonitoring || now - lastVadStatusAtMs < kVadStatusIntervalMs)
    return;
  lastVadStatusAtMs = now;
  uint8_t payload[12] = {
      static_cast<uint8_t>((captureMonitoring ? 1 : 0) |
                           (recording ? 2 : 0)),
      0, 0, 0, 0, 0, 0, 0, 0,
      vadSpeechChunks,
      vadSilenceChunks,
      vadCalibrationRemaining,
  };
  writeU16(payload, 1, rms);
  writeU16(payload, 3, vadNoiseFloor);
  writeU16(payload, 5, voiceStartThreshold());
  writeU16(payload, 7, voiceContinueThreshold());
  sendTo(captureLink, charadock::protocol::FrameType::CaptureStatus,
         payload, sizeof(payload));
}

void beginDetectedUtterance() {
  if (!captureMonitoring || recording)
    return;
  recording = true;
  pendingVoiceCandidate = captureMode == CaptureMode::HandsFree && !forcedHandsFreeRecording;
  if (!pendingVoiceCandidate) noteMeaningfulActivity();
  utteranceStartedAtMs = millis();
  vadSilenceChunks = 0;
  sendTo(captureLink, charadock::protocol::FrameType::PttStart);
  for (size_t offset = 0; offset < vadPrerollLength;
       offset += kMicrophoneChunkBytes) {
    const size_t length =
        std::min(kMicrophoneChunkBytes, vadPrerollLength - offset);
    sendTo(captureLink, charadock::protocol::FrameType::PcmChunk,
           vadPreroll + offset, length);
  }
  vadPrerollLength = 0;
}

void streamMicrophone() {
  if (!captureMonitoring)
    return;
  if (!linkConnected(captureLink)) {
    stopCapture(false);
    return;
  }
  size_t length = 0;
  if (!microphone.readPcm16(microphoneBuffer, sizeof(microphoneBuffer),
                           length, kMicrophoneReadTimeoutMs)) {
    stopCapture(recording);
    scene.updateState(DeviceState::Error);
    requestRender();
    return;
  }
  if (!length)
    return;

  const uint32_t rms = microphoneRms(microphoneBuffer, length);
  if (captureMode == CaptureMode::PushToTalk || forcedHandsFreeRecording) {
    if (recording)
      sendTo(captureLink, charadock::protocol::FrameType::PcmChunk,
             microphoneBuffer, length);
    else if (captureMode == CaptureMode::PushToTalk)
      appendVadPreroll(microphoneBuffer, length);
    sendCaptureStatus(rms);
    return;
  }

  if (vadCalibrationRemaining) {
    vadCalibrationMinimum = std::min(vadCalibrationMinimum, rms);
    --vadCalibrationRemaining;
    if (!vadCalibrationRemaining && vadCalibrationMinimum != UINT32_MAX)
      vadNoiseFloor = std::max<uint32_t>(40, vadCalibrationMinimum);
  } else if (!recording) {
    vadNoiseFloor = (vadNoiseFloor * 31 + std::min(rms, voiceStartThreshold())) /
                    32;
  }

  bool startedFromVad = false;
  if (!recording) {
    appendVadPreroll(microphoneBuffer, length);
    if (!vadCalibrationRemaining && rms >= voiceStartThreshold())
      ++vadSpeechChunks;
    else
      vadSpeechChunks = 0;
    if (vadSpeechChunks >= kVadSpeechStartChunks) {
      beginDetectedUtterance();
      // beginDetectedUtterance() already flushed the current chunk as the
      // tail of the preroll. Do not append it a second time below.
      startedFromVad = recording;
    }
  }
  if (recording) {
    if (!startedFromVad)
      sendTo(captureLink, charadock::protocol::FrameType::PcmChunk,
             microphoneBuffer, length);
    vadSilenceChunks = rms < voiceContinueThreshold()
                           ? static_cast<uint8_t>(vadSilenceChunks + 1)
                           : 0;
    if (vadSilenceChunks >= kVadSilenceEndChunks ||
        millis() - utteranceStartedAtMs >= kHandsFreeMaximumUtteranceMs) {
      const bool unconfirmed = pendingVoiceCandidate;
      stopCapture(true);
      scene.updateState(unconfirmed ? DeviceState::Idle : DeviceState::Thinking);
      if (unconfirmed) scheduleHandsFreeResume();
      requestRender();
    }
  }
  sendCaptureStatus(rms);
}

void ensureHandsFreeMonitoring(uint32_t now) {
  if (captureMode != CaptureMode::HandsFree || captureMonitoring ||
      recording || audio.active() || !anyHostConnected())
    return;
  const DeviceState state = scene.active().state;
  if (state != DeviceState::Idle && state != DeviceState::Listening)
    return;
  if (handsFreeResumeAtMs &&
      static_cast<int32_t>(now - handsFreeResumeAtMs) < 0)
    return;
  handsFreeResumeAtMs = 0;
  startCapture(false);
}

void handleKeyEvent(const ButtonEvent &event) {
  DeviceState currentState = scene.active().state;
  if (event.action == ButtonAction::Pressed) {
    ambientAtKeyPress = ambientMode;
    // Keep the ambient dashboard visually stable until the press resolves to
    // either its short-toggle action or a long PTT action.
    if (!ambientAtKeyPress)
      noteMeaningfulActivity();
    transportFor(activeLink).sendInput(nextSequence(), event.button,
                                       InputEventCode::PhysicalPress, 0);
    // Start the codec while deciding between a short press and PTT. Audio is
    // kept locally as preroll and is sent only after LongPress, so a normal
    // short press retains its existing meaning without clipping initial speech.
    if (captureMode == CaptureMode::PushToTalk &&
        !captureMonitoring &&
        (currentState == DeviceState::Idle ||
         currentState == DeviceState::Listening))
      startCapture(false);
    requestRender();
    return;
  }
  if (event.action == ButtonAction::LongPress) {
    const bool wasAmbient = ambientAtKeyPress;
    ambientAtKeyPress = false;
    if (wasAmbient)
      noteMeaningfulActivity();
    if (currentState == DeviceState::Offline) {
      scene.updateState(DeviceState::Connecting);
      transportFor(activeLink).sendInput(nextSequence(), event.button,
                                         InputEventCode::Reconnect,
                                         event.durationMs);
    } else if (captureMode != CaptureMode::Disabled) {
      if (currentState == DeviceState::Speaking ||
          currentState == DeviceState::Thinking) {
        audio.stopPlayback();
        stopCapture(false);
        sendActive(charadock::protocol::FrameType::Interrupt);
      }
      if (captureMode == CaptureMode::HandsFree) {
        if (!captureMonitoring)
          startCapture(false);
        if (captureMonitoring) {
          forcedHandsFreeRecording = true;
          beginDetectedUtterance();
        }
      } else {
        if (!captureMonitoring)
          startCapture(false);
        if (captureMonitoring)
          beginDetectedUtterance();
      }
      transportFor(activeLink).sendInput(nextSequence(), event.button,
                                         InputEventCode::PttStart,
                                         event.durationMs);
    }
    requestRender();
    return;
  }
  if (event.action == ButtonAction::LongRelease) {
    if (recording && (captureMode == CaptureMode::PushToTalk ||
                      forcedHandsFreeRecording)) {
      stopCapture(true);
      scene.updateState(DeviceState::Thinking);
      transportFor(activeLink).sendInput(nextSequence(), event.button,
                                         InputEventCode::PttEnd,
                                         event.durationMs);
    }
    requestRender();
    return;
  }
  if (event.action != ButtonAction::ShortPress)
    return;
  if (captureMode == CaptureMode::PushToTalk && captureMonitoring &&
      !recording) {
    stopCapture(false);
    if (scene.active().state == DeviceState::Listening)
      scene.updateState(DeviceState::Idle);
    // Press-time microphone preroll temporarily changes the scene to
    // Listening. Use the settled state for the short-press action instead of
    // falling through to the legacy CHAT/WORK toggle with a stale value.
    currentState = scene.active().state;
  }
  if (currentState == DeviceState::Thinking ||
      currentState == DeviceState::Speaking ||
      currentState == DeviceState::Working) {
    audio.stopPlayback();
    stopCapture(false);
    sendActive(charadock::protocol::FrameType::Interrupt);
    scene.updateState(DeviceState::Idle);
  } else if (currentState == DeviceState::Offline ||
             currentState == DeviceState::Connecting) {
    requestRender();
    return;
  } else {
    if (ambientAtKeyPress) {
      ambientAtKeyPress = false;
      noteMeaningfulActivity();
      // Ambient is a device-local view. Do not send the legacy overview
      // event: the host would answer with a CHAT/WORK scene sync and replace
      // the clock dashboard a few seconds later.
      requestRender();
      return;
    }
    // Any settled character scene may enter the clock dashboard. Previously
    // this was limited to Home, so an idle Chat/Conversation snapshot made a
    // short KEY press appear to do nothing useful.
    if (ambientPolicy(currentState, scene.active().scene, captureMonitoring,
                      recording, audio.active()) == AmbientPolicy::IdleTimeout) {
      ambientMode = true;
      requestRender();
      return;
    }
    SceneSnapshot local = scene.active();
    if (local.scene == SceneId::Work) {
      local.scene = SceneId::Home;
      local.modeLabel = "CHAT";
      local.activity.clear();
      local.nextAction.clear();
    } else {
      local.scene = SceneId::Work;
      local.modeLabel = "WORK";
      if (local.activity.empty())
        local.activity = "現在の作業はありません";
    }
    scene.setLocalScene(std::move(local));
    transportFor(activeLink).sendInput(nextSequence(), event.button,
                                       InputEventCode::ToggleOverview,
                                       event.durationMs);
  }
  requestRender();
}

void handleBootEvent(const ButtonEvent &event) {
  if (event.action != ButtonAction::LongPress)
    return;
  char diagnostic[200] = {};
  const auto &status = sensors.snapshot();
  std::snprintf(diagnostic, sizeof(diagnostic),
                "DISPLAY OK\nPSRAM %s\nRTC %s  SHTC3 %s\nES8311 %s  ES7210 %s\nMIC %s  WIFI %s",
                psramFound() ? "OK" : "ERROR",
                status.rtcAvailable ? "OK" : "--",
                status.shtc3Available ? "OK" : "--",
                status.es8311Available ? "OK" : "--",
                status.es7210Available ? "OK" : "--",
                microphone.available() ? "OK" : "--",
                wifi.networkConnected() ? "OK" : "--");
  SceneSnapshot snapshot = recoveryScene(diagnostic);
  snapshot.modeLabel = "DIAGNOSTIC";
  snapshot.state = DeviceState::Idle;
  scene.setLocalScene(std::move(snapshot));
  transportFor(activeLink).sendInput(nextSequence(), event.button,
                                     InputEventCode::Diagnostic,
                                     event.durationMs);
  requestRender();
}

FrameApplyOutcome appliedOutcome() {
  FrameApplyOutcome outcome;
  outcome.result = FrameApplyResult::Applied;
  return outcome;
}

FrameApplyOutcome invalidOutcome() {
  FrameApplyOutcome outcome;
  outcome.result = FrameApplyResult::InvalidPayload;
  return outcome;
}

void handleProtocolFrame(const charadock::protocol::Frame &frame, Link link) {
  const uint32_t now = millis();
  if (link == Link::Wifi)
    wifiWatchdog.noteActivity(now);
  else
    usbWatchdog.noteActivity(now);

  UsbTransport &transport = transportFor(link);
  if (frame.type == charadock::protocol::FrameType::HostHello) {
    if (link == Link::Wifi) {
      // The first HostHello completes the mutual HMAC handshake.  Later
      // empty HostHello frames are authenticated heartbeats on the same TCP
      // stream and must not tear down an otherwise healthy Wi-Fi session.
      const bool alreadyAuthenticated = wifi.hostAuthenticated();
      if ((!alreadyAuthenticated &&
           !wifi.markHostAuthenticated(frame.payload.data(),
                                       frame.payload.size())) ||
          (alreadyAuthenticated && !frame.payload.empty())) {
        transport.respond(frame, invalidOutcome());
        wifi.resetHost();
        return;
      }
    } else {
      usbHostConnected = true;
    }
    selectBestLink();
    transport.respond(frame, appliedOutcome());
    transport.sendDeviceHello(nextSequence(), deviceId);
    if (link == activeLink && !frame.payload.empty() && !recording && !audio.active()) {
      scene.updateState(DeviceState::Idle);
      scheduleHandsFreeResume();
      requestRender();
    }
    return;
  }
  if (frame.type == charadock::protocol::FrameType::AuthChallenge) {
    uint8_t proof[32] = {};
    if (link == Link::Wifi &&
        wifi.createAuthenticationProof(frame.payload.data(),
                                       frame.payload.size(), proof))
      transport.send(charadock::protocol::FrameType::DeviceAuth,
                     frame.sequence, proof, sizeof(proof));
    return;
  }
  // DeviceHello and Capabilities are announced when the outbound socket
  // opens so the configured PC can issue its challenge.  No other network
  // request is accepted until that PC also proves possession of the pairing
  // secret.  USB remains the only provisioning path.
  if (link == Link::Wifi && !wifi.hostAuthenticated()) {
    transport.respond(frame, invalidOutcome());
    wifi.resetHost();
    return;
  }
  if (frame.type == charadock::protocol::FrameType::WifiConfig) {
    if (link != Link::Usb) {
      transport.respond(frame, invalidOutcome());
      return;
    }
    const char *error = "";
    const bool accepted =
        wifi.provision(frame.payload.data(), frame.payload.size(), error);
    transport.respond(frame, accepted ? appliedOutcome() : invalidOutcome());
    wifi.sendStatus(usbTransport, nextSequence(),
                    accepted ? "connecting" : "error", error);
    return;
  }
  if (frame.type == charadock::protocol::FrameType::WifiStatus &&
      frame.payload.empty()) {
    wifi.sendStatus(transport, frame.sequence,
                    wifi.networkConnected() ? "connected"
                                            : (wifi.configured() ? "connecting"
                                                                 : "setup-required"));
    return;
  }
  if (frame.type == charadock::protocol::FrameType::Capabilities &&
      frame.payload.empty()) {
    transport.sendCapabilities(frame.sequence, deviceId);
    return;
  }
  if (frame.type == charadock::protocol::FrameType::DeviceHello &&
      frame.payload.empty()) {
    transport.sendDeviceHello(frame.sequence, deviceId);
    return;
  }
  if (frame.type == charadock::protocol::FrameType::SensorReport &&
      frame.payload.empty()) {
    transport.sendSensors(frame.sequence, sensors.snapshot());
    return;
  }
  if (frame.type == charadock::protocol::FrameType::CaptureConfig) {
    if (frame.payload.size() != 3 ||
        frame.payload[0] > static_cast<uint8_t>(CaptureMode::Disabled)) {
      transport.respond(frame, invalidOutcome());
      return;
    }
    // The authenticated standby link may configure during its handshake,
    // but must not stop or reconfigure the active microphone.
    if (link != activeLink) {
      transport.respond(frame, appliedOutcome());
      return;
    }
    stopCapture(recording);
    captureMode = static_cast<CaptureMode>(frame.payload[0]);
    const uint16_t requested = static_cast<uint16_t>(frame.payload[1]) |
                               static_cast<uint16_t>(frame.payload[2]) << 8;
    vadMinimumStart = std::max<uint16_t>(
        kVadMinimumThreshold, std::min(kVadMaximumThreshold, requested));
    transport.respond(frame, appliedOutcome());
    if (captureMode == CaptureMode::HandsFree)
      scheduleHandsFreeResume();
    return;
  }

  if (link != activeLink) {
    transport.respond(frame, FrameApplyOutcome{});
    return;
  }
  if (frame.type == charadock::protocol::FrameType::AudioBegin)
    stopCapture(recording);

  const FrameApplyOutcome outcome = dispatcher.apply(frame);
  transport.respond(frame, outcome);
  // PC sends Listening only after its speech gate admits the utterance.
  // Local energy detection alone must not dismiss the clock dashboard.
  if (frame.type == charadock::protocol::FrameType::State &&
      scene.active().state == DeviceState::Listening) {
    pendingVoiceCandidate = false;
    noteMeaningfulActivity(now);
  }
  if (frame.type == charadock::protocol::FrameType::AudioBegin) {
    noteMeaningfulActivity(now);
  } else if (outcome.displayChanged) {
    // Heartbeats, artwork refreshes and passive listening are not speech.
    if (ambientPolicy(scene.active().state, scene.active().scene,
                      captureMonitoring, recording, audio.active(), pendingVoiceCandidate) ==
        AmbientPolicy::Portrait)
      noteMeaningfulActivity(now);
  }
  if (frame.type == charadock::protocol::FrameType::AudioStop)
    scheduleHandsFreeResume();
  if (frame.type == charadock::protocol::FrameType::State &&
      scene.active().state == DeviceState::Idle)
    scheduleHandsFreeResume();
  if (outcome.displayChanged || scene.dirty())
    requestRender();
}

void allocatePortraitSlots() {
  for (uint8_t index = 0; index < 8; ++index) {
    portraitSlots[index] = static_cast<uint8_t *>(heap_caps_malloc(
        MonochromeAssetStore::kMaximumBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  const size_t bytes = MonochromeAssetStore::kMaximumBytes;
  if (!portrait.attach(portraitSlots[0], portraitSlots[1], bytes) ||
      !portraitBlink.attach(portraitSlots[2], portraitSlots[3], bytes) ||
      !portraitMouthHalf.attach(portraitSlots[4], portraitSlots[5], bytes) ||
      !portraitMouthOpen.attach(portraitSlots[6], portraitSlots[7], bytes))
    Serial.println("# portrait PSRAM allocation failed");
}

void buildDeviceId() {
  const uint64_t mac = ESP.getEfuseMac();
  std::snprintf(deviceId, sizeof(deviceId), "cd-rlcd-%012llx",
                static_cast<unsigned long long>(mac & 0xffffffffffffULL));
}

void handleConnectionTimeouts(uint32_t now) {
  if (usbWatchdog.consumeTimeout(now))
    usbHostConnected = false;
  if (wifiWatchdog.consumeTimeout(now))
    wifi.resetHost();
  selectBestLink();
  if (!anyHostConnected() && scene.active().state != DeviceState::Offline) {
    stopCapture(false);
    audio.stopPlayback();
    scene.setLocalScene(offlineSnapshot(scene.active()));
    requestRender();
    Serial.println("# host heartbeat timed out; showing offline snapshot");
  }
}

} // namespace

void setup() {
  // A 4 KiB PCM frame can arrive while the reflective LCD is flushing, so
  // reserve two complete frames before USB starts.
  Serial.setRxBufferSize(8192 + 2 * charadock::protocol::kHeaderBytes);
  Serial.begin(500000);
  // An unplugged/unread USB endpoint must not stall Wi-Fi or microphone I/O.
  Serial.setTxTimeoutMs(10);
  Serial.setTimeout(1);
  delay(300);
  buildDeviceId();
  pinMode(pins::kKey, INPUT_PULLUP);
  pinMode(pins::kBoot, INPUT_PULLUP);

  // PA_EN is forced LOW before probing slower peripherals.
  const bool audioReady = audio.begin();
  sensors.begin();
  const bool microphoneReady = microphone.begin();
  allocatePortraitSlots();
  const bool displayReady = display.begin(U8G2_R1);
  const bool fontsReady = renderer.attachFonts(embeddedFontAssets());
  scene.setLocalScene(recoveryScene());
  wifi.begin(deviceId);
  lastWifiNetworkConnected = wifi.networkConnected();

  Serial.printf("# CharaDock RLCD %s display=%s fonts=%s audio=%s mic=%s psram=%s id=%s\r\n",
                CHARADOCK_RLCD_FIRMWARE_VERSION,
                displayReady ? "ok" : "error", fontsReady ? "ok" : "error",
                audioReady ? "ok" : "error",
                microphoneReady ? "ok" : "error",
                psramFound() ? "ok" : "error", deviceId);
  usbTransport.sendDeviceHello(nextSequence(), deviceId);
  usbTransport.sendCapabilities(nextSequence(), deviceId);
  usbTransport.sendSensors(nextSequence(), sensors.snapshot());
  wifi.sendStatus(usbTransport, nextSequence(),
                  wifi.configured() ? "connecting" : "setup-required");
  nextSensorReportAtMs = millis() + 60000;
  lastMeaningfulActivityAtMs = millis();
  renderIfNeeded();
}

void loop() {
  const uint32_t now = millis();
  wifi.update(now);
  if (wifi.takeSocketOpened() || wifi.shouldSendHello(now)) {
    wifi.transport().sendDeviceHello(nextSequence(), deviceId);
    wifi.transport().sendCapabilities(nextSequence(), deviceId);
  }

  for (const auto &frame : usbTransport.poll())
    handleProtocolFrame(frame, Link::Usb);
  for (const auto &frame : wifi.poll())
    handleProtocolFrame(frame, Link::Wifi);

  const bool wifiNetworkConnected = wifi.networkConnected();
  if (wifiNetworkConnected != lastWifiNetworkConnected) {
    lastWifiNetworkConnected = wifiNetworkConnected;
    if (usbHostConnected) {
      wifi.sendStatus(usbTransport, nextSequence(),
                      wifiNetworkConnected ? "connected" : "connecting",
                      wifiNetworkConnected ? "" : "Wi-Fi connection lost");
    }
  }

  const AudioPlaybackEvent audioEvent = audio.pollEvent();
  if (audioEvent != AudioPlaybackEvent::None) {
    scene.updateState(audioEvent == AudioPlaybackEvent::Completed
                          ? DeviceState::Idle
                          : DeviceState::Error);
    if (audioEvent == AudioPlaybackEvent::Completed)
      scheduleHandsFreeResume();
    requestRender();
  }

  const auto &activeScene = scene.active();
  const bool ambientKeyPending = ambientMode && ambientAtKeyPress &&
      keyButton.pressed() && !recording;
  const auto policy = ambientPolicy(activeScene.state, activeScene.scene,
                                   captureMonitoring, recording, audio.active(), pendingVoiceCandidate);
  if (!ambientKeyPending) {
    if (policy == AmbientPolicy::Portrait) {
      noteMeaningfulActivity(now);
    } else if (!ambientMode && (policy == AmbientPolicy::Waiting ||
               ambientIdleExpired(now, lastMeaningfulActivityAtMs))) {
      ambientMode = true;
      requestRender();
    }
  }

  for (const auto &event : keyButton.update(digitalRead(pins::kKey) == LOW, now))
    handleKeyEvent(event);
  for (const auto &event :
       bootButton.update(digitalRead(pins::kBoot) == LOW, now))
    handleBootEvent(event);

  ensureHandsFreeMonitoring(now);
  streamMicrophone();
  updatePortraitAnimation(millis());
  updateIdlePortraitMotion(millis());

  if (sensors.update(now))
    requestRender();
  if (static_cast<int32_t>(now - nextSensorReportAtMs) >= 0) {
    if (anyHostConnected())
      transportFor(activeLink).sendSensors(nextSequence(), sensors.snapshot());
    nextSensorReportAtMs = now + 60000;
  }
  // Frame handling and codec setup can advance millis() after `now` was
  // sampled at the top of the loop.  Passing that stale value to wrap-safe
  // subtraction can look like a full 32-bit timeout immediately after a
  // freshly received frame.
  handleConnectionTimeouts(millis());
  renderIfNeeded();
  delay(1);
}
